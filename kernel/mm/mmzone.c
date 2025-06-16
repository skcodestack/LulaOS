#include <mm/mmzone.h>
#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/highmem.h>
#include <mm/mm.h>
#include <printk.h>
#include <libs/list.h>
#include <mm/bootmem.h>
#include <stddef.h>
#include <libs/memcpy.h>
#include <arch/x86/spinlock.h>
#include <arch/x86/bitops.h>

static char *zone_names[MAX_NR_ZONES] = {"DMA", "Normal", "HighMem"};

static bootmem_data_t bootmem_data;
pg_data_t contig_page_data = {bdata : &bootmem_data};
struct list_head inactive_list;
struct list_head active_list;

static inline void build_zonelists(pg_data_t *pgdat)
{
    int i, j, k;

    for (i = 0; i <= GFP_ZONEMASK; i++)
    {
        zonelist_t *zonelist;
        zone_t *zone;

        zonelist = pgdat->node_zonelists + i;
        memset(zonelist, 0, sizeof(*zonelist));
        j = 0;
        k = ZONE_NORMAL;
        if (i & __GFP_HIGHMEM)
            k = ZONE_HIGHMEM;
        if (i & __GFP_DMA)
            k = ZONE_DMA;
        switch (k)
        {

        case ZONE_HIGHMEM:
            zone = pgdat->node_zones + ZONE_HIGHMEM;
            if (zone->size)
            {
                zonelist->zones[j++] = zone;
            }
        case ZONE_NORMAL:
            zone = pgdat->node_zones + ZONE_NORMAL;
            if (zone->size)
                zonelist->zones[j++] = zone;
        case ZONE_DMA:
            zone = pgdat->node_zones + ZONE_DMA;
            if (zone->size)
                zonelist->zones[j++] = zone;
        }
        // 用NULL 结束链表
        zonelist->zones[j++] = NULL;
    }
}

void __init zone_init()
{
    pg_data_t *pgdat = NODE_DATA;
    unsigned long zone_size[MAX_NR_ZONES] = {0, 0, 0};
    unsigned long max_dma_pfn = PFN_DOWN(__pa(MAX_DMA_ADDRESS));

    zone_size[ZONE_DMA] = max_dma_pfn;
    zone_size[ZONE_NORMAL] = max_low_pfn - max_dma_pfn;
    zone_size[ZONE_HIGHMEM] = highend_pfn - max_low_pfn;

    unsigned long total_pages = 0; /// total pfn size
    for (int i = 0; i < MAX_NR_ZONES; i++)
    {
        total_pages += zone_size[i];
    }

    printk("total pages: %d,total size:%dM \n", total_pages, (total_pages * 4) >> 10);

    INIT_LIST_HEAD(&active_list);
    INIT_LIST_HEAD(&inactive_list);

    unsigned long map_size = (total_pages + 1) * sizeof(struct page);
    struct page *mem_map_start = (struct page *)alloc_bootmem_low_pages(map_size);
    mem_map_start = (struct page *)(PAGE_OFFSET + MAP_ALIGN((unsigned long)mem_map_start - PAGE_OFFSET));

    mem_map = pgdat->node_mem_map = mem_map_start;
    pgdat->node_size = total_pages;
    pgdat->node_start_paddr = 0;
    pgdat->node_start_mapnr = 0;
    pgdat->nr_zones = 0;

    /// init page
    for (struct page *p = mem_map_start; p < mem_map_start + total_pages; p++)
    {
        set_page_count(p, 0);
        SetPageReserved(p);
        INIT_LIST_HEAD(&p->list);
    }

    unsigned long offset = 0;
    unsigned long start_paddr = 0;
    /// init zone
    for (unsigned int i = 0; i < MAX_NR_ZONES; i++)
    {
        zone_t *zone = pgdat->node_zones + i;
        unsigned long size = zone_size[i];
        zone->size = size;
        zone->name = zone_names[i];
        zone->zone_pgdat = pgdat;
        zone->free_pages = 0;
        zone->lock = SPIN_LOCK_UNLOCKED;
        if (!size)
        {
            continue;
        }
        pgdat->nr_zones = i + 1;

        zone->zone_mem_map = mem_map + offset;
        zone->zone_start_paddr = start_paddr;
        zone->zone_start_mapnr = offset;

        for (unsigned long j = 0; j < size; j++)
        {
            struct page *page = (mem_map + offset + j);
            page->zone = zone;
            if (i != ZONE_HIGHMEM)
            {
                page->virtual = __va(start_paddr);
            }
            start_paddr += PAGE_SIZE;
        }
        offset += size;

        for (unsigned long k = 0; k < MAX_ORDER; k++)
        {
            INIT_LIST_HEAD(&zone->free_area[k].free_list);
            if (k == MAX_ORDER - 1)
            {
                zone->free_area[k].map = NULL;
                continue;
            }
            unsigned long bitmap_size = (size - 1) >> (i + 4); // 需要的字节
            bitmap_size = LONG_ALIGN(bitmap_size + 1);
            zone->free_area[i].map =
                (unsigned long *)__alloc_bootmem(bitmap_size, PAGE_SIZE, 0);
        }
    }

    build_zonelists(pgdat);
}

/// 回收page
static void __free_pages_ok(struct page *page, unsigned int order)
{
    unsigned long flag; 
    if (PageLocked(page)) // 加锁
        return;
    if (PageLRU(page)) // 存在activit_list 和 inactivity_list
        return;
    if (PageActive(page)) // 存在activit_list
        return;

    ClearPageDirty(page);
    ClearPageReferenced(page);

    struct zone_struct *zone = page->zone;

    unsigned long mask = (~0UL) << order; /// 计算掩码，用于确定当前块的大小

    struct page *base = zone->zone_mem_map;
    unsigned long page_idx = page - base;

    /// 验证页面索引是否对齐到当前order
    if (page_idx & ~mask)
    {
        return;
    } 
    unsigned long index = page_idx >> (1 + order); // 伙伴块位图的索引
    free_area_t *area = zone->free_area + order;

    spin_lock_irqsave(&zone->lock, flag); 
    zone->free_pages -= mask; /// 更新空闲页计数

    // 尝试与伙伴块合并(循环直到无法合并或达到最大阶)
    while (mask + (1 << (MAX_ORDER - 1)))
    {
        struct page *buddy1, *buddy2;

        if (area >= zone->free_area + MAX_ORDER)
            break;

        if (!__test_and_change_bit(index, area->map))
            break; // 伙伴块不空闲，停止合并

        // 计算伙伴块的索引：通过异或操作找到相邻块
        // -mask等于(1<<order)，异或后得到伙伴块的索引
        buddy1 = base + (page_idx ^ -mask);
        buddy2 = base + page_idx;

        if (BAD_RANGE(zone, buddy1) || BAD_RANGE(zone, buddy2))
            break;

        // 从当前阶的空闲链表中移除伙伴块
        list_del(&buddy1->list);

        // 合并后：
        // 1. 块大小翻倍(mask左移1位)
        // 2. 移动到更高阶的free_area
        // 3. 位图索引右移(阶数增加，位图粒度变大)
        // 4. 更新页面索引(合并后的块起始位置)
        mask <<= 1;
        area++;
        index >>= 1;
        page_idx &= mask; // 合并后的块索引是两个伙伴块的公共前缀
    }
    // 将最终的块(可能已合并)添加到对应阶的空闲链表头部
    list_add(&(base + page_idx)->list, &area->free_list);
    spin_unlock_irqrestore(&zone->lock, flag);
}

void __free_pages(struct page *page, unsigned int order)
{
 
    /***
            其中比较巧妙的部分就是调用put_page_testzero()宏，该函数把页面的引用计数减1，
            如果减1 后引用计数为0，则该函数返回1。因此，如果调用者不是该页面的最后一个用户，
            那么，这个页面实际上就不会被释放。另外要说明的是不可释放保留页PageReserved，这是
            通过PageReserved（）宏进行检查的

            如果调用者是该页面的最后一个用户，则__free_pages() 再调用 __free_pages_ok()。
        __free_pages_ok（）才是对页面块进行释放的实际函数，该函数把释放的页面块链入空闲链
        表，并对伙伴系统的位图进行管理，必要时合并伙伴块。这实际上是expand()函数的反操作

    */ 
    ///不是 reserved  且 count 为1
    if (!PageReserved(page) && put_page_testzero(page)){
        __free_pages_ok(page, order);
    }
       
}

void free_pages(unsigned long addr, unsigned int order)
{
    if (addr != 0)
        __free_pages(virt_to_page(addr), order);
}

struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order){
    return NULL;
}