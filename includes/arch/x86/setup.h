#ifndef __SETUP_H__
#define __SETUP_H__
#include <arch/x86/boot/multiboot.h> 
#include <arch/linkage.h>

//boot page end 
extern unsigned long init_pg_tables_end;

//multboot info addr  for blew 1M
extern multiboot_info_t * multiboot_params_addr;

//mulboot info area 
extern char multiboot_params[4096];
extern multiboot_info_t * multiboot_info_base;

void __init setup_arch();
int __init page_is_ram(unsigned long pagenr);

#endif