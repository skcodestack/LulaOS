#include <printk.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <arch/linkage.h>
#include <interrupts/interrupts.h>
#include <arch/x86/boot/multiboot.h>
void _kernel_init()
{
    _init_gdt();
    _init_idt();
    _init_interrupts();
}

asmlinkage _kernel_main(multiboot_info_t *info_table)
{  
    printk("Hello World!\nThis is LulaOS");
    printk("Multiboot info table: %d\n", info_table->mem_lower);
}