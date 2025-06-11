#ifndef __MM_H__
#define __MM_H__
#include <arch/linkage.h>
#include <libs/list.h>
#include <arch/x86/atomic.h>
#include <arch/x86/bitops.h>
#include <mm/mmzone.h>



typedef struct page {
	struct list_head list;		/* 下一页 */   
	atomic_t count;			/*引用  */
	unsigned long flags;	 
	// wait_queue_head_t wait;		/*等待这一页的页队列*/  
	void *virtual;			/*  内核映射地址  DMA  NORMAL,如果为null，那么肯定是higmem */
	struct zone_struct *zone;	/* 管理区 */
} mem_map_t;

#define get_page(p)		atomic_inc(&(p)->count)
#define put_page(p)		__free_page(p)
#define put_page_testzero(p) 	atomic_dec_and_test(&(p)->count)
#define page_count(p)		atomic_read(&(p)->count)
#define set_page_count(p,v) 	atomic_set(&(p)->count, v)

#define PG_locked		 0	 
#define PG_error		 1
#define PG_referenced	 2
#define PG_uptodate		 3
#define PG_dirty		 4
#define PG_unused		 5
#define PG_lru			 6
#define PG_active		 7
#define PG_slab			 8
#define PG_skip			10
#define PG_highmem		11   
#define PG_checked		12	 
#define PG_arch_1		13
#define PG_reserved		14   
#define PG_launder		15	 

// #define UnlockPage(page)	unlock_page(page)
#define Page_Uptodate(page)	test_bit(PG_uptodate, &(page)->flags)
#define SetPageUptodate(page)	set_bit(PG_uptodate, &(page)->flags)
#define ClearPageUptodate(page)	clear_bit(PG_uptodate, &(page)->flags)
#define PageDirty(page)		test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)	set_bit(PG_dirty, &(page)->flags)
#define ClearPageDirty(page)	clear_bit(PG_dirty, &(page)->flags)
#define PageLocked(page)	test_bit(PG_locked, &(page)->flags)
#define LockPage(page)		set_bit(PG_locked, &(page)->flags)
#define TryLockPage(page)	test_and_set_bit(PG_locked, &(page)->flags)
#define PageChecked(page)	test_bit(PG_checked, &(page)->flags)
#define SetPageChecked(page)	set_bit(PG_checked, &(page)->flags)
#define PageLaunder(page)	test_bit(PG_launder, &(page)->flags)
#define SetPageLaunder(page)	set_bit(PG_launder, &(page)->flags)

#define PageError(page)		test_bit(PG_error, &(page)->flags)
#define SetPageError(page)	set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	clear_bit(PG_error, &(page)->flags)
#define PageReferenced(page)	test_bit(PG_referenced, &(page)->flags)
#define SetPageReferenced(page)	set_bit(PG_referenced, &(page)->flags)
#define ClearPageReferenced(page)	clear_bit(PG_referenced, &(page)->flags)
#define PageTestandClearReferenced(page)	test_and_clear_bit(PG_referenced, &(page)->flags)
#define PageSlab(page)		test_bit(PG_slab, &(page)->flags)
#define PageSetSlab(page)	set_bit(PG_slab, &(page)->flags)
#define PageClearSlab(page)	clear_bit(PG_slab, &(page)->flags)
#define PageReserved(page)	test_bit(PG_reserved, &(page)->flags)


#define PageActive(page)	test_bit(PG_active, &(page)->flags)
#define SetPageActive(page)	set_bit(PG_active, &(page)->flags)
#define ClearPageActive(page)	clear_bit(PG_active, &(page)->flags)

#define PageLRU(page)		test_bit(PG_lru, &(page)->flags)
#define TestSetPageLRU(page)	test_and_set_bit(PG_lru, &(page)->flags)
#define TestClearPageLRU(page)	test_and_clear_bit(PG_lru, &(page)->flags)

#define PageHighMem(page)		test_bit(PG_highmem, &(page)->flags)

#define SetPageReserved(page)		set_bit(PG_reserved, &(page)->flags)
#define ClearPageReserved(page)		clear_bit(PG_reserved, &(page)->flags)


#define set_page_count(p,v) 	atomic_set(&(p)->count, v)
//page array
extern mem_map_t * mem_map;

extern unsigned long max_mapnr;
extern unsigned long num_physpages;
extern void * high_memory; //

extern struct list_head inactive_list;
extern struct list_head active_list;

void __init paging_init();
void __init mm_init(void);    

#endif