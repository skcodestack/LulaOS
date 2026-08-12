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

/* ========== 全局 SMP 状态 ========== */

int smp_num_cpus = 1;           /* BSP 始终算一个 */
int cpu_online_map = 1;         /* bit 0 = BSP 在线 */
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
    unsigned int *p;

    /* ---- 1. 复制 trampoline 代码到物理 0x1000 ---- */
    trampoline_size = (unsigned int)(_trampoline_end - _trampoline_start);
    printk("SMP: trampoline code size = %u bytes\n", trampoline_size);

    if (trampoline_size > SMP_TRAMP_STACK_OFF) {
        printk("SMP: ERROR trampoline too large (%u > %u), SMP disabled\n",
               trampoline_size, SMP_TRAMP_STACK_OFF);
        return;
    }

    tramp_page = (unsigned char *)__va(SMP_TRAMPOLINE_PHYS);
    memcpy(tramp_page, _trampoline_start, trampoline_size);

    /* ---- 2. 写入运行时数据到 trampoline 页 ---- */

    /* start_secondary 虚拟地址（偏移 0x304，AP 开启分页后读取） */
    p = (unsigned int *)(tramp_page + SMP_TRAMP_ENTRY_OFF);
    *p = (unsigned int)start_secondary;

    /* 临时 GDTR（偏移 0x400）：limit + base（物理地址） */
    /* limit 已由汇编初始化，只需更新 base 为正确的物理地址 */
    p = (unsigned int *)(tramp_page + SMP_TRAMP_GDTR_OFF + 2); /* skip limit word */
    *p = SMP_TRAMPOLINE_PHYS + SMP_TRAMP_GDT_OFF;

    /* 临时 GDT（偏移 0x410）：已由汇编静态初始化（null + KCS + KDS） */

    /* ---- 3. 初始化 APIC ID 映射 ---- */
    for (i = 0; i < 256; i++)
        apicid_to_cpu[i] = 0;   /* 默认映射到 BSP */

    bsp_apic_id = GET_APIC_ID(apic_read(APIC_ID));
    apicid_to_cpu[bsp_apic_id] = 0;
    printk("SMP: BSP APIC ID = %d\n", bsp_apic_id);

    /* ---- 4. 遍历 ACPI LAPIC 列表，启动每个 AP ---- */
    for (i = 0; i < acpi_context.lapic_count; i++) {
        struct acpi_table_lapic *lapic = &acpi_context.lapics[i];
        struct page *stack_page;
        unsigned long stack_top;
        int cpu_id;
        int timeout;

        /* 跳过 BSP 和未启用的 LAPIC */
        if (lapic->id == bsp_apic_id)
            continue;
        if (!(lapic->flags.enabled))
            continue;

        /* 分配 AP 内核栈（1 页 = 4KB） */
        stack_page = __alloc_pages(0, 0);
        if (!stack_page) {
            printk("SMP: failed to allocate stack for AP %d, skipped\n",
                   lapic->id);
            continue;
        }
        stack_top = (unsigned long)stack_page->virtual + PAGE_SIZE;

        /* 写入 AP 栈顶虚拟地址到 trampoline 偏移 0x300 */
        p = (unsigned int *)(tramp_page + SMP_TRAMP_STACK_OFF);
        *p = stack_top;

        /* 分配逻辑 CPU 号 */
        cpu_id = next_cpu_id;
        apicid_to_cpu[lapic->id] = cpu_id;

        printk("SMP: booting AP %d (cpu %d, stack %p)\n",
               lapic->id, cpu_id, (void *)stack_top);

        /* 发送 INIT-SIPI-SIPI */
        send_startup_ipi(lapic->id, SMP_TRAMPOLINE_VECTOR);

        /* 等待 AP 上线（轮询 cpu_online_map，最多 ~100ms） */
        timeout = 100;
        while (!(cpu_online_map & (1 << cpu_id)) && timeout > 0) {
            udelay(1000);   /* 1ms */
            timeout--;
        }

        if (cpu_online_map & (1 << cpu_id)) {
            smp_num_cpus++;
            next_cpu_id++;
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
 * 执行环境：分页已开启，GDT/IDT 已加载，内核栈已由 BSP 分配。
 * 此时 current_p[cpu] 指向 init_task（由 sched_init 初始化）。
 *
 * 流程：
 *   1. 初始化本 CPU 的 Local APIC（remapping + enable + LVT + Timer + SPIV）
 *   2. 开中断
 *   3. 标记 CPU 在线
 *   4. 进入 idle 循环
 */
void start_secondary(void)
{
    int cpu = smp_processor_id();

    /* 初始化本 CPU 的 Local APIC */
    local_apic_init_ap();

    /* 标记在线 */
    cpu_online_map |= (1 << cpu);

    printk("SMP: cpu %d is up (APIC ID %d)\n",
           cpu, GET_APIC_ID(apic_read(APIC_ID)));

    /* 开中断 */
    sti();

    /* idle 循环 */
    for (;;)
        safe_halt();
}
