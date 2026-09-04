/*
 * LulaOS ioremap / vmalloc 实现
 *
 * 参考 arch/i386/mm/ioremap.c, lib/ioremap.c, mm/vmalloc.c（Linux 2.6.20）
 *
 * 功能：
 *   ioremap  : 将任意物理地址范围映射到内核虚拟地址空间，供 MMIO 访问。
 *   vmalloc  : 分配物理不连续但虚拟连续的内存块（普通内核内存）。
 *
 * 虚拟地址空间布局：
 *   0xC0000000 ~ high_memory        : 直接映射区（0~896MB，已建立）
 *   FB_IOREMAP_BASE ~ fb_ioremap_end : fb_ioremap 专用映射区（fbcon_init 使用）
 *   fb_ioremap_end(4MB对齐) ~ 0xFE000000 : ioremap/vmalloc 共享区（本文件使用）
 *   0xFE000000 ~ 0xFE3FFFFF         : PKMAP 永久映射区
 *   0xFFC00000 ~ 0xFFFFE000         : Fixmap 固定映射区
 *
 * vmalloc 起始地址由 vmalloc_area_init() 在 fbcon_init() 完成后
 * 动态设定，从 fb_ioremap 映射结束位置按 4MB（PGDIR）对齐开始，
 * 避免与 framebuffer 映射地址冲突。
 *
 * 两级页表（PGD → PTE）：
 *   - 若目标 PGD 条目为空，分配一页作为 PTE 表并安装
 *   - 若 PGD 已存在（不应发生在 vmalloc 区），直接定位 PTE
 *   - 每个 PTE 条目填入物理页帧号 + 保护属性
 *
 * 初始化时机：须在 slab 分配器就绪后调用（需要 kmalloc 分配 vm_struct）
 */

#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/highmem.h>
#include <mm/mm.h>
#include <mm/mmzone.h>
#include <mm/slab.h>
#include <printk.h>
#include <libs/memcpy.h>
#include <stddef.h>

/* ======================== 常量 ======================== */

#define VMALLOC_END     0xFE000000UL   /* PKMAP 之前 */

/* fb.c 导出的 fb_ioremap 分配游标（映射结束后的下一个虚拟地址） */
extern unsigned long ioremap_next_vaddr;

/* vm_struct.flags 取值 */
#define VM_IOREMAP  0x00000001   /* ioremap 映射（不释放物理页） */
#define VM_ALLOC    0x00000002   /* vmalloc 分配（vfree 时释放物理页） */

/* ======================== 数据结构 ======================== */

/*
 * vm_struct - 已分配的虚拟内存区域描述
 *
 * 参考 Linux struct vm_struct（简化版），同时服务 ioremap 和 vmalloc。
 * 通过链表组织，供 iounmap / vfree 查找并释放。
 *
 * flags 字段区分区域来源：
 *   VM_IOREMAP : ioremap 建立，iounmap 只清除 PTE，不释放物理页
 *   VM_ALLOC   : vmalloc 分配，vfree 需从 PTE 读 PFN 并归还伙伴系统
 */
struct vm_struct {
    void            *addr;        /* 映射起始虚拟地址 */
    unsigned long    size;        /* 映射大小（页对齐后） */
    unsigned long    phys_addr;   /* ioremap: 原始物理地址（vmalloc: 0） */
    unsigned long    flags;       /* VM_IOREMAP / VM_ALLOC */
    struct vm_struct *next;
};

/* vmalloc 区域链表头及分配游标（由 vmalloc_area_init 动态初始化） */
static struct vm_struct *vmlist = NULL;
static unsigned long vmalloc_next = 0;

/* ======================== 虚拟地址区域分配 ======================== */

/*
 * vmalloc_area_init - 初始化 vmalloc 起始地址
 *
 * 在 fbcon_init() 完成后调用。读取 fb.c 中 fb_ioremap 的分配游标
 * ioremap_next_vaddr（即 framebuffer 映射结束后的下一个虚拟地址），
 * 按 4MB（PGDIR_SIZE）向上对齐，作为 vmalloc/ioremap 的起始地址。
 *
 * 若 fb 未激活（ioremap_next_vaddr 仍为 FB_IOREMAP_BASE），
 * vmalloc 直接从 FB_IOREMAP_BASE 开始，与旧逻辑兼容。
 *
 * 调用时机：setup_arch() 中，fbcon_init() 之后、slab 初始化之前。
 */
void vmalloc_area_init(void)
{
    unsigned long fb_end = ioremap_next_vaddr;

    /* 按 4MB（PGDIR_SIZE）向上对齐 */
    vmalloc_next = (fb_end + PGDIR_SIZE - 1) & PGDIR_MASK;

    printk("vmalloc: start=0x%lx (fb_ioremap end=0x%lx, aligned to 4MB)\n",
           vmalloc_next, fb_end);
}

/*
 * get_vm_area - 在 vmalloc 区分配连续虚拟地址段
 *
 * 采用 bump allocator（线性分配），每次从 vmalloc_next 推进。
 * 分配段末尾留一页 guard page（不映射），用于捕获越界访问。
 *
 * 返回: vm_struct 指针，NULL 表示空间不足或 kmalloc 失败
 */
static struct vm_struct *get_vm_area(unsigned long size)
{
    unsigned long total, start;
    struct vm_struct *area;

    /* 安全检查：vmalloc_area_init() 必须已被调用 */
    if (!vmalloc_next) {
        printk("get_vm_area: vmalloc area not initialized (call vmalloc_area_init first)\n");
        return NULL;
    }

    /* 加一页 guard page */
    total = PAGE_ALIGN(size) + PAGE_SIZE;
    start = vmalloc_next;

    if (start + total > VMALLOC_END || start + total < start) {
        printk("ioremap: vmalloc area exhausted\n");
        return NULL;
    }

    area = kmalloc(sizeof(*area), GFP_KERNEL);
    if (!area)
        return NULL;

    area->addr      = (void *)start;
    area->size      = PAGE_ALIGN(size);
    area->phys_addr = 0;
    area->flags     = 0;
    area->next      = vmlist;
    vmlist          = area;

    vmalloc_next = start + total;
    return area;
}

/* ======================== 页表映射 ======================== */

/*
 * ioremap_pte_range - 填充 PTE 条目，建立物理→虚拟映射
 *
 * 若 PTE 所在页表尚未分配（PGD 为空），先分配一页清零后安装到 PGD。
 * 然后逐页填入物理页帧号 + 保护属性。
 */
static int ioremap_pte_range(pgd_t *pgd, unsigned long addr,
                              unsigned long end, unsigned long phys_addr,
                              pgprot_t prot)
{
    pte_t *pte;

    /* 若 PGD 条目为空，分配一页作为 PTE 表 */
    if (pgd_none(*pgd)) {
        struct page *ptepage = __alloc_pages(0, 0);
        if (!ptepage) {
            printk("ioremap: cannot allocate PTE page for addr=%#lx\n", addr);
            return -1;
        }
        unsigned long pfn  = (unsigned long)(ptepage - mem_map);
        pte_t *pte_base    = (pte_t *)__va(pfn << PAGE_SHIFT);
        memset(pte_base, 0, PAGE_SIZE);
        set_pgd(pgd, __pgd(__pa(pte_base) + _KERNPG_TABLE));
    }

    /* 定位 PTE 并逐页填入映射（与 fb_ioremap 保持一致：物理地址 | 保护位） */
    pte = pte_offset(pgd, addr);
    do {
        set_pte(pte, __pte(phys_addr | pgprot_val(prot)));
        phys_addr += PAGE_SIZE;
    } while (pte++, addr += PAGE_SIZE, addr < end);

    return 0;
}

/*
 * ioremap_page_range - 在 swapper_pg_dir 中建立虚拟→物理映射
 *
 * 遍历 [addr, end) 的每个 4MB PGD 块，调用 ioremap_pte_range()。
 * 完成后刷新 TLB。
 *
 * 参考 lib/ioremap.c ioremap_page_range()（两级页表简化版）
 */
static int ioremap_page_range(unsigned long addr, unsigned long end,
                               unsigned long phys_addr, pgprot_t prot)
{
    pgd_t *pgd;
    unsigned long next;

    pgd = swapper_pg_dir + pgd_index(addr);
    do {
        /* 当前 PGD 块的结束地址 */
        next = (addr + PGDIR_SIZE) & PGDIR_MASK;
        if (next > end || next < addr)
            next = end;

        if (ioremap_pte_range(pgd, addr, next, phys_addr, prot))
            return -1;

        phys_addr += (next - addr);
        addr = next;
    } while (pgd++, addr < end);

    /* 刷新 TLB，确保所有 CPU 看到新映射 */
    __flush_tlb_all();

    return 0;
}

/* ======================== 公共 API ======================== */

/*
 * ioremap - 将物理地址映射到内核虚拟地址空间
 *
 * 参考 arch/i386/mm/ioremap.c __ioremap()
 *
 * phys_addr: 物理地址（无需页对齐，内部自动处理偏移）
 * size:      映射大小（字节）
 *
 * 返回: 可直接读写的内核虚拟地址，NULL 表示失败
 *
 * 注意：
 *   - 映射默认禁用缓存（_PAGE_PCD），适合 MMIO 访问
 *   - 若物理地址在直接映射区（<896MB），直接返回 __va() 结果
 *   - 释放使用 iounmap()
 */
void *ioremap(unsigned long phys_addr, unsigned long size)
{
    unsigned long offset, last_addr;
    unsigned long aligned_phys, aligned_size;
    struct vm_struct *area;
    pgprot_t prot;

    /* ① 合法性检查 */
    if (!size)
        return NULL;
    last_addr = phys_addr + size - 1;
    if (last_addr < phys_addr)   /* 溢出 */
        return NULL;

    /* ② 物理地址在直接映射区（<896MB），无需重映射
     *
     * high_memory 是直接映射区末尾的虚拟地址（如 0xF8000000）。
     * 减去 PAGE_OFFSET 得到直接映射的物理上限（如 0x38000000 = 896MB）。
     * 此写法比 __va(phys_addr) < high_memory 安全，
     * 避免 phys_addr > 3GB 时 __va() 在 32 位下溢出回绕。
     */
    if (last_addr < (unsigned long)high_memory - PAGE_OFFSET)
        return __va(phys_addr);

    /* ③ 保存页内偏移，对齐到页边界 */
    offset       = phys_addr & ~PAGE_MASK;
    aligned_phys = phys_addr & PAGE_MASK;
    aligned_size = PAGE_ALIGN(last_addr + 1) - aligned_phys;

    /* ④ 分配虚拟地址段 */
    area = get_vm_area(aligned_size);
    if (!area)
        return NULL;
    area->phys_addr = aligned_phys;
    area->flags     = VM_IOREMAP;

    /* 保护位：直接复用 PAGE_KERNEL_NOCACHE（Present+RW+Dirty+Accessed+PCD） */
    prot = PAGE_KERNEL_NOCACHE;

    /* ⑥ 建立页表映射 */
    if (ioremap_page_range((unsigned long)area->addr,
                           (unsigned long)area->addr + aligned_size,
                           aligned_phys, prot)) {
        printk("ioremap: failed to map phys=%#lx size=%#lx\n",
               phys_addr, size);
        return NULL;
    }

    printk("ioremap: phys=%#lx size=%#lx -> virt=%p\n",
           aligned_phys, aligned_size, area->addr);

    /* ⑦ 加上原始页内偏移返回 */
    return (void *)((unsigned long)area->addr + offset);
}

/*
 * ioremap_nocache - 映射 IO 内存（禁用缓存）
 *
 * 在 LulaOS 中 ioremap 默认即为 nocache（_PAGE_PCD），直接转发。
 */
void *ioremap_nocache(unsigned long phys_addr, unsigned long size)
{
    return ioremap(phys_addr, size);
}

/*
 * iounmap - 释放 ioremap 建立的映射
 *
 * 流程：
 *   1. 在 vmlist 中查找对应的 vm_struct
 *   2. 清除对应的所有 PTE 条目（解除映射）
 *   3. 刷新 TLB
 *   4. 从链表中移除并释放 vm_struct
 *
 * 注意：PTE 页表页本身暂不回收（避免复杂性），虚拟地址也不回收（bump allocator）
 */
void iounmap(void *addr)
{
    struct vm_struct **p, *area;
    unsigned long vaddr, vaddr_end;

    if (!addr)
        return;

    /* 页对齐虚拟地址 */
    vaddr = (unsigned long)addr & PAGE_MASK;

    /* 在链表中查找 */
    for (p = &vmlist; *p; p = &(*p)->next) {
        if ((unsigned long)(*p)->addr == vaddr)
            break;
    }

    if (!*p) {
        printk("iounmap: address %p not found in vmalloc list\n", addr);
        return;
    }

    area = *p;
    if (!(area->flags & VM_IOREMAP)) {
        printk("iounmap: address %p is not an ioremap region\n", addr);
        return;
    }

    vaddr     = (unsigned long)area->addr;
    vaddr_end = vaddr + area->size;

    /* 清除所有 PTE 条目（解除映射，不释放物理页） */
    while (vaddr < vaddr_end) {
        pgd_t *pgd = swapper_pg_dir + pgd_index(vaddr);
        if (!pgd_none(*pgd)) {
            pte_t *pte = pte_offset(pgd, vaddr);
            set_pte(pte, __pte(0));
        }
        vaddr += PAGE_SIZE;
    }
    /* 全部清除后统一刷新 TLB */
    __flush_tlb_all();

    /* 从链表移除 */
    *p = area->next;
    kfree(area);
}

/* ======================== vmalloc / vfree ======================== */

/*
 * vmalloc_pte_range - 为 vmalloc 分配物理页并填入 PTE
 *
 * 与 ioremap_pte_range 的核心差异：
 *   ioremap:  外部提供 phys_addr，只建立映射
 *   vmalloc:  每页调用 __alloc_pages() 从伙伴系统取新页，填入 PTE
 *
 * 失败时已分配的页不在此函数回滚，由上层 vfree 路径负责。
 */
static int vmalloc_pte_range(pgd_t *pgd, unsigned long addr,
                              unsigned long end, pgprot_t prot)
{
    pte_t *pte;

    /* 若 PGD 条目为空，分配一页作为 PTE 表 */
    if (pgd_none(*pgd)) {
        struct page *ptepage = __alloc_pages(0, 0);
        if (!ptepage) {
            printk("vmalloc: cannot allocate PTE page for addr=%#lx\n", addr);
            return -1;
        }
        unsigned long pfn  = (unsigned long)(ptepage - mem_map);
        pte_t *pte_base    = (pte_t *)__va(pfn << PAGE_SHIFT);
        memset(pte_base, 0, PAGE_SIZE);
        set_pgd(pgd, __pgd(__pa(pte_base) + _KERNPG_TABLE));
    }

    pte = pte_offset(pgd, addr);
    do {
        struct page *page = __alloc_pages(0, 0);
        if (!page) {
            printk("vmalloc: out of memory at addr=%#lx\n", addr);
            return -1;
        }
        unsigned long pfn  = (unsigned long)(page - mem_map);
        unsigned long phys = pfn << PAGE_SHIFT;
        set_pte(pte, __pte(phys | pgprot_val(prot)));
    } while (pte++, addr += PAGE_SIZE, addr < end);

    return 0;
}

/*
 * vmalloc_page_range - 在 swapper_pg_dir 中为 vmalloc 建立映射
 *
 * 与 ioremap_page_range 的差异：不传入 phys_addr，每页由
 * vmalloc_pte_range 自行从伙伴系统分配。
 */
static int vmalloc_page_range(unsigned long addr, unsigned long end,
                               pgprot_t prot)
{
    pgd_t *pgd;
    unsigned long next;

    pgd = swapper_pg_dir + pgd_index(addr);
    do {
        next = (addr + PGDIR_SIZE) & PGDIR_MASK;
        if (next > end || next < addr)
            next = end;

        if (vmalloc_pte_range(pgd, addr, next, prot))
            return -1;

        addr = next;
    } while (pgd++, addr < end);

    /* 刷新 TLB */
    __flush_tlb_all();

    return 0;
}

/*
 * vmalloc - 分配虚拟连续、物理不连续的内核内存
 *
 * 参考 mm/vmalloc.c __vmalloc()（Linux 2.6.20）
 *
 * 每页独立从伙伴系统分配，映射到 vmalloc 区的连续虚拟地址。
 * 适合需要大块连续虚拟地址、但不要求物理连续的场景。
 *
 * size : 请求大小（字节，内部页对齐）
 * 返回 : 可直接读写的内核虚拟地址，NULL 表示失败
 * 释放 : vfree()
 *
 * 注意：分配出的内存未清零，调用方需自行初始化。
 */
void *vmalloc(unsigned long size)
{
    unsigned long aligned_size;
    struct vm_struct *area;
    pgprot_t prot;

    if (!size)
        return NULL;

    aligned_size = PAGE_ALIGN(size);

    /* 分配虚拟地址段 */
    area = get_vm_area(aligned_size);
    if (!area)
        return NULL;
    area->flags = VM_ALLOC;

    /* 普通内核内存：可缓存，直接使用 PAGE_KERNEL */
    prot = PAGE_KERNEL;

    /* 逐页分配物理页并建立映射 */
    if (vmalloc_page_range((unsigned long)area->addr,
                           (unsigned long)area->addr + aligned_size,
                           prot)) {
        printk("vmalloc: failed to allocate %lu bytes\n", size);
        /* 失败时由 vfree 路径清理已分配页 */
        vfree(area->addr);
        return NULL;
    }

    return area->addr;
}

/*
 * vfree - 释放 vmalloc 分配的内存
 *
 * 流程：
 *   1. 在 vmlist 中查找对应的 vm_struct（须为 VM_ALLOC 类型）
 *   2. 遍历 PTE，读出每页的 PFN，归还伙伴系统
 *   3. 清除 PTE 条目，刷新 TLB
 *   4. 从链表移除并释放 vm_struct
 *
 * 参考 mm/vmalloc.c vfree()（Linux 2.6.20）
 */
void vfree(void *addr)
{
    struct vm_struct **p, *area;
    unsigned long vaddr, vaddr_end;

    if (!addr)
        return;

    vaddr = (unsigned long)addr & PAGE_MASK;

    /* 在链表中查找 */
    for (p = &vmlist; *p; p = &(*p)->next) {
        if ((unsigned long)(*p)->addr == vaddr)
            break;
    }

    if (!*p) {
        printk("vfree: address %p not found in vmalloc list\n", addr);
        return;
    }

    area = *p;
    if (!(area->flags & VM_ALLOC)) {
        printk("vfree: address %p is not a vmalloc region\n", addr);
        return;
    }

    vaddr     = (unsigned long)area->addr;
    vaddr_end = vaddr + area->size;

    /* 读 PTE 获取 PFN，归还物理页，并清除 PTE */
    while (vaddr < vaddr_end) {
        pgd_t *pgd = swapper_pg_dir + pgd_index(vaddr);
        if (!pgd_none(*pgd)) {
            pte_t *pte = pte_offset(pgd, vaddr);
            if (pte_present(*pte)) {
                unsigned long pfn = pte_val(*pte) >> PAGE_SHIFT;
                __free_page(mem_map + pfn);
            }
            set_pte(pte, __pte(0));
        }
        vaddr += PAGE_SIZE;
    }
    /* 全部清除后统一刷新 TLB */
    __flush_tlb_all();

    /* 从链表移除 */
    *p = area->next;
    kfree(area);
}
