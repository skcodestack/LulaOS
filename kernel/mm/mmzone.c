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



struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order, zonelist_t *zonelist)
{
//     unsigned long min;
// 	zone_t **zone, * classzone;
// 	struct page * page;
// 	int freed;

// 	zone = zonelist->zones;
// 	classzone = *zone;
// 	min = 1UL << order;
// 	for (;;) {
// 		zone_t *z = *(zone++);
// 		if (!z)
// 			break;

// 		min += z->pages_low;
// 		//空闲页面总量大于最低线+请求分配页面数
// 		if (z->free_pages > min) {
// 			//分配 rmqueue
// 			page = rmqueue(z, order);
// 			if (page)
// 				return page;
// 		}
// 	}

// 	//可分配管理区都内存紧张，重新平衡标识设为1
// 	classzone->need_balance = 1;
// 	mb();//内存屏障
// 	//内核线程kswapd在一个等待队列中睡眠，唤醒
// 	if (waitqueue_active(&kswapd_wait))
// 		wake_up_interruptible(&kswapd_wait);

// 	//下调最低水位/4 ，看是否满足条件，如果满足，开始分配rmqueue
// 	zone = zonelist->zones;
// 	min = 1UL << order;
// 	for (;;) {
// 		unsigned long local_min;
// 		zone_t *z = *(zone++);
// 		if (!z)
// 			break;

// 		local_min = z->pages_min;
// 		if (!(gfp_mask & __GFP_WAIT))
// 			local_min >>= 2;
// 		min += local_min;
// 		if (z->free_pages > min) {
// 			page = rmqueue(z, order);
// 			if (page)
// 				return page;
// 		}
// 	}

// 	/* here we're in the low on memory slow path

// 		内存不足的缓慢路径



// 		如果分配还不成功，这时候就要看是哪类进程在请求分配内存页面。其中PF_MEMALLOC
// 和PF_MEMDIE 是进程的task_struct 结构中flags 域的值，对于正在分配页面的进程（如
// kswapd 内核线程），则其PF_MEMALLOC 的值为1（一般进程的这个标志为0），而对于使内存
// 溢出而被杀死的进程，则其PF_MEMDIE 为1。不管哪种情况，都说明必须给该进程分配页面
// （想想为什么）。因此，继续进行分配
// 	*/

// rebalance:
// 	if (current->flags & (PF_MEMALLOC | PF_MEMDIE)) {
// 		//继续分配 rmqueue
// 		zone = zonelist->zones;
// 		for (;;) {
// 			zone_t *z = *(zone++);
// 			if (!z)
// 				break;

// 			page = rmqueue(z, order);
// 			if (page)
// 				return page;
// 		}
// 		return NULL;
// 	}

// 	/* Atomic allocations - we can't balance anything
// 	如果请求分配页面的进程不能等待，也不能被重新调度，只好在没有分配到页面的情况
// 下“空手”返回
// */
// 	if (!(gfp_mask & __GFP_WAIT))
// 		return NULL;

// 	/****


// 	如果经过几番努力，必须得到页面的进程（如kswapd）还没有分配到页面，就要调用
// 	balance_classzone（）函数把当前进程所占有的局部页面释放出来。如果释放成功，则返回
// 	一个page 结构指针，指向页面块中第一个页面的起始地址。


// 	*/
// 	page = balance_classzone(classzone, gfp_mask, order, &freed);
// 	if (page)
// 		return page;

// 	//继续进行分配
// 	zone = zonelist->zones;
// 	min = 1UL << order;
// 	for (;;) {
// 		zone_t *z = *(zone++);
// 		if (!z)
// 			break;

// 		min += z->pages_min;
// 		if (z->free_pages > min) {
// 			page = rmqueue(z, order);
// 			if (page)
// 				return page;
// 		}
// 	}

// 	/* Don't let big-order allocations loop 
	
// 	*/
// 	if (order > 3)
// 		return NULL;

// 	/* Yield for kswapd, and try again
// 		 重试
// 	*/
// 	current->policy |= SCHED_YIELD;
// 	__set_current_state(TASK_RUNNING);
// 	schedule();
// 	goto rebalance;

    return NULL;
}

struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order)
{
	return __alloc_pages(gfp_mask, order,
		contig_page_data.node_zonelists+(gfp_mask & GFP_ZONEMASK));
}