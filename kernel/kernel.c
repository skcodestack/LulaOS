#include <printk.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <linkage.h>
#include <interrupts/interrupts.h>
void _kernel_init()
{
    _init_gdt();
    _init_idt();
    _init_interrupts();
}

asmlinkage _kernel_main(void *info_table)
{
   
    printk("Hello World!\nThis is LulaOS");
}