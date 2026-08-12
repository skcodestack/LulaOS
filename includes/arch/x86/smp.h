#ifndef __ARCH_X86_SMP_H__
#define __ARCH_X86_SMP_H__

#include <thread.h>

/*
 * SMP（Symmetric Multi-Processing）支持
 *
 * AP 启动流程：
 *   1. BSP 将 trampoline 代码复制到物理地址 SMP_TRAMPOLINE_PHYS
 *   2. BSP 向每个 AP 发送 INIT-SIPI-SIPI 序列
 *   3. AP 在实模式启动 → 切换保护模式 → 开启分页 → 调用 start_secondary()
 *   4. AP 初始化自己的 Local APIC / Timer，进入 idle 循环
 */

/* SIPI 目标：物理页 0x1000（与 setup.c 中 reserve_bootmem(PAGE_SIZE, PAGE_SIZE) 对应） */
#define SMP_TRAMPOLINE_PHYS    0x1000
#define SMP_TRAMPOLINE_VECTOR  (SMP_TRAMPOLINE_PHYS >> 12)  /* = 0x01 */

/* trampoline 页内数据偏移（BSP 写入，AP 读取）
 * 必须在 trampoline 代码结束之后（代码约 0x228 字节，取整至 0x300 起步） */
#define SMP_TRAMP_STACK_OFF    0x300   /* AP 内核栈顶（虚拟地址） */
#define SMP_TRAMP_ENTRY_OFF    0x304   /* start_secondary 入口地址 */
#define SMP_TRAMP_GDTR_OFF     0x400   /* 临时 GDTR（6 字节） */
#define SMP_TRAMP_GDT_OFF      0x410   /* 临时 GDT（3 * 8 = 24 字节） */

#define SMP_MAX_CPUS           NR_CPUS  /* 32 */

/* AP 上线超时（毫秒） */
#define SMP_AP_BOOT_TIMEOUT_MS 100

/* ========== 全局 SMP 状态 ========== */
extern int  smp_num_cpus;       /* 在线 CPU 总数 */
extern int  cpu_online_map;     /* 在线 CPU 位图（bit N = cpu N 在线） */

/* APIC ID → 逻辑 CPU 号映射（由 smp_init 填充） */
extern int  apicid_to_cpu[256];

/* ========== API ========== */

/*
 * smp_processor_id - 返回当前 CPU 的逻辑编号 [0, smp_num_cpus)
 *
 * 通过读取 Local APIC ID 并查表获得，每次调用约 ~100ns（MMIO 读）
 */
int smp_processor_id(void);

/*
 * smp_init - BSP 调用，启动所有 AP
 *
 * 必须在 mm_init() / kmem_cache_init() 之后调用（需要页分配器）
 */
void smp_init(void);

/*
 * start_secondary - AP 的 C 入口（由 trampoline 调用）
 *
 * 初始化本 CPU 的 Local APIC、Timer，标记在线，进入 idle 循环
 */
void start_secondary(void);

/* trampoline 代码边界符号（由 linker.lds 定义） */
extern char _trampoline_start[];
extern char _trampoline_end[];

#endif /* __ARCH_X86_SMP_H__ */
