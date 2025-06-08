#include <mm/bootmem.h>
#include <mm/mmzone.h>
#include <arch/x86/page.h>
#include <libs/memcpy.h>
#include <arch/x86/bitops.h>
#include <stddef.h>

unsigned long min_low_pfn;
unsigned long max_low_pfn;
unsigned long max_pfn;


unsigned long __init init_bootmem(unsigned long start,unsigned long end){
    min_low_pfn = start;
    max_low_pfn = end; 
    
    unsigned long mapSize = ((end-0) + 7)/8; // need bytes
    mapSize = (mapSize + (sizeof(long) - 1)) & ~(sizeof(long) - 1); //align 4 byte


    bootmem_data_t* bdata =  contig_page_data.bdata; 
    bdata->node_bootmem_map = __va(PFN_PHYS(start));
    bdata->node_boot_start = PFN_PHYS(0);
    bdata->node_low_pfn = end;

    memset(bdata->node_bootmem_map,0xff,mapSize);
    
    return mapSize;
}


/**
 * free mem bit, setting usable
 */
void __init free_bootmem(unsigned long addr, unsigned long size){

    bootmem_data_t* bdata =  contig_page_data.bdata;
    unsigned long start_pfn = PFN_UP(addr);
    unsigned long starti = start_pfn - PFN_DOWN(bdata->node_boot_start);
    unsigned long end_pfn = PFN_DOWN(addr + size);
    unsigned long endi = end_pfn - PFN_DOWN(bdata->node_boot_start);

    if(end_pfn > bdata->node_low_pfn){
        return;
    }
    for (int i = starti; i < endi; i++)
    {
        test_and_clear_bit(i,bdata->node_bootmem_map);
    }  
}

/**
 * reserve bit , unusable
 */
void __init reserve_bootmem(unsigned long addr, unsigned long size){
    bootmem_data_t* bdata =  contig_page_data.bdata;
    unsigned long start_pfn = PFN_DOWN(addr);
    unsigned long starti = start_pfn - PFN_DOWN(bdata->node_boot_start);
    unsigned long end_pfn = PFN_UP(addr + size);
    unsigned long endi = end_pfn - PFN_DOWN(bdata->node_boot_start);

    if(starti < 0 
        || endi < 0
        || start_pfn >= bdata->node_low_pfn
        || end_pfn > bdata->node_low_pfn
        || starti >= endi){
        return;
    }
    for (int i = starti; i < endi; i++)
    {
        test_and_set_bit(i,bdata->node_bootmem_map);
    }  
}

/**
 * alloc mem 
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align, unsigned long goal){
    unsigned long areasize,start,preferred,incr,offset,remaining_size;
    void * ret;
    bootmem_data_t* bdata =  contig_page_data.bdata;
    unsigned long pageCount = bdata->node_low_pfn - PFN_DOWN(bdata->node_boot_start);

    if(!size || (align & (align-1))){
        return NULL;
    }
        

    if(goal && goal >= bdata->node_boot_start 
        && PFN_DOWN(goal) < bdata->node_low_pfn){
        preferred = goal - bdata->node_boot_start;
    }else {
        preferred = 0;
    }

    preferred = PFN_DOWN((preferred + (align -1)) & ~(align-1));
    areasize = PFN_UP(size); //need page count
    incr = (align >> PAGE_SHIFT) ? (align >> PAGE_SHIFT): 1;

restart_alloc:
    for (unsigned long i = preferred; i < pageCount; i++)
    {
        unsigned long j;
         //页面不可用
         if(test_bit(i, bdata->node_bootmem_map)){
            continue;
         }
         for (j = i+1; j < areasize; j++)
         {
             if(j >= pageCount){
                goto fail_block;
             }
             if(test_bit(j, bdata->node_bootmem_map)){
               goto fail_block;
             }
         }
         start = i;
         goto found;
    fail_block:;
    }
    if(preferred){
        preferred = 0;
        goto restart_alloc;
    }
    return NULL;
found:
    if(start >= pageCount){
        return NULL;
    }

    if(align <= PAGE_SIZE 
        && bdata->last_offset 
        && bdata->last_pos+1 == start){
        //新申请的page上一页还没分配完
        offset =  (bdata->last_offset + align -1) & ~(align -1);
        if(offset > PAGE_SIZE){
            return NULL;
        }
        remaining_size = PAGE_SIZE-offset;
        if(size < remaining_size){
            areasize = 0;
            bdata->last_offset= offset + size;
            ret = __va(PFN_PHYS(bdata->last_pos) + offset + bdata->node_boot_start);
        }else {
            //还需remaining_size 大小
			remaining_size = size - remaining_size;
			//再分配areasize大小页
			areasize = PFN_UP(remaining_size);
			ret = __va(PFN_PHYS(bdata->last_pos) + offset + bdata->node_boot_start);
			 
			bdata->last_pos = start+areasize-1;
			bdata->last_offset = remaining_size & ~PAGE_MASK;
        }
    }else {
        bdata->last_pos = start + areasize - 1;
		bdata->last_offset = size & ~PAGE_MASK;
		ret = __va(PFN_PHYS(start) + bdata->node_boot_start);
    }

    for (unsigned long i = start; i < start+areasize; i++){
        test_and_set_bit(i, bdata->node_bootmem_map);
    }  	 
	memset(ret, 0, size);
	return ret;
} 


void *alloc_bootmem_low_pages(unsigned long size)
{
    return __alloc_bootmem(size, PAGE_SIZE, 0);
}