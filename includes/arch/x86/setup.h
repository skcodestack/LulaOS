#ifndef __SETUP_H__
#define __SETUP_H__
#include <arch/x86/boot/multiboot.h> 

//boot page end 
extern unsigned long init_pg_tables_end;

//multboot info table
extern multiboot_info_t* multiboot_params;


void setup_arch();


#endif