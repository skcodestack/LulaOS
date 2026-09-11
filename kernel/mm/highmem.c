/*
 * LulaOS pkmap（永久映射区）实现
 *
 * 参考 mm/highmem.c kmap()/kunmap() 
 *
 * 问题背景：
 *   ZONE_HIGHMEM（> 896MB）的物理页在开机时不被永久映射，page->virtual = NULL。
 *   buffer cache 通过 kmap(page) 获取页的有效虚拟地址，供 bh->b_data 使用。
 *
 * 地址空间布局（pkmap 区）：
 *   PKMAP_BASE = 0xfe000000（4G - 32M）
 *   LAST_PKMAP = 1024 个槽位，每槽 4KB，共 4MB
 *
 * 与 ioremap 方案的区别（旧版）：
 *   ioremap：设置 _PAGE_PCD 禁用 CPU 缓存，访问慢（MMIO 场景才需要）
 *   pkmap：  使用 PAGE_KERNEL（缓存启用），访问快，且槽位可被 kunmap 回收复用
 *
 * 设计参考  mm/highmem.c：
 *   - pkmap_count[i] = 0   : 槽位空闲
 *   - pkmap_count[i] = 1   : 无人引用（可被 flush_all_zero_pkmaps 回收）
 *   - pkmap_count[i] > 1   : 有活跃 kmap 引用（每次 kmap +1，kunmap -1）
 *
 *   last_pkmap_nr 轮转：从上次使用位置继续向后扫描，O(LAST_PKMAP) 找空闲槽位。
 *   所有 1024 个槽位耗尽时，调用 flush_all_zero_pkmaps() 清理计数为 1 的槽位后重试。
 * 
 */

#include <arch/x86/highmem.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/page.h>
#include <arch/x86/smp.h>   /* smp_processor_id */
#include <mm/mm.h>
#include <mm/mmzone.h>
#include <printk.h>
#include <stddef.h>

unsigned long highstart_pfn, highend_pfn;

pte_t *fixed_kmap_pte;
pgprot_t fixed_kmap_prot;

/*
 * pkmap_page_table - pkmap 区域对应的 PTE 表指针（1024 个 PTE 条目）
 *
 * 由 persist_area_init()（mm.c）在 paging_init() 阶段分配并初始化：
 *   pkmap_page_table = pte_offset(swapper_pg_dir + pgd_index(PKMAP_BASE), PKMAP_BASE)
 *
 * pkmap_page_table[0..1023] 对应虚拟地址 PKMAP_BASE + 0*4K ~ PKMAP_BASE + 1023*4K
 */
pte_t *pkmap_page_table;

/*
 * pkmap_count - pkmap 槽位引用计数数组（LAST_PKMAP = 1024 个）
 *
 * 语义 ：
 *   0       : 槽位空闲，PTE 为空，可分配
 *   1       : 槽位无人引用（kunmap 后留下），但 PTE 未清除（延迟清除优化）
 *   >= 2    : 有活跃 kmap 引用（kmap 时 +1，kunmap 时 -1）
 *
 * 初始状态：全 0（所有槽位空闲）
 */
static int pkmap_count[LAST_PKMAP];

/* 上次分配的槽位号（轮转游标，范围 0 ~ LAST_PKMAP-1） */
static unsigned long last_pkmap_nr;

/* ======================== fixmap ======================== */

static inline void set_pte_phys(unsigned long vaddr,
                                unsigned long phys, pgprot_t flags)
{
    pgd_t *pgd = swapper_pg_dir + pgd_index(vaddr);
    if (pgd_none(*pgd)) {
        printk("PAE BUG #00!\n");
        return;
    }
    pte_t *pte = pte_offset(pgd, vaddr);
    pgprot_t prot = __pgprot(pgprot_val(PAGE_KERNEL) | pgprot_val(flags));
    set_pte(pte, mk_pte_phys(phys, prot));
}

void __set_fixmap(enum fixed_addresses idx,
                  unsigned long phys, pgprot_t flags)
{
    unsigned long address = fix_to_virt(idx);

    if (idx >= __end_of_fixed_addresses) {
        printk("Invalid __set_fixmap\n");
        return;
    }
    set_pte_phys(address, phys, flags);
}

__init void fixed_kmap_init(void)
{
    unsigned long vaddr = fix_to_virt(FIX_KMAP_BEGIN);
    pgd_t *pgd = swapper_pg_dir + pgd_index(vaddr);
    fixed_kmap_pte  = pte_offset(pgd, vaddr);
    fixed_kmap_prot = PAGE_KERNEL;
}

/* ======================== pkmap ======================== */

/*
 * kmap_lock - pkmap 数据结构的 SMP 自旋锁
 *
 * 保护对象：
 *   - pkmap_count[]    （槽位引用计数数组）
 *   - last_pkmap_nr    （轮转游标）
 *   - pkmap_page_table[] PTE 写入/清除操作
 *
 * 必须持锁的原因（SMP 场景）：
 *   若两个 CPU 同时发现 pkmap_count[nr] == 0 并各自设置 PTE，
 *   两个不同的物理页将共用同一虚拟地址，导致静默数据损坏。
 *
 * 参考   mm/highmem.c 中的 kmap_lock（spinlock_t）
 */
static spinlock_t kmap_lock = SPIN_LOCK_UNLOCKED;

/*
 * smp_flush_tlb_pkmap - 刷新本 CPU 的 pkmap TLB 条目
 *
 * vaddr == 0  : 全量刷新（__flush_tlb_all，重载 CR3）
 * vaddr != 0  : 单页刷新（invlpg，仅刷指定地址）
 *
 * SMP 完整性说明（TODO）：
 *   当前仅刷新本地 CPU 的 TLB。在完整 SMP 实现中，kunmap 清除 PTE 后
 *   必须通过 IPI（TLB flush vector）通知所有其他在线 CPU 同步刷新，
 *   否则其他 CPU 的旧 TLB 条目可能导致静默数据损坏：
 *
 *     CPU A: kunmap(page_X, slot N) → set_pte(slot N, 0) → __flush_tlb_one
 *     CPU B: kmap(page_Y, slot N)   → set_pte(slot N, phys_Y)
 *     CPU C: 访问 slot N 虚拟地址 → TLB 命中（stale）→ 读 phys_X（旧页）！
 *
 *   Linux 2.6.20 的解决方案：kmap/kunmap 内部调用 smp_call_function()
 *   向所有其他 CPU 发送 IPI，由目标 CPU 在中断上下文中执行 __flush_tlb_all()。
 *
 *   LulaOS 后续需要：
 *     1. 在 IDT 中注册 TLB_FLUSH_VECTOR（如 0xFD）的中断处理函数
 *     2. 处理函数：__flush_tlb_all() + lapic_eoi()
 *     3. 在此函数中向除当前 CPU 外的所有在线 CPU 发送 send_ipi(apic_id, vector)
 *
 *   当前 spinlock 已防止数据竞争（两个 CPU 不会同时使用同一槽位），
 *   跨 CPU 的 TLB 一致性问题（stale 条目）暂接受为已知限制。
 */
static inline void smp_flush_tlb_pkmap(unsigned long vaddr)
{
    if (vaddr == 0)
        __flush_tlb_all();
    else
        __flush_tlb_one(vaddr);

    /*
     * TODO: SMP 完整实现 — 向其他在线 CPU 发送 TLB flush IPI
     *
     * int this_cpu = smp_processor_id();
     * for (int apic_id = 0; apic_id < 256; apic_id++) {
     *     int cpu = apicid_to_cpu[apic_id];
     *     if (cpu != this_cpu && (cpu_online_map & (1 << cpu)))
     *         send_ipi(apic_id, TLB_FLUSH_VECTOR);
     * }
     */
}

/*
 * flush_all_zero_pkmaps - 回收所有"计数为 1"（无人引用但 PTE 未清）的 pkmap 槽位
 *
 * 设计来源：Linux 2.6.20 mm/highmem.c flush_all_zero_pkmaps()
 *
 * 当 pkmap 找不到空闲槽位（pkmap_count 均 > 0）时调用。
 * 将计数恰好为 1 的槽位降级为 0（清除 PTE，标记空闲），
 * 然后统一刷新 TLB，让这些槽位可被后续 kmap 重新使用。
 *
 * 注意：此函数在 kmap_lock 持锁状态下调用，无需额外加锁。
 *
 * 返回值：1 表示至少回收了一个槽位，0 表示无可回收槽位（pkmap 真正耗尽）
 */
static int flush_all_zero_pkmaps(void)
{
    int flushed = 0;

    for (int i = 0; i < LAST_PKMAP; i++) {
        if (pkmap_count[i] != 1)
            continue;

        /* 将计数从 1 降为 0，标记槽位空闲 */
        pkmap_count[i] = 0;

        /* 清除 PTE，解除虚拟→物理映射 */
        if (pte_present(pkmap_page_table[i]))
            set_pte(&pkmap_page_table[i], __pte(0));

        flushed = 1;
    }

    /* 全量刷新本地 TLB（批量清除多条 PTE 后统一刷新，比逐条 invlpg 高效） */
    if (flushed)
        smp_flush_tlb_pkmap(0);

    return flushed;
}

/*
 * kmap - 为页面建立可访问的内核虚拟地址（pkmap 实现，SMP 安全）
 *
 * 参考 Linux 2.6.20 mm/highmem.c kmap()
 *
 * 分两路处理：
 *   ZONE_NORMAL（< 896MB）：page->virtual 已在 zone_init 时由 __va(phys) 设置，
 *                            直接返回，零成本（覆盖 LulaOS 64MB 场景的 100% 情况）。
 *                            无需持锁：直接映射地址永不变，不受其他 CPU 影响。
 *
 *   ZONE_HIGHMEM（> 896MB）：page->virtual 为 NULL，从 pkmap 分配槽位：
 *     ① 若 page->virtual 已在 pkmap 范围内（上次 kmap 的结果），直接返回
 *     ② 加 kmap_lock 自旋锁，禁止本 CPU 中断（spin_lock_irqsave）
 *     ③ 轮转扫描 pkmap_count[]，找计数为 0 的空闲槽位
 *     ④ 写入 PTE：phys | PAGE_KERNEL（缓存启用，优于 ioremap 的 NOCACHE）
 *     ⑤ smp_flush_tlb_pkmap() 刷新本 CPU TLB
 *     ⑥ 结果缓存在 page->virtual，后续 kmap 直接命中
 *     ⑦ 释放 kmap_lock
 *
 * SMP 安全性说明：
 *   - 锁保护 pkmap_count[]/last_pkmap_nr/PTE 写入，防止两 CPU 同时分配同一槽位
 *   - spin_lock_irqsave 同时禁用本 CPU 中断，防止中断处理程序嵌套访问 pkmap
 *   - 跨 CPU TLB 一致性见 smp_flush_tlb_pkmap() 注释（TODO：IPI flush）
 *
 * @page: 目标 struct page（可为任意 zone）
 * 返回：有效内核虚拟地址，NULL 表示 pkmap 槽位耗尽
 */
void *kmap(struct page *page)
{
    unsigned long vaddr;
    unsigned long phys;
    unsigned long nr;
    unsigned long flags;   /* spin_lock_irqsave 保存的中断标志 */
    int found;

    /*
     * ZONE_NORMAL：已有永久映射，直接返回，零开销，无需持锁
     * （直接映射地址由 zone_init 建立，全生命周期不变，SMP 无竞争）
     */
    if (page->virtual) {
        vaddr = (unsigned long)page->virtual;
        /* 已在 pkmap 范围内（上次 kmap 的结果），也直接返回 */
        if (vaddr >= PKMAP_BASE &&
            vaddr < PKMAP_BASE + LAST_PKMAP * PAGE_SIZE)
            return page->virtual;
        /* ZONE_NORMAL 的直接映射地址，直接返回 */
        return page->virtual;
    }

    /* ZONE_HIGHMEM：从 pkmap 分配槽位，全程持 kmap_lock */
    spin_lock_irqsave(&kmap_lock, flags);

    /* 轮转扫描：从 last_pkmap_nr+1 开始，绕一圈找空闲槽位（pkmap_count == 0） */
    nr    = last_pkmap_nr;
    found = 0;
    do {
        nr = (nr + 1) & (LAST_PKMAP - 1);  /* 对 1024 取模（位运算） */

        if (pkmap_count[nr] == 0) {
            found = 1;
            break;
        }
    } while (nr != last_pkmap_nr);

    /* 所有槽位被占用：尝试回收计数为 1 的槽位后重试 */
    if (!found) {
        if (!flush_all_zero_pkmaps()) {
            printk("kmap: pkmap slots exhausted (all %d in use)\n", LAST_PKMAP);
            spin_unlock_irqrestore(&kmap_lock, flags);
            return NULL;
        }
        /* 回收后重新扫描（此时必然有空闲槽位） */
        nr = last_pkmap_nr;
        do {
            nr = (nr + 1) & (LAST_PKMAP - 1);
            if (pkmap_count[nr] == 0)
                break;
        } while (nr != last_pkmap_nr);
    }

    last_pkmap_nr = nr;

    /* 初始计数设为 2（活跃引用：kmap 持有 1 个引用 + 占位 1） */
    pkmap_count[nr] = 2;

    /* 计算槽位对应的虚拟地址 */
    vaddr = PKMAP_BASE + nr * PAGE_SIZE;

    /* 计算页面物理地址并写入 PTE（PAGE_KERNEL：Present+RW+Dirty+Accessed，缓存启用） */
    phys = page_to_phys(page);
    set_pte(&pkmap_page_table[nr],
            __pte(phys | pgprot_val(PAGE_KERNEL)));

    /* 刷新该虚拟地址的 TLB 条目，确保本 CPU 立即看到新映射 */
    smp_flush_tlb_pkmap(vaddr);

    /* 缓存到 page->virtual，后续 kmap 调用直接命中，无需重复分配槽位 */
    page->virtual = (void *)vaddr;

    spin_unlock_irqrestore(&kmap_lock, flags);

    printk("kmap: page phys=0x%lx -> pkmap vaddr=0x%lx (slot %lu, cpu %d)\n",
           phys, vaddr, nr, smp_processor_id());

    return page->virtual;
}

/*
 * kunmap - 释放 kmap 建立的 pkmap 映射（SMP 安全）
 *
 * 参考 Linux 2.6.20 mm/highmem.c kunmap()
 *
 * 处理逻辑：
 *   ZONE_NORMAL（page->virtual 不在 pkmap 范围）：no-op（永久映射无需释放）
 *   ZONE_HIGHMEM（page->virtual 在 pkmap 范围）：
 *     ① 加 kmap_lock 自旋锁
 *     ② 计算槽位号 nr = (vaddr - PKMAP_BASE) / PAGE_SIZE
 *     ③ pkmap_count[nr]--（减少引用计数）
 *     ④ 若计数降为 1（无人引用），清除 PTE + smp_flush_tlb_pkmap() 刷 TLB
 *     ⑤ 清空 page->virtual = NULL（下次 kmap 重新分配槽位）
 *     ⑥ 释放 kmap_lock
 *
 * SMP 安全性说明：
 *   - 锁保护 pkmap_count[] 减操作和 PTE 清除的原子性
 *   - spin_lock_irqsave 防止中断处理程序嵌套修改同一槽位
 *   - 跨 CPU TLB 一致性见 smp_flush_tlb_pkmap() 注释（TODO：IPI flush）
 *
 * 注意：buffer cache 场景下通常不调用 kunmap（页长期缓存），
 * 映射持久保留在 page->virtual，直到页释放时由上层负责清理。
 */
void kunmap(struct page *page)
{
    unsigned long vaddr;
    unsigned long nr;
    unsigned long flags;   /* spin_lock_irqsave 保存的中断标志 */

    if (!page->virtual)
        return;

    vaddr = (unsigned long)page->virtual;

    /* ZONE_NORMAL：不在 pkmap 范围内，无需释放（永久映射） */
    if (vaddr < PKMAP_BASE || vaddr >= PKMAP_BASE + LAST_PKMAP * PAGE_SIZE)
        return;

    /* ZONE_HIGHMEM：在 pkmap 范围内，释放槽位，全程持 kmap_lock */
    spin_lock_irqsave(&kmap_lock, flags);

    nr = (vaddr - PKMAP_BASE) >> PAGE_SHIFT;

    if (pkmap_count[nr] <= 0) {
        printk("kunmap: slot %lu already free (count=%d), vaddr=0x%lx\n",
               nr, pkmap_count[nr], vaddr);
        page->virtual = NULL;
        spin_unlock_irqrestore(&kmap_lock, flags);
        return;
    }

    pkmap_count[nr]--;

    /* 计数降为 1（无人活跃引用）：清除 PTE，刷新本 CPU TLB */
    if (pkmap_count[nr] == 1) {
        set_pte(&pkmap_page_table[nr], __pte(0));
        smp_flush_tlb_pkmap(vaddr);
    }

    /* 清空 page->virtual，下次 kmap 重新分配槽位 */
    page->virtual = NULL;

    spin_unlock_irqrestore(&kmap_lock, flags);
}
