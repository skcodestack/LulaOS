#include <arch/x86/setup.h> 
#include<arch/x86/boot/multiboot.h>

unsigned int multi_boot_size = sizeof(multiboot_info_t);

unsigned long init_pg_tables_end = ~0UL;