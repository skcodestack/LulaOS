#include <arch/x86/setup.h> 
#include<arch/x86/boot/multiboot.h>
#include <arch/linkage.h>
#include <mm/bootmem.h>
#include <arch/x86/pfh.h>
#include <arch/x86/mb.h>

unsigned int multi_boot_size = sizeof(multiboot_info_t);

unsigned long init_pg_tables_end = ~0UL;
multiboot_info_t * multiboot_params;

/**
 * setup memery info 
 */
static __init unsigned long setup_memery(){
    
    min_low_pfn = PFN_UP(init_pg_tables_end);
     
    find_max_pfn();
    
     
    return 0;
}

void __init setup_arch(){
    unsigned long max_low_pfn;

    max_low_pfn = setup_memery();
}

