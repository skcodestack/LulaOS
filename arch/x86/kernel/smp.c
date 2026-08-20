/*
 * LulaOS SMP（Symmetric Multi-Processing）初始化
 *
 * BSP 启动流程：
 *   1. 复制 trampoline 代码到物理 0x1000
 *   2. 写入临时 GDTR/GDT、start_secondary 入口、AP 栈地址
 *   3. 遍历 ACPI MADT 中的 LAPIC 列表，对每个 AP 发送 INIT-SIPI-SIPI
 *   4. 等待 AP 上线（轮询 cpu_online_map 或超时）
 *
 * AP 启动流程：
 *   1. trampoline 实模式 → 保护模式 → 分页 → call start_secondary()
 *   2. start_secondary 初始化 LAPIC/Timer，标记在线，进入 idle 循环
 */

#include <arch/x86/smp.h>
#include <arch/x86/apic.h>
#include <arch/x86/acpi.h>
#include <arch/x86/page.h>
#include <arch/x86/system.h>
#include <libs/memcpy.h>
#include <printk.h>
#include <mm/mmzone.h>
#include <mm/slab.h>
#include <kernel/sched.h>

/* 链接器符号（entry.S / linker.lds 导出），用于定位 GDT/GDTR 在 trampoline 页内的偏移 */
extern unsigned char tramp_gdt[];
extern unsigned char tramp_gdtr[];

int smp_num_cpus = 1;           /* BSP 始终算一个 */
int cpu_online_map = 1;         /* bit 0 = BSP 在线 */

/* AP 启动栈描述符（参考 Linux 2.6.20 arch/i386/kernel/head.S stack_start）
 * BSP 对每个 AP 分配 thread_union 后，在发 SIPI 前写入 esp。
 * AP 在 trampoline 开启分页后直接执行 movl ap_stack_start, %%esp。 */
struct ap_stack_start_t ap_stack_start = {
    .esp = 0,
    .ss  = 0x10,   /* __KERNEL_DS */
};

/* BSP ↔ AP 握手变量（参考 Linux 2.6.20 smpboot.c 第 89-90 行）
 * volatile 防止编译器优化掉轮询循环中的内存读取 */
volatile int cpu_callout_map = 0;  /* BSP → AP：BSP 发完 IPI 后置位 */
volatile int cpu_callin_map  = 0;  /* AP → BSP：AP 完成初始化后置位 */

int apicid_to_cpu[256];         /* APIC ID → 逻辑 CPU 号 */

/* 下一个可用逻辑 CPU 号 */
static int next_cpu_id = 1;

/* BSP 的 APIC ID（在 smp_init 中记录） */
static unsigned int bsp_apic_id;

/* ========== API ========== */

/*
 * smp_processor_id - 返回当前 CPU 的逻辑编号 [0, smp_num_cpus)
 *
 * 通过读取 Local APIC ID 并查 apicid_to_cpu[] 获得。
 * 每次调用约 ~100ns（MMIO 读 LAPIC ID 寄存器）
 */
int smp_processor_id(void)
{
    unsigned int id = GET_APIC_ID(apic_read(APIC_ID));
    return apicid_to_cpu[id];
}

/*
 * smp_init - BSP 调用，启动所有 AP
 *
 * 必须在 mm_init() / kmem_cache_init() 之后调用（需要页分配器来分配 AP 栈）
 */
void smp_init(void)
{
    unsigned int i;
    unsigned int trampoline_size;
    unsigned char *tramp_page;
    /* ---- 1. 复制 trampoline 代码到物理 0x1000 ---- */
    trampoline_size = (unsigned int)(_trampoline_end - _trampoline_start);
    printk("SMP: trampoline code size = %u bytes\n", trampoline_size);

    tramp_page = (unsigned char *)__va(SMP_TRAMPOLINE_PHYS);
    memcpy(tramp_page, _trampoline_start, trampoline_size);

    /* ---- 2. 更新 trampoline 页内的 GDTR base 字段 ---- */

    /* 临时 GDTR：limit 已由汇编初始化，只更新 base 字段为正确物理地址
     * 偏移由链接器符号 _trampoline_gdtr 确定（紧跟代码之后，无 .org 填充） */
    {
        unsigned int gdt_phys = SMP_TRAMPOLINE_PHYS
                              + ((unsigned int)tramp_gdt - (unsigned int)_trampoline_start);
        unsigned int gdtr_off = (unsigned int)tramp_gdtr - (unsigned int)_trampoline_start;
        /* GDTR 格式：[limit:2][base:4]，base 在偏移 +2 处 */
        *(unsigned int *)(tramp_page + gdtr_off + 2) = gdt_phys;
    }

    /* ---- 3. 初始化 APIC ID 映射 ---- */
    for (i = 0; i < 256; i++)
        apicid_to_cpu[i] = 0;   /* 默认映射到 BSP */

    bsp_apic_id = GET_APIC_ID(apic_read(APIC_ID));
    apicid_to_cpu[bsp_apic_id] = 0;
    printk("SMP: BSP APIC ID = %d\n", bsp_apic_id);

    /* ---- 4. 遍历 ACPI LAPIC 列表，启动每个 AP ---- */
    for (i = 0; i < acpi_context.lapic_count; i++) {
        struct acpi_table_lapic *lapic = &acpi_context.lapics[i];
        int cpu_id;
        int timeout;

        /* 跳过 BSP 和未启用的 LAPIC */
        if (lapic->id == bsp_apic_id)
            continue;
        if (!(lapic->flags.enabled))
            continue;

        /* 为 AP 分配独立的 thread_union（task_struct + 内核栈）
         *
         * 参考 Linux 2.6.20 arch/i386/kernel/smpboot.c alloc_idle_task() / fork_idle():
         *   idle task 需要独立的 task_struct 和内核栈，避免多 CPU 共享上下文
         *
         * kmalloc(THREAD_SIZE) 使用 order=1 伙伴系统分配 2 页，
         * 返回 8KB 对齐地址，满足 current 宏 esp屏蔽需求
         */
        union thread_union *tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
        if (!tu) {
            printk("SMP: failed to allocate thread_union for AP %d, skipped\n",
                   lapic->id);
            continue;
        }

        struct task_struct *idle = &tu->task;
        /* 从 BSP init_task 复制基本字段，然后覆盖 AP 特定字段 */
        *idle = init_task;
        idle->pid   = 0;           /* idle 进程 pid=0 */
        idle->flags = 0;           /* 清除 PF_NEVER_STARTED */
        INIT_LIST_HEAD(&idle->run_list);
        INIT_LIST_HEAD(&idle->tasks);

        /* AP 内核栈顶 = thread_union 基址 + THREAD_SIZE */
        unsigned long stack_top = (unsigned long)tu + THREAD_SIZE;

        /* 写入 AP 栈顶地址到全局变量（参考 Linux 2.6.20 smpboot.c 第 979 行）
         * AP 在 trampoline 开启分页后直接通过 movl ap_stack_start, %%esp 加载 */
        ap_stack_start.esp = stack_top;

        /* 【关键】同步设置 idle->thread.esp，供调度器 __switch_to 切换回 idle 时恢复
         * 若不设置，__switch_to 从 thread.esp=0（INIT_THREAD 初始值）恢复 ESP，
         * 导致下次中断到来时 ESP=0 → push 写到 0xFFFFFFFC → #PF → Triple Fault
         * ex */
        idle->thread.esp = stack_top;

        /* 【关键】BSP 刷缓存，确保 ap_stack_start.esp 回写主存
         * BSP 写完后数据在 L1 cache 中，若不刷新，AP 从主存读到的是旧值 0。
         * wbinvd 强制把 BSP 所有 dirty cache line 回写并作废，
         * 之后 AP 读 ap_stack_start 必然从主存获得正确值。 */
        __asm__ volatile ("wbinvd" ::: "memory");

        /* 分配逻辑 CPU 号 */
        cpu_id = next_cpu_id;
        apicid_to_cpu[lapic->id] = cpu_id;

        printk("SMP: booting AP %d (cpu %d, stack %p)\n",
               lapic->id, cpu_id, (void *)stack_top);

        /* 发送 INIT-SIPI-SIPI
         * 参考 Linux 2.6.20 do_boot_cpu() 第 989-1000 行:
         *   清除 callin/callout → 发送 IPI → 设 callout 通知 AP */
        cpu_callout_map = 0;
        cpu_callin_map  = 0;

        send_startup_ipi(lapic->id, SMP_TRAMPOLINE_VECTOR, cpu_id);

        /* BSP 通知 AP：可以继续初始化（参考 Linux cpu_set(cpu, cpu_callout_map)） */
        cpu_callout_map |= (1 << cpu_id);

        /* 等待 AP 完成初始化（参考 Linux 第 1013-1017 行的 ~5s 超时轮询）
         * AP 在 start_secondary() 中完成 APIC 初始化后会设 callin_map */
        timeout = 5000;
        while (!(cpu_callin_map & (1 << cpu_id)) && timeout > 0) {
            udelay(1000);   /* 1ms */
            timeout--;
        }

        if (cpu_callin_map & (1 << cpu_id)) {
            smp_num_cpus++;
            next_cpu_id++;
            /* 将 AP 的 idle task 注册到对应 CPU 的 runqueue */
            {
                extern runqueue_t runqueues[];
                runqueues[cpu_id].idle = idle;
            }
            printk("SMP: AP %d (cpu %d) is online\n", lapic->id, cpu_id);
        } else {
            printk("SMP: AP %d (cpu %d) did not respond, timeout\n",
                   lapic->id, cpu_id);
        }
    }

    printk("SMP: %d CPUs online (total %d)\n", smp_num_cpus, smp_num_cpus);
}

/*
 * start_secondary - AP 的 C 入口（由 trampoline 调用）
 *
 * 参考 Linux 2.6.20 start_secondary() (smpboot.c 第 541-588 行)
 *             + smp_callin()   (smpboot.c 第 367-454 行)
 *
 * 执行环境：分页已开启，GDT/IDT 已加载，内核栈已由 BSP 分配。
 * 此时 current 宏通过 esp & ~0x1FFF 定位，指向内核栈所在的 thread_union.task。
 *
 * 流程（参考 Linux）：
 *   1. 尽早设 cpu_online_map（阻止 BSP 发送第二个 SIPI）
 *   2. 等待 cpu_callout_map（BSP 通知可以继续）
 *   3. 初始化 Local APIC / Timer
 *   4. 设 cpu_callin_map（通知 BSP 初始化完成）
 *   5. 开中断，进入 idle 循环
 */
void start_secondary(void)
{
    int cpu = smp_processor_id();

    /* 【关键】尽早标记在线，阻止 BSP 发送第二个 SIPI
     * 参考问题分析：若 BSP 在 AP 未完成初始化时发送 SIPI2，
     * 会将 AP 强制复位回实模式，破坏栈/分页状态 → Triple Fault
     * 在 local_apic_init_ap() 之前设此标志，使 send_startup_ipi()
     * 中的 cpu_online_map 检查能够生效，跳过 SIPI2 */
    cpu_online_map |= (1 << cpu);

    /* 等待 BSP 的 callout 信号
     * 参考 Linux smp_callin() 第 404-411 行:
     *   AP 在收到 callout 之前不做任何初始化，
     *   确保 BSP 已完成 IPI 发送并准备好接受 AP 的响应 */
    while (!(cpu_callout_map & (1 << cpu)))
        ;

    /* 初始化本 CPU 的 Local APIC
     * 参考 Linux smp_callin() 第 428 行 setup_local_APIC() */
    local_apic_init_ap();

    /* 通知 BSP：AP 初始化完成
     * 参考 Linux smp_callin() 第 447 行 cpu_set(cpuid, cpu_callin_map)
     * BSP 在 smp_init() 中轮询此标志，见到后才标记 AP 成功 */
    cpu_callin_map |= (1 << cpu);

    printk("SMP: cpu %d is up (APIC ID %d)\n",
           cpu, GET_APIC_ID(apic_read(APIC_ID)));

    /* 开中断 */
    sti();

    /*
     * 进入 idle 循环
     * 参考 Linux 2.6.20 start_secondary() 第 586-587 行:
     *   wmb(); cpu_idle();
     * current 宏通过 esp & ~0x1FFF 自动指向本 CPU 的 AP idle task
     */
    cpu_idle();
}
