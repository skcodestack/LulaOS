#include <arch/x86/setup.h> 
#include<arch/x86/boot/multiboot.h>
#include <arch/linkage.h>
#include <mm/bootmem.h>
#include <arch/x86/page.h> 
#include <printk.h> 
#include <mm/mm.h>
#include <libs/memcpy.h>
#include <arch/x86/highmem.h> 

unsigned long init_pg_tables_end = ~0UL;
multiboot_info_t * multiboot_params_addr;

char multiboot_params[4096] ={0};
multiboot_info_t * multiboot_info_base = (multiboot_info_t *)multiboot_params;


/**
 * copy multiboot info from blow 1M to 
 */
static __init void copy_multiboot_info(){

    void * dest = multiboot_params;
    memcpy(dest, multiboot_params_addr, sizeof(multiboot_info_t));
    dest+= sizeof(multiboot_info_t);

    multiboot_memory_map_t * mmap =   (multiboot_memory_map_t *)multiboot_params_addr->mmap_addr;
    unsigned long mmap_size =  multiboot_params_addr->mmap_length;
    memcpy(dest,mmap,mmap_size);
    multiboot_params_addr->mmap_addr = (unsigned long)dest;
    dest+= mmap_size; 

} 


/**
 *  page is need ram 
 */
static inline int page_is_ram(unsigned long pagenr){
 
    unsigned int size = multiboot_info_base->mmap_length / sizeof(multiboot_memory_map_t);
    multiboot_memory_map_t * mmap =   (multiboot_memory_map_t *)multiboot_info_base->mmap_addr;
     
    for (unsigned int i = 0; i < size; i++)
    {   
        multiboot_memory_map_t* entry =  (multiboot_memory_map_t *)(mmap+i); 
        if(entry->type != MULTIBOOT_MEMORY_AVAILABLE){
            continue;
        }
        unsigned long start = PFN_UP(entry->addr_low);
        unsigned long end = PFN_DOWN(entry->addr_low + entry->len_low);
        if(pagenr >= start && pagenr < end){
            return 1;
        }
    }
    return 0;
}


/**
 * search phy mem max low pfn 
 */
unsigned long __init find_max_low_pfn(){
    unsigned long max_low_pfn;

#define VMALLOC_RESERVE	(unsigned long)(128 << 20) //128M for vmalloc
#define MAXMEM (unsigned long)(-PAGE_OFFSET-VMALLOC_RESERVE) //896M for direct map
#define MAXMEM_PFN PFN_DOWN(MAXMEM)
#define MAX_4G_PFN 1UL << 20
    max_low_pfn = max_pfn;
    if(max_low_pfn > MAXMEM_PFN){
        max_low_pfn = MAXMEM_PFN; 
    }
    if(max_pfn > MAX_4G_PFN){
        max_pfn = MAX_4G_PFN;
    } 
    highstart_pfn = highend_pfn = max_pfn;
    if(max_pfn > MAXMEM_PFN){
        highstart_pfn = MAXMEM_PFN;
    }
    printk("hight mem available size: %d M\n",((highend_pfn - highstart_pfn)>>(20-PAGE_SHIFT))); 

    return max_low_pfn;
}


/**
 * search phy mem max pfn 
 */
unsigned long __init find_max_pfn(){ 

    unsigned long  max_pfn = 0;

    unsigned int size = multiboot_info_base->mmap_length / sizeof(multiboot_memory_map_t);
    multiboot_memory_map_t * mmap =   (multiboot_memory_map_t *)multiboot_info_base->mmap_addr;
     
    for (unsigned int i = 0; i < size; i++)
    {   
        multiboot_memory_map_t* entry =  (multiboot_memory_map_t *)(mmap+i);
        printk("Start Address: %x, Length: %d KB , Size: %x , Type: %d\n",entry->addr_low,entry->len_low >> 10,entry->size,entry->type);
        if(entry->type != MULTIBOOT_MEMORY_AVAILABLE){
            continue;
        }
        unsigned long start = PFN_UP(entry->addr_low);
        unsigned long end = PFN_DOWN(entry->addr_low + entry->len_low);
        if(start >= end){
            continue;
        }
        if(end > max_pfn){
            max_pfn = end;
        }
    }
    return max_pfn;
}

/**
 *  setup mem bootmem allocator
 */
void __init setup_bootmem_allocator(){
    unsigned long bootmap_size;
    unsigned int i;
    bootmap_size = init_bootmem(min_low_pfn,max_low_pfn);
    
    unsigned int size = multiboot_info_base->mmap_length / sizeof(multiboot_memory_map_t);
    multiboot_memory_map_t * mmap =   (multiboot_memory_map_t *)multiboot_info_base->mmap_addr;
     
    for (i = 0; i < size; i++)
    {   
        multiboot_memory_map_t* entry =  (multiboot_memory_map_t *)(mmap+i); 
        if(entry->type != MULTIBOOT_MEMORY_AVAILABLE){
            continue;
        } 
        unsigned long start = PFN_UP(entry->addr_low);
        if(start > max_low_pfn){
            continue;
        }
        unsigned long end = PFN_DOWN(entry->addr_low + entry->len_low); 
        if(end > max_low_pfn){
            end = max_low_pfn;
        } 
        if(start >= end){
            continue;
        }
        int size = end - start;
        //setting useable 
        free_bootmem(PFN_PHYS(start),PFN_PHYS(size));
    }

    //reserve  1M--1M+bitmap
#define MEM_1M (1<<20)
    reserve_bootmem(MEM_1M, (PFN_PHYS(min_low_pfn) + bootmap_size + PAGE_SIZE -1) - MEM_1M);

    //reserve 0-4K
    reserve_bootmem(0,PAGE_SIZE);
    //reserve 4K-8K for smp ap use
    reserve_bootmem(PAGE_SIZE,PAGE_SIZE); 

    /*
        执行到这0-896M空间结构图(不包含APIC，不考虑非Ram或者空洞)：
        ---------------------------------------------
        0-8k   |不可分配
        ---------------------------------------------
        8k-640k   |可分配（可能有APIC配置表不可分配区域）
        -----------------------------------------------
        640k-1M   |不可分配（图形卡+BIOS代码，被剔除了，不在bootmem管理）
        --------------------------------------------------
        1M-(1M+内核大小+bootmem大小)     |        不可分配
        -----------------------------------------------------------
        (1M+内核大小+bootmem大小)- 896M   |        可分配
	    -------------------------------------------------------------
    */ 
}


/**
 * setup memery info 
 */
static __init unsigned long setup_memery(){
    
    min_low_pfn = PFN_UP(init_pg_tables_end);
     
    max_pfn = find_max_pfn();
    
    max_low_pfn = find_max_low_pfn(); 

    printk("min_low_pfn: %d ,max_low_pfn: %d, max_pfn: %d\n",min_low_pfn,max_low_pfn,max_pfn);

    setup_bootmem_allocator();
    
    /** 3G --4G 页表建设  */ 
    paging_init();
    return max_low_pfn;
}



void __init setup_arch(){
    copy_multiboot_info();
    setup_memery();

    tty_set_buffer_base(PAGE_OFFSET);
}

