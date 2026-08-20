/*
 * LulaOS 进程调度器实现
 *
 * 参考 Linux 2.6.20 kernel/sched.c / kernel/fork.c / arch/i386/kernel/smpboot.c
 *
 * 主要改动（相对旧版）：
 *   1. 使用 thread_union（task_struct + 内核栈合并 8KB 块）
 *   2. current 宏通过 esp & ~0x1FFF 直接获取 task_struct
 *   3. sys_fork / kernel_thread 使用 kmalloc(THREAD_SIZE) 分配 thread_union
 *   4. BSP 使用 init_thread_union（.data.init_task 节，8KB 对齐）
 *   5. cpu_idle() 循环检查 need_resched 并调用 schedule()
 *   6. __switch_to 不再需要第三个 current_p_addr 参数
 *
 * 调度策略：静态优先级 + 时间片轮转
 *   - 时间片 = priority * 10（MIN_TIMESLICE ~ MAX_TIMESLICE）
 *   - 每个 tick（APIC Timer）递减 counter
 *   - counter=0 时设置 need_resched，下次 schedule() 选下一任务
 *   - 选任务时遍历 runqueue，取 counter 最大的 TASK_RUNNING 任务
 *
 * task_struct 字段偏移（供 entry.S 汇编使用，32 位 x86）：
 *   need_resched  = 16  (0x10)
 *   thread.esp    = 56  (0x38)
 *   thread.eip    = 60  (0x3C)
 *   thread.pgd    = 84  (0x54)
 *   flags         = 92  (0x5C)
 */

#include <kernel/sched.h>
#include <printk.h>
#include <libs/list.h>
#include <libs/memcpy.h>
#include <arch/x86/spinlock.h>
#include <arch/x86/system.h>
#include <arch/x86/page.h>
#include <mm/mmzone.h>
#include <mm/slab.h>
#include <stddef.h>

/* ========== 全局数据 ========== */

/*
 * BSP 的初始 thread_union：task_struct 在低地址，内核栈在高地址
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/init_task.c：
 *   union thread_union init_thread_union
 *       __attribute__((__section__(".data.init_task"))) =
 *       { INIT_THREAD_INFO(init_task) };
 *
 * linker.lds 中 .data.init_task 段以 BLOCK(8K) 对齐，保证 current 宏正确工作。
 */
union thread_union init_thread_union
    __attribute__((__section__(".data.init_task")))
    __attribute__((aligned(THREAD_SIZE))) = {
        .task = INIT_TASK(init_thread_union.task)
    };

/* 每个 CPU 一个运行队列 */
runqueue_t runqueues[NR_CPUS];

/* 全局任务链表（调试用） */
static LIST_HEAD(task_list);

/* 下一个 PID */
static int next_pid = 1;

/* 上下文切换汇编入口 */
extern void __switch_to(void);
extern void ret_from_fork(void);
extern void ret_from_intr(void);

/* 新创建的内核线程 helper */
static void kernel_thread_helper(void);

/* ========== 初始化 ========== */

void sched_init(void)
{
    int i;
    runqueue_t *rq;

    /* 初始化所有 CPU 的运行队列 */
    for (i = 0; i < NR_CPUS; i++) {
        rq = &runqueues[i];
        spin_lock_init(&rq->lock);
        INIT_LIST_HEAD(&rq->queue);
        rq->nr_running = 0;
        /* 默认所有 CPU 的 idle 指向 BSP 的 init_task，
         * AP 启动时 smp_init() 会为每个 AP 分配独立 idle */
        rq->idle = &init_task;
    }

    /* 将 init_task 链入全局任务链表 */
    INIT_LIST_HEAD(&task_list);
    list_add(&init_task.tasks, &task_list);

    printk("sched: scheduler initialized (idle pid=%d)\n", init_task.pid);
}

/* ========== 核心调度 ========== */

/*
 * schedule - 调度器主入口
 *
 * 可由以下路径调用：
 *   1. 中断返回前（ret_from_intr 检查 need_resched）
 *   2. 主动调用（do_exit / interruptible_sleep）
 *   3. cpu_idle() 循环中 need_resched 置位后
 */
void schedule(void)
{
    int cpu = smp_processor_id();
    runqueue_t *rq = &runqueues[cpu];
    struct task_struct *prev, *next, *p;
    unsigned long flags;
    struct list_head *pos;

    local_irq_save(flags);

    prev = current;

    /* 获取运行队列锁 */
    spin_lock(&rq->lock);

    /* 若 need_resched 置位，清除之 */
    if (prev->need_resched)
        prev->need_resched = 0;

    /*
     * 若 prev 时间片耗尽且仍可运行，移入队列尾部，
     * 给它新的时间片，下一轮重新竞争 CPU
     *
     * 注意：idle 任务的 counter 永远是 MAX_TIMESLICE（scheduler_tick
     * 对 idle 直接 return，不递减），因此这个条件对 idle 永远为假，
     * 不会触发 list_del/list_add_tail，链表不会被破坏。
     */
    if (prev->state == TASK_RUNNING && prev->counter == 0) {
        prev->counter = prev->timeslice;
        list_del(&prev->run_list);
        list_add_tail(&prev->run_list, &rq->queue);
    }

    /* 选择下一个任务：扫描 runqueue，取 counter 最大者 */
    next = rq->idle;  /* 默认为空闲任务 */
    {
        long best_cnt = -1;
        list_for_each(pos, &rq->queue) {
            p = list_entry(pos, struct task_struct, run_list);
            if (p->state == TASK_RUNNING && (long)p->counter > best_cnt) {
                best_cnt = p->counter;
                next = p;
            }
        }
    }

    /* 选中自己则无需切换 */
    if (next == prev) {
        spin_unlock(&rq->lock);
        local_irq_restore(flags);
        return;
    }

    spin_unlock(&rq->lock);

    /*
     * 上下文切换
     *
     * current 宏现在通过 esp & ~0x1FFF 自动获取，
     * __switch_to 只需要 prev 和 next 两个参数。
     *
     * 若 next 设置了 PF_NEVER_STARTED，__switch_to 会跳转到
     * ret_from_fork，由 ret_from_fork 负责清除该标志并跳转到
     * next->thread.eip 保存的入口地址。
     */
    __asm__ __volatile__(
        "pushl  %1\n\t"        /* next（第二个参数）*/
        "pushl  %0\n\t"        /* prev（第一个参数）*/
        "call   __switch_to\n\t"
        "addl   $8, %%esp\n\t" /* 弹出 prev / next */
        :
        : "r" (prev),          /* %0: prev */
          "r" (next)           /* %1: next */
        : "eax", "ecx", "edx", "memory"
    );

    local_irq_restore(flags);
}

/* ========== 定时器 Tick ========== */

/*
 * scheduler_tick - APIC Timer 每次触发时调用
 *
 * 递减当前任务的 counter，耗尽时设置 need_resched，
 * schedule() 会在合适的时机被调用（中断返回前或 cpu_idle 中）
 */
void scheduler_tick(void)
{
    int cpu = smp_processor_id();
    struct task_struct *p = current;
    runqueue_t *rq = &runqueues[cpu];

    if (p == rq->idle)
        return;

    if (p->counter > 0)
        p->counter--;

    if (p->counter == 0)
        p->need_resched = 1;
}

/* ========== idle 循环 ========== */

/*
 * cpu_idle - 每个 CPU 的空闲循环
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/process.c cpu_idle()：
 *   - 内层循环：CPU 无事可做时 hlt，等待中断
 *   - 外层循环：中断返回后检查 need_resched，若置位则调用 schedule()
 *
 * BSP 在 _kernel_main 末尾调用，AP 在 start_secondary 末尾调用。
 */
void cpu_idle(void)
{
    for (;;) {
        /* 等待 need_resched 被中断处理器置位 */
        while (!current->need_resched)
            safe_halt();
        /* 调度到下一个任务 */
        schedule();
    }
}

/* ========== 进程创建 ========== */

/*
 * sys_fork - 创建子进程（完整地址空间复制）
 *
 * 参考 Linux 2.6.20 kernel/fork.c dup_task_struct + copy_thread：
 *   - 使用 kmalloc(THREAD_SIZE) 分配 8KB 对齐的 thread_union
 *   - task_struct 在低地址（union 起始），内核栈在高地址向下增长
 *   - 这样 current 宏（esp & ~0x1FFF）对新进程天然有效
 */
int sys_fork(struct pt_regs *regs)
{
    union thread_union *tu;
    struct task_struct *p;
    unsigned long stack_base;
    unsigned long flags;

    /*
     * 分配 THREAD_SIZE(8KB) 对齐的 thread_union
     * kmalloc(8192) 使用 order=1 伙伴系统分配，保证 8KB 对齐
     */
    tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
    if (!tu)
        return -1;

    stack_base = (unsigned long)tu;
    p = &tu->task;
    memset(p, 0, sizeof(*p));

    /* 从父进程复制基本字段 */
    *p = *current;

    /* 分配新 PID */
    p->pid = next_pid++;
    p->state = TASK_UNINTERRUPTIBLE;

    /* 重新初始化链表节点 */
    INIT_LIST_HEAD(&p->run_list);
    INIT_LIST_HEAD(&p->tasks);

    /* 重置调度状态 */
    p->counter    = p->timeslice;
    p->need_resched = 0;
    p->flags      = PF_NEVER_STARTED;

    /* 内核栈顶 = thread_union 基址 + THREAD_SIZE */
    p->thread.esp0 = stack_base + THREAD_SIZE;
    p->thread.pgd  = 0;

    /*
     * 将父进程 pt_regs 复制到子进程栈顶，
     * 这样子进程被调度时通过 RESTORE_ALL + iret 返回用户态
     *
     * 参考 Linux copy_thread：childregs = task_pt_regs(p)
     */
    {
        struct pt_regs *child_regs =
            (struct pt_regs *)(stack_base + THREAD_SIZE - sizeof(struct pt_regs));
        memcpy(child_regs, regs, sizeof(struct pt_regs));
        /* 子进程 fork 返回 0 */
        child_regs->eax = 0;
        p->pt_regs = child_regs;
    }

    /*
     * 设置切换上下文：
     *   thread.esp = pt_regs 首地址
     *   thread.eip = ret_from_intr（跳至 RESTORE_ALL → iret）
     */
    p->thread.eip = (unsigned long)ret_from_intr;
    p->thread.esp = (unsigned long)p->pt_regs;

    /* 标记为可运行，加入运行队列 */
    p->state = TASK_RUNNING;

    local_irq_save(flags);
    add_task_to_runqueue(p);
    list_add(&p->tasks, &task_list);
    local_irq_restore(flags);

    printk("sched: fork pid=%d from pid=%d\n", p->pid, current->pid);
    return p->pid;
}

/*
 * kernel_thread - 创建内核线程
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/process.c kernel_thread + copy_thread：
 *   - 同 sys_fork 使用 kmalloc(THREAD_SIZE) 分配 thread_union
 *   - 新线程首次调度时跳转到 kernel_thread_helper → fn(arg)
 */
int kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
    union thread_union *tu;
    struct task_struct *p;
    unsigned long stack_base;
    unsigned long *sp;
    unsigned long irq_flags;

    tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
    if (!tu)
        return -1;

    stack_base = (unsigned long)tu;
    p = &tu->task;
    memset(p, 0, sizeof(*p));

    *p = *current;

    p->pid = next_pid++;
    p->state = TASK_UNINTERRUPTIBLE;
    INIT_LIST_HEAD(&p->run_list);
    INIT_LIST_HEAD(&p->tasks);

    p->counter    = p->timeslice;
    p->need_resched = 0;
    p->flags      = PF_NEVER_STARTED;

    p->thread.esp0 = stack_base + THREAD_SIZE;

    /*
     * 构造初始栈帧：
     *   *(--sp) = arg   ; fn 的参数
     *   *(--sp) = fn    ; kernel_thread_helper 的 ret 弹出此值跳转
     * thread.eip = kernel_thread_helper（__switch_to 检测 PF_NEVER_STARTED 后跳转）
     * thread.esp = sp（指向 [fn, arg]）
     */
    sp = (unsigned long *)(stack_base + THREAD_SIZE);
    *(--sp) = (unsigned long)arg;
    *(--sp) = (unsigned long)fn;

    p->thread.eip = (unsigned long)kernel_thread_helper;
    p->thread.esp = (unsigned long)sp;

    p->state = TASK_RUNNING;

    local_irq_save(irq_flags);
    add_task_to_runqueue(p);
    list_add(&p->tasks, &task_list);
    local_irq_restore(irq_flags);

    printk("sched: kernel_thread pid=%d fn=%p\n", p->pid, fn);
    return p->pid;
}

/*
 * kernel_thread_helper - 内核线程的统一入口
 *
 * __switch_to 检测 PF_NEVER_STARTED 后跳转到 ret_from_fork，
 * ret_from_fork 清除标志后跳转到 thread.eip（即此函数）。
 * 此时栈上（由 kernel_thread 构造）：
 *   [esp+0] = fn
 *   [esp+4] = arg
 */
static void kernel_thread_helper(void)
{
    /* 开中断：新线程从未执行过 sti，需要显式开中断 */
    sti();

    {
        unsigned long *sp;
        int (*fn)(void *);
        void *arg;

        __asm__ volatile("movl %%esp, %0" : "=r"(sp));
        fn  = (int (*)(void *))sp[0];
        arg = (void *)sp[1];
        fn(arg);
    }

    /* fn 返回后退出 */
    do_exit(0);
}

/* ========== 进程退出 / 睡眠 ========== */

void do_exit(long code)
{
    struct task_struct *p = current;
    unsigned long flags;

    (void)code;

    local_irq_save(flags);

    p->state = TASK_ZOMBIE;

    /* 从运行队列移除（若还在队列中） */
    if (p->run_list.next != &p->run_list)
        del_task_from_runqueue(p);

    printk("sched: pid=%d (%s) exited\n", p->pid, p->comm);

    local_irq_restore(flags);

    /* 触发调度，CPU 将切换到其他任务，此任务不再执行 */
    schedule();

    /* 不应执行到这里 */
    for (;;)
        safe_halt();
}

void interruptible_sleep(void)
{
    struct task_struct *p = current;
    unsigned long flags;

    local_irq_save(flags);

    p->state = TASK_INTERRUPTIBLE;
    if (p->run_list.next != &p->run_list)
        del_task_from_runqueue(p);

    local_irq_restore(flags);

    schedule();
}
