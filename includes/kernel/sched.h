/*
 * LulaOS 进程调度器
 *
 * 基于 Linux 的简化优先级轮转调度器：
 *   - thread_union 将 task_struct 与内核栈合并到同一 THREAD_SIZE 块
 *   - current 宏通过 esp & ~(THREAD_SIZE-1) 直接定位 task_struct
 *   - 静态优先级（priority）越高，基础时间片越大
 *   - 时间片耗尽后移入队列尾部，等待下一轮
 *   - 单 CPU 实现，多核预留 NR_CPUS
 */

#ifndef __KERNEL_SCHED_H__
#define __KERNEL_SCHED_H__

#include <libs/list.h>
#include <thread.h>
#include <arch/x86/spinlock.h>
#include <arch/x86/ptrace.h>
#include <arch/x86/smp.h>

/* ========== 线程堆大小 ========== */
#define THREAD_SIZE     8192
#define THREAD_MASK     (~(THREAD_SIZE - 1))   /* ~0x1FFF，用于 esp 屏蔽 */

/* ========== 任务状态 ========== */
#define TASK_RUNNING         0   /* 可运行（在 runqueue 中） */
#define TASK_INTERRUPTIBLE   1   /* 可中断睡眠 */
#define TASK_UNINTERRUPTIBLE 2   /* 不可中断睡眠 */
#define TASK_ZOMBIE          4   /* 已退出，等待回收 */
#define TASK_STOPPED         8   /* 已停止 */

/* ========== 优先级 ========== */
#define MAX_PRIO        40
#define DEF_PRIO        20
#define MIN_TIMESLICE   5
#define MAX_TIMESLICE   200

/* ========== PF 标志 ========== */
#define PF_NEVER_STARTED  0x00000001  /* 任务从未被调度过 */

/* ========== 进程描述符 ========== */
struct task_struct {
    /* ---- 调度信息 ---- */
    volatile long       state;       /* TASK_*  */
    unsigned long       priority;    /* 静态优先级 [0, MAX_PRIO) */
    unsigned long       counter;     /* 剩余时间片（tick） */
    unsigned long       timeslice;   /* 基础时间片 */
    long                need_resched;/* 非零 → 需要重新调度 */

    /* ---- 进程标识 ---- */
    int                 pid;
    char                comm[16];

    /* ---- 链表节点 ---- */
    struct list_head    run_list;    /* 运行队列节点 */
    struct list_head    tasks;       /* 全局任务链表节点 */

    /* ---- CPU 上下文 ---- */
    thread_struct       thread;

    /* ---- 其他 ---- */
    unsigned long       flags;       /* PF_* */
    struct pt_regs     *pt_regs;     /* 内核栈顶保存的寄存器快照 */
};

/*
 * thread_union - task_struct 与内核栈合并到同一 8KB 块
 *
 * 参考 Linux 2.6.20 include/linux/sched.h union thread_union
 * 内存布局：
 *   [低地址] task_struct（占用小部分）
 *   [... 空闲 ...]
 *   [高地址] 内核栈（向下增长）
 */
union thread_union {
    struct task_struct task;                         /* 位于 union 起始 */
    unsigned long stack[THREAD_SIZE / sizeof(long)]; /* 占满整个 8KB */
};

/* ========== 运行队列 ========== */
typedef struct {
    spinlock_t          lock;
    struct list_head    queue;       /* 可运行任务链表 */
    unsigned int        nr_running;
    struct task_struct *idle;        /* 空闲任务 */
} runqueue_t;

/* ========== 当前进程（current 宏）========== */

/*
 * get_current() - 通过 esp & ~(THREAD_SIZE-1) 定位当前 task_struct
 *
 * 原理：每个进程的 task_struct 在 thread_union 起始，
 *   该块 8KB 对齐，因此 esp 屏蔽低 13 位可得到基址。
 * 参考 Linux 2.6.20 include/asm-i386/thread_info.h current_thread_info()
 */
static inline struct task_struct *get_current(void)
{
    struct task_struct *cur;
    __asm__("andl %%esp, %0" : "=r"(cur) : "0"(~(THREAD_SIZE - 1)));
    return cur;
}
#define current  get_current()

/* ========== 初始化宏 ========== */
#define INIT_TASK(tsk) { \
    .state       = TASK_RUNNING, \
    .priority    = DEF_PRIO, \
    .counter     = MAX_TIMESLICE, \
    .timeslice   = MAX_TIMESLICE, \
    .need_resched = 0, \
    .pid         = 0, \
    .comm        = "idle", \
    .run_list    = { &(tsk).run_list, &(tsk).run_list }, \
    .tasks       = { &(tsk).tasks,  &(tsk).tasks  }, \
    .thread      = INIT_THREAD, \
    .flags       = 0, \
    .pt_regs     = (void *)0 \
}

/* BSP 的初始 thread_union（.data.init_task 节，8KB 对齐） */
extern union thread_union init_thread_union;
#define init_task  (init_thread_union.task)

/* ========== 调度器 API ========== */
void sched_init(void);
void schedule(void);
void scheduler_tick(void);
void cpu_idle(void);

/* 进程创建 */
int  kernel_thread(int (*fn)(void *), void *arg, unsigned long flags);
int  sys_fork(struct pt_regs *regs);

/* 进程退出/睡眠 */
void do_exit(long code);
void interruptible_sleep(void);

/* ========== 辅助函数 ========== */

/* 将任务加入运行队列（尾部）- per-CPU：加入当前 CPU 的 runqueue */
static inline void add_task_to_runqueue(struct task_struct *p)
{
    extern runqueue_t runqueues[NR_CPUS];
    runqueue_t *rq = &runqueues[smp_processor_id()];
    list_add_tail(&p->run_list, &rq->queue);
    rq->nr_running++;

    /*
     * 若当前 CPU 正在运行 idle 任务，设置 need_resched：
     * cpu_idle() 的内层循环以 need_resched 为退出条件，
     * 不置位则 idle 永远在 hlt，schedule() 不被调用，新任务饿死。
     */
    if (current == rq->idle)
        rq->idle->need_resched = 1;
}

/* 将任务从运行队列移除 - per-CPU：从当前 CPU 的 runqueue 移除 */
static inline void del_task_from_runqueue(struct task_struct *p)
{
    extern runqueue_t runqueues[NR_CPUS];
    runqueue_t *rq = &runqueues[smp_processor_id()];
    list_del(&p->run_list);
    p->run_list.next = p->run_list.prev = &p->run_list;
    rq->nr_running--;
}

/* 设置任务状态并加入运行队列 */
static inline void wake_up_process(struct task_struct *p)
{
    p->state = TASK_RUNNING;
    add_task_to_runqueue(p);
}

#endif /* __KERNEL_SCHED_H__ */
