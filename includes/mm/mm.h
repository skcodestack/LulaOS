#ifndef __MM_H__
#define __MM_H__
#include <arch/linkage.h>
#include <libs/list.h>
#include <arch/x86/atomic.h>


typedef struct page {
	struct list_head list;		/* 下一页 */   
	atomic_t count;			/*引用  */
	unsigned long flags;	 
	// wait_queue_head_t wait;		/*等待这一页的页队列*/  
	void *virtual;			/*  内核映射地址  DMA  NORMAL,如果为null，那么肯定是higmem */
	struct zone_struct *zone;	/* 管理区 */
} mem_map_t;

//page array
extern mem_map_t * mem_map;

extern struct list_head inactive_list;
extern struct list_head active_list;

void __init paging_init();
void __init mm_init(void);    

#endif