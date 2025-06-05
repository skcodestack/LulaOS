#include <printk.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <arch/linkage.h>
#include <interrupts/interrupts.h>
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/setup.h>
#include <arch/x86/setup.h>

void _kernel_init()
{
    _init_gdt();
    _init_idt();
    _init_interrupts();
}

asmlinkage _kernel_main()
{  
    printk("Hello World!\nThis is LulaOS\n");
    
    setup_arch();

    


}