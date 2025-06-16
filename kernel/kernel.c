#include <printk.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <arch/linkage.h>
#include <interrupts/interrupts.h>
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/setup.h>
#include <arch/x86/system.h>
#include <mm/slab.h>
#include <arch/x86/cpu.h>

void _kernel_init()
{
    _init_gdt();
}

asmlinkage void  _kernel_main()
{  
    printk("Hello World!\nThis is LulaOS\n");
    
    init_cpu();
    setup_arch();

    _init_idt();
    _init_interrupts(); 
    //sched_init,softirq_init,kmem_cache_init

    kmem_cache_init();
    sti();
    mm_init();
    kmem_cache_sizes_init();

    
    printk("Finished\n");



}