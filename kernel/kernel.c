#include <printk.h>
#include <stddef.h>
#include <device/device.h>
#include <device/platform.h>
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
#include <keyboard.h>
#include <mouse.h>
#include <i8042.h>
#include <arch/x86/acpi.h>
#include <pci/pci.h>
#include <usb/usb.h>
#include <usb/uhci.h>
#include <drm/drm_core.h> 
 
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
     * 初始化 vmalloc 起始地址：从 fb_ioremap 映射结束位置
     * 按 4MB 对齐开始，避免与 framebuffer 虚拟地址冲突。
     * 必须在 fbcon_init() 之后调用。
     */
    vmalloc_area_init();

    /* 注册 Platform 总线（键盘/鼠标等设备挂在此总线上） */
    platform_bus_init();

    /* ACPI DSDT 枚举：扫描 AML 发现 Platform 设备（PNP0303/PNP0F13 等） */
    acpi_register_platform_devices();

    /* PS/2 控制器初始化（注册 i8042 设备+驱动，probe 完成硬件初始化） */
    i8042_init();

    /* 注册键盘驱动（匹配 ACPI 发现的 PNP0303 设备） */
    keyboard_init();

    /* 注册鼠标驱动（匹配 ACPI 发现的 PNP0F13 设备） */
    mouse_init();

    /* PCI 总线枚举（需要 kmalloc 就绪） */
    pci_init();

    /* USB 总线注册（注册 usb_bus_type，须先于 UHCI 驱动） */
    usb_init();

    /* UHCI 主机控制器 PCI 驱动（发现并初始化 UHCI 控制器，创建 Root Hub） */
    uhci_init();

    /* DRM 核心框架初始化 */
    drm_core_init();

    /* Bochs/QEMU VBE DRM 驱动（发现 VGA 设备并初始化 KMS） */
    drm_bochs_init();

    /*
     * 切换到 init_thread_union 内核栈并进入 idle 循环
     * 在此函数内调用 smp_init() 和 cpu_idle()
     */
    bsp_start_idle();

}