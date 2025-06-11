#ifndef __BOOTMEM_H__
#define __BOOTMEM_H__

#include <stdint.h>
#include <arch/linkage.h>

extern unsigned long min_low_pfn; /** low mem min pfn */
extern unsigned long max_low_pfn; /** low mem max pfn */
extern unsigned long max_pfn;     /** mem max pfn */


typedef struct  bootmem_data
{
    unsigned long node_boot_start; 
    unsigned long node_low_pfn; /// 896M
    void * node_bootmem_map; /** bootmem bitmap */
    unsigned long last_pos;
    unsigned long last_offset;
} bootmem_data_t;
 

unsigned long __init init_bootmem(unsigned long start,unsigned long end);
void __init free_bootmem(unsigned long addr, unsigned long size);
void __init reserve_bootmem(unsigned long addr, unsigned long size);
void * __init __alloc_bootmem(unsigned long size, unsigned long align, unsigned long goal);
void *alloc_bootmem_low_pages(unsigned long size);
unsigned long __init free_all_bootmem();
#endif