#include <printk.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <arch/linkage.h>
#include <interrupts/interrupts.h>
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/setup.h>
#include <arch/x86/cpu.h>
#include <arch/x86/system.h>
#include <kernel/sched.h>
#include <arch/x86/smp.h>

void _kernel_init()
{
    _init_gdt();
}

asmlinkage void _kernel_main()
{  
    printk("This is LulaOS\n");
    
    init_cpu();

    setup_arch();

    _init_idt();
    _init_interrupts();
    
    sched_init();
    /* softirq_init(); */

    mm_init();

    kmem_cache_init();

    /* 启动所有 AP（需要页分配器来分配 AP 内核栈） */
    smp_init();

    printk("Finished\n");

    /* 所有子系统初始化完成，开启 CPU 中断 */
    sti();

    /* 空闲循环：CPU 无事可做时 halt，等待下一个中断唤醒 */
    for (;;)
        safe_halt();
}