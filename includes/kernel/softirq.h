/*
 * LulaOS 软中断（softirq）与 Tasklet 子系统
 *
 * kernel/softirq.c：
 *   - per-CPU softirq_pending 位掩码
 *   - irq_exit() 路径自动检测并执行 pending softirq
 *   - tasklet 通过 TASKLET_SOFTIRQ / HI_SOFTIRQ 向量驱动
 *   - ksoftirqd 内核线程防止软中断风暴
 */

#ifndef __KERNEL_SOFTIRQ_H__
#define __KERNEL_SOFTIRQ_H__

#include <arch/x86/smp.h>

/* ========== 软中断向量编号 ========== */
enum {
    HI_SOFTIRQ = 0,
    TIMER_SOFTIRQ,
    NET_TX_SOFTIRQ,
    NET_RX_SOFTIRQ,
    BLOCK_SOFTIRQ,
    TASKLET_SOFTIRQ,
    NR_SOFTIRQS
};

/* ========== softirq_action ========== */
struct softirq_action {
    void (*action)(struct softirq_action *);
    void *data;
};

/* ========== 软中断核心 API ========== */

/* 注册软中断向量 */
void open_softirq(int nr, void (*action)(struct softirq_action *), void *data);

/* 触发（置位）软中断 */
void raise_softirq(int nr);

/* 执行当前 CPU 所有 pending 软中断 */
void do_softirq(void);

/* 硬件中断上下文进入/退出 */
void irq_enter(void);
void irq_exit(void);

/* 初始化软中断子系统（注册 tasklet 向量，创建 BSP ksoftirqd） */
void softirq_init(void);

/* AP 上线后调用：创建本 CPU 的 ksoftirqd 线程 */
void ksoftirqd_init(void);

/* ========== Tasklet ========== */

#define TASKLET_STATE_SCHED  1   /* 已调度（在链表中等待执行） */
#define TASKLET_STATE_RUN    2   /* 正在执行（SMP 防并发） */

struct tasklet_struct {
    struct tasklet_struct *next;
    unsigned long state;       /* TASKLET_STATE_* 位掩码 */
    int count;                 /* disable 计数（>0 表示禁用） */
    void (*func)(unsigned long);
    unsigned long data;
};

/*
 * DECLARE_TASKLET - 静态声明并初始化一个 tasklet
 *
 * 用法：DECLARE_TASKLET(my_tasklet, my_func, my_data);
 */
#define DECLARE_TASKLET(name, _func, _data) \
    struct tasklet_struct name = { NULL, 0, 0, _func, _data }

/* 运行时初始化 tasklet */
void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long),
                  unsigned long data);

/* 调度 tasklet（普通优先级，通过 TASKLET_SOFTIRQ 执行） */
void tasklet_schedule(struct tasklet_struct *t);

/* 调度 tasklet（高优先级，通过 HI_SOFTIRQ 执行） */
void tasklet_hi_schedule(struct tasklet_struct *t);

/* 禁用 tasklet（count++，若正在运行则等待完成） */
void tasklet_disable(struct tasklet_struct *t);

/* 禁用 tasklet（count++，不等待当前执行完成） */
void tasklet_disable_nosync(struct tasklet_struct *t);

/* 启用 tasklet（count--） */
void tasklet_enable(struct tasklet_struct *t);

/* 立即执行指定 tasklet（跳过软中断路径，调试/特殊用途） */
void tasklet_action_now(struct tasklet_struct *t);

#endif /* __KERNEL_SOFTIRQ_H__ */
