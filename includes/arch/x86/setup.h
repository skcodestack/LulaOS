#ifndef __SETUP_H__
#define __SETUP_H__

//multi boot size
extern unsigned int multi_boot_size;

//boot page end 
extern unsigned long init_pg_tables_end;

//multboot info table
extern multiboot_info_t* multiboot_params;


void setup_arch();


#endif