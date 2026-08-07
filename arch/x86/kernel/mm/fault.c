#include <arch/linkage.h>
#include <arch/x86/ptrace.h>
#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <mm/mm.h>
#include <mm/mmzone.h>
#include <stddef.h>
#include <stdint.h>
#include <printk.h>
#include <libs/memcpy.h>

/*
 * 缺页异常处理体系（两级页表：PGD → PTE）
 *
 * error_code 位定义：
 *   bit 0: 页是否存在（0=不存在, 1=存在但权限不足）
 *   bit 1: 访问类型（0=读/执行, 1=写）
 *   bit 2: 特权级（0=内核态, 1=用户态）
 *   bit 3: 保留位违规
 *   bit 4: 取指引发
 *
 * 处理策略：
 *   PTE=0           → Demand Paging，分配物理页建立映射
 *   PTE存在但只读写 → COW（写时复制）
 *   其他            → 非法访问，内核 panic

	CPU触发 #PF (int 14)
      ↓
entry.S: page_fault 桩
  (CPU已自动压入error_code，不额外压，直接跳 interrupt_warpper)
      ↓
do_page_fault(regs, error_code)
  ├── 读CR2 → 异常地址
  ├── 快速校验: NULL指针/EIP有效性
  ├── pgd_offset → 定位PGD条目
  │
  ├── [内核地址 >= PAGE_OFFSET]
  │     ├── PGD为空 → handle_pde_fault (分配PTE页表页)
  │     │                → handle_pte_fault
  │     └── PGD存在 → pte_offset → handle_pte_fault
  │
  └── [用户地址 < PAGE_OFFSET]
        ├── addr < 4K → SEGFAULT(NULL保护页)
        ├── PGD为空 → SEGFAULT(未分配区域)
        └── PGD存在 → pte_offset → handle_pte_fault

handle_pte_fault 三种情况：
  1. PTE=0 (PAGE_NONE) → Demand Paging
     alloc_pages → memset清零 → set_pte(PAGE_KERNEL) → invlpg
  2. PTE存在 + 只读 + 写访问 → COW
     alloc新页 → memcpy旧页内容 → set_pte(新页+可写) → invlpg
  3. PTE存在 + 内核态异常 → BUG → do_page_fault_panic

 */

/* 刷新整个 TLB（重载 CR3） */
static inline void __flush_tlb_all(void)
{
    unsigned long cr3;
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("movl %0, %%cr3" :: "r"(cr3) : "memory");
}

/* 刷新单条 TLB 条目 */
static inline void __flush_tlb_one(unsigned long addr)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

/*
 * 按需分配一个页表页（1024个PTE条目，4KB），清零后返回内核虚拟地址
 * 供 PGD 缺失时调用
 */
static pte_t *pte_alloc_one(void)
{
    struct page *page = __alloc_pages(0, 0);
    if (!page)
        return NULL;

    unsigned long pfn = (unsigned long)(page - mem_map);
    pte_t *pte = (pte_t *)__va(pfn << PAGE_SHIFT);
    memset(pte, 0, PAGE_SIZE);
    return pte;
}

/*
 * PTE 级缺页处理
 *   pte         : 发生异常的 PTE 指针
 *   address     : 触发异常的线性地址
 *   error_code  : CPU 压入的错误码
 *
 * 返回值: 0=成功处理, -1=无法处理
 */
static int handle_pte_fault(pte_t *pte, unsigned long address, unsigned long error_code)
{
    int write_access = error_code & 0x02;

    /* ---- Case 1: PTE 条目为空（未建立任何映射） → Demand Paging ---- */
    if (pte_val(*pte) == pte_val(PAGE_NONE)) {
        struct page *new_page = __alloc_pages(0, 0);
        if (!new_page) {
            printk("[OOM] Cannot allocate page for address %#lx\n", address);
            return -1;
        }
        /* 清零新页，避免泄露内核数据 */
        unsigned long pfn  = (unsigned long)(new_page - mem_map);
        void *vaddr        = __va(pfn << PAGE_SHIFT);
        memset(vaddr, 0, PAGE_SIZE);

        /* 建立映射：物理页帧 + 可读可写内核属性 */
        set_pte(pte, __mk_pte(pfn, PAGE_KERNEL));
        __flush_tlb_one(address);
        return 0;
    }

    /* ---- Case 2: PTE 存在但写保护，写访问触发 → COW ---- */
    if (pte_present(*pte) && write_access && !pte_write(*pte)) {
        unsigned long old_phys = pte_val(*pte) & PAGE_MASK;

        /* 分配新物理页 */
        struct page *new_page = __alloc_pages(0, 0);
        if (!new_page) {
            printk("[OOM] COW: Cannot allocate page for address %#lx\n", address);
            return -1;
        }
        unsigned long new_pfn   = (unsigned long)(new_page - mem_map);
        unsigned long new_phys  = new_pfn << PAGE_SHIFT;
        void *new_vaddr         = __va(new_phys);

        /* 复制旧页内容到新页 */
        void *old_vaddr = __va(old_phys);
        memcpy(new_vaddr, old_vaddr, PAGE_SIZE);

        /* 用新物理地址 + 可写权限替换原 PTE */
        set_pte(pte, __mk_pte(new_pfn, PAGE_KERNEL));
        __flush_tlb_one(address);
        return 0;
    }

    /* ---- Case 3: PTE 存在，内核态写保护违规 → BUG ---- */
    if (pte_present(*pte) && !(error_code & 0x04)) {
        printk("[BUG] Kernel write fault on present page, addr=%#lx, pte=%#lx\n",
               address, pte_val(*pte));
        return -1;
    }

    return -1;
}

/*
 * PGD 级缺页处理
 *   若 PDE 为空，先分配 PTE 页表页并安装
 *   再转入 handle_pte_fault() 处理具体 PTE
 */
static int handle_pde_fault(pgd_t *pgd, unsigned long address, unsigned long error_code)
{
    if (pgd_none(*pgd)) {
        pte_t *pte_base = pte_alloc_one();
        if (!pte_base) {
            printk("[OOM] Cannot allocate PTE table for address %#lx\n", address);
            return -1;
        }
        /* 安装 PDE：PTE 页表的物理地址 + 内核页表属性 */
        set_pgd(pgd, __pgd(__pa(pte_base) + _KERNPG_TABLE));
    }

    pte_t *pte = pte_offset(pgd, address);
    return handle_pte_fault(pte, address, error_code);
}

/*
 * 缺页异常主入口（由 entry.S page_fault 桩调用）
 *
 * 执行流程：
 *   1. 读取 CR2 获取触发异常的线性地址
 *   2. 合法性校验（NULL指针、EIP有效性）
 *   3. 定位 PGD 条目，分派到 handle_pde_fault / handle_pte_fault
 *   4. 无法处理时进入 do_page_fault_panic 打印现场并停机
 */
static void do_page_fault_panic(struct pt_regs *regs, unsigned long error_code,
                                unsigned long address);
asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
    unsigned long address;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(address) :: "memory");

    /* 空指针解引用快速路径 */
    if (address == 0) {
        printk("[BUG] NULL pointer dereference at EIP=%#lx\n", regs->eip);
        goto fault_panic;
    }

    /* EIP 本身在无效地址 → 严重内核错误 */
    if (regs->eip == 0 || regs->eip == 0xffffffff) {
        printk("[BUG] Invalid EIP=%#lx during page fault, addr=%#lx\n",
               regs->eip, address);
        goto fault_panic;
    }

    /* 查找地址所属的 PGD 条目（swapper_pg_dir 为内核全局页目录） */
    pgd_t *pgd = pgd_offset(swapper_pg_dir, address);

    /* ---- 内核空间地址（>= PAGE_OFFSET）---- */
    if (address >= PAGE_OFFSET) {
        if (pgd_none(*pgd)) {
            /* 内核按需映射：为缺失的 PDE 分配 PTE 页表 */
            if (handle_pde_fault(pgd, address, error_code) == 0)
                return;
            goto fault_panic;
        }
        /* PDE 已存在，直接处理 PTE 层 */
        pte_t *pte = pte_offset(pgd, address);
        if (handle_pte_fault(pte, address, error_code) == 0)
            return;
        goto fault_panic;
    }

    /* ---- 用户空间地址（< PAGE_OFFSET）---- */

    /* 用户空间低地址 NULL 保护页（0 ~ 4K） */
    if (address < PAGE_SIZE) {
        printk("[SEGFAULT] User NULL dereference, addr=%#lx, EIP=%#lx\n",
               address, regs->eip);
        goto fault_panic;
    }

    /* PGD 不存在 → 用户访问了未分配的内存区域 */
    if (pgd_none(*pgd)) {
        printk("[SEGFAULT] User access to unallocated area, addr=%#lx, EIP=%#lx\n",
               address, regs->eip);
        goto fault_panic;
    }

    /* PDE 存在，处理 PTE */
    pte_t *pte = pte_offset(pgd, address);
    if (handle_pte_fault(pte, address, error_code) == 0)
        return;

fault_panic:
    do_page_fault_panic(regs, error_code, address);
}

/*
 * 无法处理的缺页异常：打印完整现场后停机
 */
static void do_page_fault_panic(struct pt_regs *regs, unsigned long error_code,
                                unsigned long address)
{
    printk("\n===== PAGE FAULT PANIC =====\n");
    printk("Address (CR2): %#010lx\n", address);
    printk("Error Code : %#010lx  [", error_code);

    if (!(error_code & 0x01)) printk("Not-Present ");
    else                      printk("Protection ");

    if (error_code & 0x02) printk("Write ");
    else                   printk("Read ");

    if (error_code & 0x04) printk("User ");
    else                   printk("Supervisor ");

    if (error_code & 0x08) printk("Reserved-Bit ");
    if (error_code & 0x10) printk("Instruction-Fetch ");
    printk("]\n");

    printk("EIP  : %#010lx\n", regs->eip);
    printk("CS   : %#06x\n",  (unsigned)(regs->cs & 0xffff));
    printk("EFLAGS: %#010lx\n", regs->eflags);
    printk("ESP  : %#010lx\n", regs->esp);
    printk("EAX  : %#010lx  EBX: %#010lx\n", regs->eax, regs->ebx);
    printk("ECX  : %#010lx  EDX: %#010lx\n", regs->ecx, regs->edx);
    printk("ESI  : %#010lx  EDI: %#010lx\n", regs->esi, regs->edi);
    printk("EBP  : %#010lx\n", regs->ebp);
    printk("============================\n");

    /* 停机，防止进一步破坏现场 */
    while (1) {
        __asm__ __volatile__("cli; hlt");
    }
}