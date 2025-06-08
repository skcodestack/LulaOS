#include <mm/mm.h>
#include <mm/bootmem.h>
#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/highmem.h>
#include <mm/mmzone.h>


mem_map_t * mem_map;
struct list_head inactive_list;
struct list_head active_list;



/**
 * 直接内存映射 0-896M
 */
static void __init direct_area_init(void)
{
    unsigned long vaddr;
    unsigned long end = __va(PFN_PHYS(max_low_pfn)); // 896M virtual address
    pgd_t *pgd_base = swapper_pg_dir;

    int i = pgd_index(PAGE_OFFSET); // PAGE_OFFSET index on the pgd table
    pgd_t *pgd = pgd_base + i;      // PAGE_OFFSET item on the pgd table

    for (; i < PTRS_PER_PGD; pgd++, i++)
    {
        vaddr = i * PGDIR_SIZE;
        if (end && vaddr >= end)
        {
            break;
        }
        pte_t *pte = (pte_t *)alloc_bootmem_low_pages(PAGE_SIZE);
        pte_t *pte_base = pte;
        for (int j = 0; j < PTRS_PER_PTE; pte++, i++)
        {
            vaddr = i * PGDIR_SIZE + j * PAGE_SIZE;
            if (end && (vaddr >= end))
            {
                break;
            }
            set_pte(pte, __mk_pte(PFN_DOWN(__pa(vaddr)), PAGE_KERNEL)); // set pte item
        }

        set_pgd(pgd, __pgd(__pa(pte_base) + _KERNPG_TABLE)); // set pdt item
    }
}

/**
 * 这边对齐4M,最终区域位 4G-4M --4G
 */
static __init fix_area_init(void)
{
    // 4M alin
    unsigned long vaddr = fix_to_virt(__end_of_fixed_addresses - 1) & PGDIR_MASK;

    pgd_t *pgd_base = swapper_pg_dir;
    unsigned long end = 0;
    int i = pgd_index(vaddr); // PAGE_OFFSET index on the pgd table
    pgd_t *pgd = pgd_base + i;      // PAGE_OFFSET item on the pgd table

    for (; i < PTRS_PER_PGD; pgd++, i++)
    { 
        if ( vaddr == end)
        {
            break;
        }
        if(pgd_none(*pgd)){
            pte_t *pte = (pte_t *)alloc_bootmem_low_pages(PAGE_SIZE); 
            set_pgd(pgd, __pgd(__pa(pte) + _KERNPG_TABLE)); // set pdt item
        } 
        vaddr+= PGDIR_SIZE;
    }

}

static void __init persist_area_init(){
    unsigned long vaddr = PKMAP_BASE;
    unsigned long end = vaddr + PAGE_SIZE*LAST_PKMAP; //4M
    pgd_t *pgd_base = swapper_pg_dir;

    int i = pgd_index(vaddr); // PAGE_OFFSET index on the pgd table
    pgd_t *pgd = pgd_base + i;      // PAGE_OFFSET item on the pgd table

    for (; i < PTRS_PER_PGD; pgd++, i++)
    { 
        if (vaddr >= end)
        {
            break;
        }
        if(pgd_none(*pgd)){
            pte_t *pte = (pte_t *)alloc_bootmem_low_pages(PAGE_SIZE); 
            set_pgd(pgd, __pgd(__pa(pte) + _KERNPG_TABLE)); // set pdt item
        } 
        vaddr+= PGDIR_SIZE;
    } 

    pkmap_page_table =  pte_offset(pgd,vaddr);

}

/**
 *  页表初始化,zone区初始化
 *  1.建立 0----896页表
    2.虚拟内存 
        1.固定映射 4G-4M --- 4G
        2.永久映射 4G-32M --4G-28M

 */
void __init paging_init()
{

    /// 1. map direct area 0M-896M
    direct_area_init(); 
    /// 2. fix area n*4K - 4G 
    fix_area_init(); 
    /// 3.persist map area 4G-8M
    persist_area_init();
    
    /// 4.load pgt
    load_cr3(__pa(swapper_pg_dir));

    /// 5.fixed kmap init
    fixed_kmap_init();

    /// 6.zone init
    zone_init();
}

/** 
 *   
 *      1.将低端内存添加伙伴系统（0->896M）
		2.回收清除启动内存分配器（位图）
		3.高端内存处理（896M-4G），将高端内存添加到伙伴系统
		4.empty_zero_page 使用结束，清除
		6.0->8M 映射到用户空间，清除（非SMP,次CPU需要使用）
		7.收集统计信息
 */
void __init mm_init(void)
{ 
    /// 1. 将低端内存添加伙伴系统
    

    /// 2. 回收清除启动内存分配器 bootmem alloc

    /// 3. 高端内存添加到伙伴系统
    
    /// 4.回收empty_zero_page

    /// 5.清除内核映射到用户空间页表


 
}