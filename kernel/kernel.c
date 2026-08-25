#include <printk.h>
#include <stddef.h>
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
#include <kernel/softirq.h>

/* ========== 进程调度测试线程 ========== */

/*
 * test_thread_a / test_thread_b - 调度器测试线程
 *
 * 两个线程各自循环打印计数，验证调度器的时间片轮转是否正常工作。
 * 若调度器正常，两个线程的输出应当交替出现。
 */
static int test_thread_a(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 10; i++) {
        printk("[thread-A] loop %d  (pid=%d)\n", i, current->pid);
    }
    printk("[thread-A] done, exiting.\n");
    return 0;
}

static int test_thread_b(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 10; i++) {
        printk("[thread-B] loop %d  (pid=%d)\n", i, current->pid);
    }
    printk("[thread-B] done, exiting.\n");
    return 0;
}

void _kernel_init()
{
    _init_gdt();
}

/*
 * bsp_start_idle - BSP 初始化完成后，切换到 init_thread_union 栈并进入 idle 循环
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/head.S：
 *   movl $(init_thread_union+THREAD_SIZE), %esp
 *
 * 此函数设计为 noreturn：直接用内联汇编将 esp 切到
 * init_thread_union.stack 顶部（高地址），然后调用 smp_init() 和 cpu_idle().
 * 不再返回到旧栈，因此限制仅在初始化完成后调用一次。
 */
static __attribute__((noreturn)) void bsp_start_idle(void)
{
    /*
     * 切换 BSP 内核栈到 init_thread_union 顶部
     * init_thread_union 在 .data.init_task 节，8KB 对齐，
     * esp = 基址 + THREAD_SIZE = 栈顶。
     * 参考 Linux 2.6.20 arch/i386/kernel/head.S:
     *   movl $(init_thread_union+THREAD_SIZE), %esp
     */
    __asm__ __volatile__(
        "movl  $init_thread_union, %%esp\n\t"
        "addl  %0, %%esp\n\t"
        :
        : "i"(THREAD_SIZE)
        : "memory"
    );

    /* 初始化软中断子系统（kmem_cache_init 已完成，kmalloc 可用）*/
    softirq_init();

    /* 启动所有 AP（需要页分配器和 kmalloc 就绪） */
    smp_init();

    printk("Finished\n");

     /*
     * 调度器测试：创建两个内核线程
     * 它们会各自循环打印计数，验证时间片轮转调度是否正常工作。
     * 预期结果：thread-A 与 thread-B 的输出交替出现。
     */
    printk("sched_test: creating test threads...\n");
    kernel_thread(test_thread_a, NULL, 0);
    kernel_thread(test_thread_b, NULL, 0);
    /* 开中断，进入 idle 循环 */
    sti();
    cpu_idle();
 
}

asmlinkage void _kernel_main()
{  
    printk("This is LulaOS\n");
    
    init_cpu();

    setup_arch();

    _init_idt();
    _init_interrupts();
    
    sched_init();

    mm_init();

    kmem_cache_init(); 

    /*
     * 切换到 init_thread_union 内核栈并进入 idle 循环
     * 在此函数内调用 smp_init() 和 cpu_idle()
     */
    bsp_start_idle();

}