/*
 * LulaOS 软中断（softirq）与 Tasklet 子系统实现
 *
 *  kernel/softirq.c：
 *
 * 中断处理层次：
 *   Top Half（硬中断）：entry.S → interrupt_warpper → do_IRQ()
 *     → irq_enter()  in_irq++
 *     → handler 设备驱动顶半部（可调用 raise_softirq）
 *     → irq_exit()   in_irq--；若 in_irq==0 且 pending!=0 → do_softirq()
 *
 *   Bottom Half（软中断）：do_softirq()
 *     → 读+清零 pending
 *     → 开中断，依次执行各置位向量的 action
 *     → 若执行期间又被 raise → 唤醒 ksoftirqd（进程上下文）
 *
 * Tasklet：通过 TASKLET_SOFTIRQ / HI_SOFTIRQ 向量延迟执行，
 *   由 tasklet_action() 在软中断上下文中遍历链表执行
 *
 * ksoftirqd：每 CPU 一个内核线程，处理软中断风暴（handler 持续 raise）
 */

#include <kernel/softirq.h>
#include <kernel/sched.h>
#include <arch/x86/system.h>
#include <printk.h>
#include <stddef.h>

/* ========== 全局数据 ========== */

/* 软中断向量表 */
static struct softirq_action softirq_vec[NR_SOFTIRQS];

/* per-CPU 软中断 pending 位掩码 */
static unsigned long softirq_pending[NR_CPUS];

/* per-CPU 硬件中断嵌套计数 */
static int in_irq[NR_CPUS];

/* 软中断执行深度（防止递归：do_softirq 期间再次触发则唤醒 ksoftirqd） */
static int in_softirq;

/* per-CPU ksoftirqd 线程指针 */
static struct task_struct *ksoftirqd_thread[NR_CPUS];

/* ========== 软中断核心 ========== */

/*
 * open_softirq - 注册软中断向量
 * @nr:    向量编号（HI_SOFTIRQ..TASKLET_SOFTIRQ）
 * @action: 处理函数
 * @data:  传给 action 的私有数据
 */
void open_softirq(int nr, void (*action)(struct softirq_action *), void *data)
{
    if (nr < 0 || nr >= NR_SOFTIRQS)
        return;
    softirq_vec[nr].action = action;
    softirq_vec[nr].data   = data;
}

/*
 * raise_softirq - 触发（置位）指定软中断
 *
 * 在关中断（或 irq_enter/irq_exit 保护）下原子置位 pending，
 * 下一次 irq_exit() 时 do_softirq() 会执行对应 action。
 */
void raise_softirq(int nr)
{
    unsigned long flags;
    int cpu;

    local_irq_save(flags);
    cpu = smp_processor_id();
    softirq_pending[cpu] |= (1UL << nr);
    local_irq_restore(flags);
}

/*
 * do_softirq - 执行当前 CPU 所有 pending 软中断
 *
 * 流程：
 *   1. 若已在软中断上下文中（in_softirq > 0），直接返回（防递归）
 *   2. 读+清零 pending 位掩码
 *   3. 开中断，依次执行各置位向量的 action
 *   4. 关中断，恢复 in_softirq
 *   5. 若执行期间软中断再次被 raise → 唤醒 ksoftirqd
 */
void do_softirq(void)
{
    unsigned long pending, flags;
    int cpu, i;

    if (in_softirq)
        return;

    local_irq_save(flags);
    cpu = smp_processor_id();
    pending = softirq_pending[cpu];
    if (!pending) {
        local_irq_restore(flags);
        return;
    }
    softirq_pending[cpu] = 0;   /* 读+清零 */
    in_softirq++;
    local_irq_restore(flags);   /* 开中断执行软中断 */

    for (i = 0; i < NR_SOFTIRQS; i++) {
        if ((pending & (1UL << i)) && softirq_vec[i].action)
            softirq_vec[i].action(&softirq_vec[i]);
    }

    local_irq_save(flags);
    in_softirq--;

    /* 执行期间若又被 raise，唤醒 ksoftirqd 在进程上下文中处理 */
    if (softirq_pending[cpu] && ksoftirqd_thread[cpu]) {
        struct task_struct *k = ksoftirqd_thread[cpu];
        if (k->state != TASK_RUNNING)
            wake_up_process(k);
    }
    local_irq_restore(flags);
}

/*
 * irq_enter - 进入硬件中断上下文
 *
 * 每次硬件中断触发时在 handler 之前调用，递增嵌套计数，
 * 防止 irq_exit 在嵌套中断时错误触发软中断。
 */
void irq_enter(void)
{
    int cpu = smp_processor_id();
    in_irq[cpu]++;
}

/*
 * irq_exit - 退出硬件中断上下文
 *
 * 递减嵌套计数；若归零（最外层中断退出）且 softirq_pending 非零，
 * 则调用 do_softirq() 处理所有 pending 软中断。
 */
void irq_exit(void)
{
    int cpu = smp_processor_id();
    in_irq[cpu]--;
    if (in_irq[cpu] == 0 && softirq_pending[cpu])
        do_softirq();
}

/* ========== ksoftirqd 内核线程 ========== */

/*
 * ksoftirqd - per-CPU 软中断守护线程
 *
 * 当 do_softirq() 执行一轮后 pending 再次被置位（handler 持续 raise），
 * 唤醒此线程在进程上下文中持续处理，避免中断上下文长时间霸占 CPU。
 *
 * 参考 Linux 2.6.20 kernel/softirq.c ksoftirqd()
 */
static int ksoftirqd(void *arg)
{
    (void)arg;
    for (;;) {
        /* 无 pending 时睡眠，等待 do_softirq 唤醒 */
        if (!softirq_pending[smp_processor_id()])
            interruptible_sleep();

        /* 有 pending，循环处理直到清空 */
        while (softirq_pending[smp_processor_id()])
            do_softirq();
    }
    return 0;
}

/*
 * ksoftirqd_init - 为当前 CPU 创建 ksoftirqd 线程
 *
 * BSP 在 softirq_init() 中调用（为自己创建）；
 * AP 在 start_secondary() 中调用（为自己创建）。
 *
 * 不指定 CPU 亲和性（LulaOS 无此机制），线程在当前 CPU 上运行，
 * 通过 smp_processor_id() 读取自己的 pending 位。
 */
void ksoftirqd_init(void)
{
    int cpu = smp_processor_id();
    ksoftirqd_thread[cpu] = kernel_thread(ksoftirqd, NULL, 0);
    printk("softirq: ksoftirqd/%d created (pid=%d)\n",
           cpu, ksoftirqd_thread[cpu]->pid);
}

/* ========== Tasklet 实现 ==========
 *
 * Linux 使用 per-CPU tasklet 队列（DEFINE_PER_CPU），
 * tasklet_schedule() 将 tasklet 链入当前 CPU 的链表，
 * tasklet_action() 只消费当前 CPU 的链表，保证：
 *   - CPU A 调度的 tasklet 只会在 CPU A 的 do_softirq() 中执行
 *   - 不会跨 CPU 重复执行
 *
 * LulaOS 简化：使用 NR_CPUS 索引数组代替 DEFINE_PER_CPU
 */

/* per-CPU 普通优先级 tasklet 链表头 */
static struct tasklet_struct *tasklet_head[NR_CPUS];

/* per-CPU 高优先级 tasklet 链表头 */
static struct tasklet_struct *tasklet_hi_head[NR_CPUS];

void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long),
                  unsigned long data)
{
    t->next   = NULL;
    t->state  = 0;
    t->count  = 0;
    t->func   = func;
    t->data   = data;
}

/*
 * tasklet_schedule - 调度 tasklet（TASKLET_SOFTIRQ 优先级）
 *
 * 将 tasklet 链入当前 CPU 的 tasklet_head，并置位 TASKLET_SOFTIRQ。
 * 若 tasklet 已在链表中（STATE_SCHED 已置位），不重复追加。
 * 参考 Linux 2.6.20 kernel/softirq.c tasklet_schedule()
 */
void tasklet_schedule(struct tasklet_struct *t)
{
    unsigned long flags;
    int cpu;

    local_irq_save(flags);
    cpu = smp_processor_id();
    if (!(t->state & TASKLET_STATE_SCHED)) {
        t->state |= TASKLET_STATE_SCHED;
        t->next            = tasklet_head[cpu];
        tasklet_head[cpu]  = t;
    }
    local_irq_restore(flags);
    raise_softirq(TASKLET_SOFTIRQ);
}

/*
 * tasklet_hi_schedule - 调度 tasklet（HI_SOFTIRQ 优先级）
 *
 * 将 tasklet 链入当前 CPU 的 tasklet_hi_head，
 * 通过 HI_SOFTIRQ 执行，优先级高于 TASKLET_SOFTIRQ。
 */
void tasklet_hi_schedule(struct tasklet_struct *t)
{
    unsigned long flags;
    int cpu;

    local_irq_save(flags);
    cpu = smp_processor_id();
    if (!(t->state & TASKLET_STATE_SCHED)) {
        t->state |= TASKLET_STATE_SCHED;
        t->next              = tasklet_hi_head[cpu];
        tasklet_hi_head[cpu] = t;
    }
    local_irq_restore(flags);
    raise_softirq(HI_SOFTIRQ);
}

/*
 * tasklet_disable_nosync - 禁用 tasklet（不等待当前执行完成）
 *
 * count++ 后 tasklet_action() 会跳过此 tasklet 并重新调度。
 * 用于需要临时屏蔽 tasklet 执行的关键区。
 */
void tasklet_disable_nosync(struct tasklet_struct *t)
{
    unsigned long flags;
    local_irq_save(flags);
    t->count++;
    local_irq_restore(flags);
}

/*
 * tasklet_disable - 禁用 tasklet（若正在执行则等待完成）
 *
 * count++ 后自旋等待 STATE_RUN 清除（SMP 下另一个 CPU 正在执行该 tasklet）。
 * 单 CPU 系统中 STATE_RUN 不会与当前上下文并发，等价于 disable_nosync。
 */
void tasklet_disable(struct tasklet_struct *t)
{
    tasklet_disable_nosync(t);
    /* SMP：等待另一个 CPU 完成该 tasklet 的执行 */
    while (t->state & TASKLET_STATE_RUN)
        barrier();
}

/*
 * tasklet_enable - 启用 tasklet（count--）
 *
 * count 归零后，下次 tasklet_schedule() 可正常执行。
 * 若 count 归零时 STATE_SCHED 已置位，重新 raise 使其尽快执行。
 */
void tasklet_enable(struct tasklet_struct *t)
{
    unsigned long flags;
    local_irq_save(flags);
    if (t->count > 0)
        t->count--;
    local_irq_restore(flags);
    /* count 归零后若有待处理的调度，重新触发 */
    if (t->state & TASKLET_STATE_SCHED)
        raise_softirq(TASKLET_SOFTIRQ);
}

/*
 * tasklet_action_now - 立即在当前上下文执行指定 tasklet
 *
 * 不经过软中断路径，直接在调用者上下文中执行 func(data)。
 * 用于调试或需要立即执行的特殊场景。
 */
void tasklet_action_now(struct tasklet_struct *t)
{
    if (t->count)
        return;
    t->state |= TASKLET_STATE_RUN;
    t->func(t->data);
    t->state &= ~TASKLET_STATE_RUN;
    t->state &= ~TASKLET_STATE_SCHED;
}

/*
 * do_tasklet_list - 遍历并执行 tasklet 链表
 *
 * 原子取出整个链表头并清零，然后遍历每个 tasklet：
 *   - count > 0（被禁用）：清除 STATE_SCHED，不执行，不重新调度
 *   - count == 0：执行 func(data)，清除 STATE_SCHED 和 STATE_RUN
 *
 * 注意：被禁用的 tasklet 不会重新调度，下次 tasklet_schedule() 时
 * 会重新链入链表，count 归零后（tasklet_enable）触发执行。
 */
static void do_tasklet_list(struct tasklet_struct **head_ptr)
{
    struct tasklet_struct *list, *t;
    unsigned long flags;

    /* 原子取出链表头并清零 */
    local_irq_save(flags);
    list = *head_ptr;
    *head_ptr = NULL;
    local_irq_restore(flags);

    while (list) {
        t = list;
        list = list->next;

        if (t->count) {
            /* 被禁用：清除 STATE_SCHED，不执行，等待 tasklet_enable 重新调度 */
            t->state &= ~TASKLET_STATE_SCHED;
            continue;
        }

        t->state |= TASKLET_STATE_RUN;
        t->state &= ~TASKLET_STATE_SCHED;
        t->func(t->data);
        t->state &= ~TASKLET_STATE_RUN;
    }
}

/* TASKLET_SOFTIRQ 处理函数（注册到 softirq_vec[TASKLET_SOFTIRQ]）
 * 只处理当前 CPU 的 tasklet 链表，不跨 CPU */
static void tasklet_action(struct softirq_action *a)
{
    int cpu = smp_processor_id();
    (void)a;
    do_tasklet_list(&tasklet_head[cpu]);
}

/* HI_SOFTIRQ 处理函数（注册到 softirq_vec[HI_SOFTIRQ]）
 * 只处理当前 CPU 的高优先级 tasklet 链表 */
static void tasklet_hi_action(struct softirq_action *a)
{
    int cpu = smp_processor_id();
    (void)a;
    do_tasklet_list(&tasklet_hi_head[cpu]);
}

/* ========== 初始化 ========== */

/*
 * softirq_init - 初始化软中断子系统
 *
 * 1. 清零向量表（BSS 已是零，显式清零保险）
 * 2. 注册 HI_SOFTIRQ → tasklet_hi_action
 * 3. 注册 TASKLET_SOFTIRQ → tasklet_action
 * 4. 为 BSP 创建 ksoftirqd 内核线程
 *
 * 须在 kmem_cache_init() 之后、sti() 之前调用（需要 kmalloc 来创建线程）
 */
void softirq_init(void)
{
    int i;
    for (i = 0; i < NR_SOFTIRQS; i++) {
        softirq_vec[i].action = NULL;
        softirq_vec[i].data   = NULL;
    }

    open_softirq(HI_SOFTIRQ,      tasklet_hi_action, NULL);
    open_softirq(TASKLET_SOFTIRQ, tasklet_action,    NULL);

    /* 为 BSP 创建 ksoftirqd */
    ksoftirqd_init();

    printk("softirq: softirq subsystem initialized\n");
}
