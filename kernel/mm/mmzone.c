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



static char *zone_names[MAX_NR_ZONES] = { "DMA", "Normal", "HighMem" };

static bootmem_data_t bootmem_data;
pg_data_t contig_page_data = {bdata : &bootmem_data};
struct list_head inactive_list;
struct list_head active_list;

static inline void build_zonelists(pg_data_t *pgdat)
{
	int i, j, k;
	
	for (i = 0; i <= GFP_ZONEMASK; i++) { 
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
		switch (k) { 
			 
			case ZONE_HIGHMEM:
				zone = pgdat->node_zones + ZONE_HIGHMEM;
				if (zone->size) { 
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
		//用NULL 结束链表
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

    printk("total pages: %d,total size:%dM \n",total_pages ,(total_pages * 4) >> 10  );

    INIT_LIST_HEAD(&active_list);
    INIT_LIST_HEAD(&inactive_list);

    unsigned long map_size =  (total_pages + 1) * sizeof(struct page);
    struct page * mem_map_start = (struct page *)alloc_bootmem_low_pages(map_size);
    mem_map_start = (struct page *) (PAGE_OFFSET + MAP_ALIGN((unsigned long)mem_map_start - PAGE_OFFSET));
    
    mem_map =pgdat->node_mem_map = mem_map_start;
    pgdat->node_size = total_pages;
    pgdat->node_start_paddr = 0;
    pgdat->node_start_mapnr = 0;
    pgdat->nr_zones = 0;


    /// init page
    for (struct page * p = mem_map_start; p < mem_map_start + total_pages; p++)
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
        zone_t* zone =  pgdat->node_zones + i;
        unsigned long size = zone_size[i];
        zone->size = size;
        zone->name = zone_names[i];
        zone->zone_pgdat = pgdat;
        zone->free_pages = 0;
        if(!size){
            continue;
        }
        pgdat->nr_zones = i + 1;

        zone->zone_mem_map = mem_map + offset;
        zone->zone_start_paddr = start_paddr;
        zone->zone_start_mapnr = offset;

        for (unsigned long j = 0; j < size; j++)
        {
           struct page* page =  (mem_map + offset + j);
           page->zone = zone;
           if(i != ZONE_HIGHMEM){
                page->virtual =   __va(start_paddr);
           }
           start_paddr += PAGE_SIZE;
        }
        offset += size;

        
        for (unsigned long k = 0; k < MAX_ORDER; k++)
        {
            INIT_LIST_HEAD(&zone->free_area[k].free_list);
            if(k == MAX_ORDER -1){
                zone->free_area[k].map = NULL;
                continue;
            }
            unsigned long bitmap_size = (size-1) >> (i+4);//需要的字节
			bitmap_size = LONG_ALIGN(bitmap_size+1);
			zone->free_area[i].map = 
			  (unsigned long *) __alloc_bootmem(bitmap_size,PAGE_SIZE,0);
        } 
    }
    
    build_zonelists(pgdat);
}