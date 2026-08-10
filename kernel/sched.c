/*
 * LulaOS 进程调度器实现
 *
 * 调度策略：静态优先级 + 时间片轮转
 *   - 时间片 = priority * 10（MIN_TIMESLICE ~ MAX_TIMESLICE）
 *   - 每个 tick（APIC Timer）递减 counter
 *   - counter=0 时设置 need_resched，下次 schedule() 选下一任务
 *   - 选任务时遍历 runqueue，取 counter 最大的 TASK_RUNNING 任务
 */

#include <kernel/sched.h>
#include <printk.h>
#include <libs/list.h>
#include <libs/memcpy.h>
#include <arch/x86/spinlock.h>
#include <arch/x86/system.h>
#include <arch/x86/page.h>
#include <mm/mmzone.h>
#include <stddef.h>

/* ========== 全局数据 ========== */

/* 引导任务（静态分配，不依赖页分配器） */
struct task_struct init_task = INIT_TASK(init_task);

/* per-CPU current 指针数组：索引 = 逻辑 CPU 号 */
struct task_struct *current_p[NR_CPUS];

/* 每个 CPU 一个运行队列 */
runqueue_t runqueues[NR_CPUS];

/* 全局任务链表（调试用） */
static LIST_HEAD(task_list);

/* 下一个 PID */
static int next_pid = 1;

/* 上下文切换汇编入口 */
extern void __switch_to(void);
extern void ret_from_fork(void);
extern void ret_from_intr(void);  /* entry.S: 中断返回路径，跳至 RESTORE_ALL + iret */

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
        rq->idle = &init_task;
        current_p[i] = &init_task;
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
 *   3. 系统调用返回
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
     * 传递 &current_p[cpu]（当前 CPU 的 per-CPU 槽位地址）作为第三个参数，
     * __switch_to 在切换栈之后将 next 写入该地址，完成 per-CPU current 更新。
     *
     * 若 next 设置了 PF_NEVER_STARTED，__switch_to 会跳转到
     * ret_from_fork，由 ret_from_fork 负责清除该标志并跳转到
     * next->thread.eip 保存的入口地址。
     */
    {
        struct task_struct **cp = &current_p[cpu];
        __asm__ __volatile__(
            "pushl  %2\n\t"        /* current_p_addr（__switch_to 第三个参数）*/
            "pushl  %1\n\t"        /* next（__switch_to 第二个参数）*/
            "pushl  %0\n\t"        /* prev（__switch_to 第一个参数）*/
            "call   __switch_to\n\t"
            "addl   $12, %%esp\n\t" /* 弹出 prev / next / current_p_addr */
            :
            : "m" (*cp),           /* %0: prev = 当前 current */
              "r" (next),          /* %1: next */
              "r" (cp)             /* %2: &current_p[cpu] */
            : "eax", "ecx", "edx", "memory"
        );
    }

    local_irq_restore(flags);
}

/* ========== 定时器 Tick ========== */

/*
 * scheduler_tick - APIC Timer 每次触发时调用
 *
 * 递减当前任务的 counter，耗尽时设置 need_resched，
 * schedule() 会在合适的时机被调用（中断返回前或主动调用）
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

/* ========== 进程创建 ========== */

/*
 * sys_fork - 创建子进程（完整地址空间复制）
 *
 * 为简化实现，当前不复制页表（父子共享内核地址空间），
 * 子进程使用新分配的内核栈，pt_regs 从父进程中断帧复制。
 */
int sys_fork(struct pt_regs *regs)
{
    struct page *page;
    struct task_struct *p;
    unsigned long stack_base;
    unsigned long flags;

    /* 分配一页用于 task_struct + 内核栈（task_struct 在页首，栈从页尾向下增长） */
    page = __alloc_pages(0, 0);
    if (!page)
        return -1;

    stack_base = (unsigned long)page->virtual;
    p = (struct task_struct *)stack_base;
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

    /* 内核栈 */
    p->thread.esp0 = stack_base + PAGE_SIZE;
    p->thread.pgd  = 0;

    /*
     * 将父进程 pt_regs 复制到子进程栈顶，
     * 这样子进程被调度时通过 RESTORE_ALL + iret 返回用户态
     */
    {
        struct pt_regs *child_regs =
            (struct pt_regs *)(stack_base + PAGE_SIZE - sizeof(struct pt_regs));
        memcpy(child_regs, regs, sizeof(struct pt_regs));
        /* 子进程 fork 返回 0（eax 在 pt_regs 偏移 0x08，由中断 push 顺序确定） */
        child_regs->eax = 0;
        p->pt_regs = child_regs;
    }

    /*
     * 设置切换上下文：
     *   thread.esp = pt_regs 首地址（RESTORE_ALL 从此处开始 pop）
     *   thread.eip = ret_from_intr（跳至 RESTORE_ALL → iret）
     *
     * 首次调度到子进程时：
     *   __switch_to → 检测 PF_NEVER_STARTED → jmp ret_from_fork
     *   ret_from_fork → 读 thread.eip → jmp ret_from_intr
     *   ret_from_intr → RESTORE_ALL → pop registers → iret（返回用户态）
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
 * 新线程在首次被调度时直接跳转到 fn(arg)，
 * 不需要经过 iret 返回用户态（纯内核上下文）。
 *
 * 栈布局（从高到低）：
 *   [栈顶]
 *   pushl arg           ← thread.esp + 4
 *   pushl fn            ← thread.esp     （switch_to 的 ret 弹出此值作为 eip）
 *   实际: pushl arg; pushl fn; thread.eip = helper; thread.esp = esp_after_pushes
 *
 *   当 switch_to 切换到子线程并执行 ret 时：
 *     ret 弹出 helper → eip = helper
 *     helper 执行 ret → 弹出 fn → eip = fn
 *     fn 执行 ret → 弹出 arg（不会返回到这里，fn 应调用 do_exit）
 */
int kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
    struct page *page;
    struct task_struct *p;
    unsigned long stack_base;
    unsigned long *sp;
    unsigned long irq_flags;

    page = __alloc_pages(0, 0);
    if (!page)
        return -1;

    stack_base = (unsigned long)page->virtual;
    p = (struct task_struct *)stack_base;
    memset(p, 0, sizeof(*p));

    *p = *current;

    p->pid = next_pid++;
    p->state = TASK_UNINTERRUPTIBLE;
    INIT_LIST_HEAD(&p->run_list);
    INIT_LIST_HEAD(&p->tasks);

    p->counter    = p->timeslice;
    p->need_resched = 0;
    p->flags      = PF_NEVER_STARTED;

    p->thread.esp0 = stack_base + PAGE_SIZE;

    /*
     * 构造初始栈帧：
     *   pushl arg         ; 参数
     *   pushl fn          ; 线程函数
     *   → thread.eip = kernel_thread_helper
     *   → thread.esp = 当前 sp（switch_to ret 弹出 helper）
     *
     * switch_to 执行 ret 时：
     *   eip ← helper（由 thread.eip 在 switch_to 里被 push 到栈上，
     *          实际上 helper 直接赋给 thread.eip，switch_to 的 ret
     *          会跳到这里。然后 helper 内部 call fn。）
     *
     * 简化方案：thread.eip = helper，thread.esp 指向 [fn, arg]
     */
    sp = (unsigned long *)(stack_base + PAGE_SIZE);
    *(--sp) = (unsigned long)arg;   /* fn 的参数 */
    *(--sp) = (unsigned long)fn;    /* fn 地址（helper 的 ret 弹出此值）*/

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
 * switch_to 切换到新线程后 ret 跳转到此函数，
 * 此时栈上（由 kernel_thread 构造）：
 *   [esp+0] = fn
 *   [esp+4] = arg
 */
static void kernel_thread_helper(void)
{
    /* 开中断：新线程从未执行过 sti，需要显式开中断 */
    sti();

    /*
     * 从栈上取 fn 和 arg（由 kernel_thread 压入）。
     * 由于 helper 是被 switch_to 的 ret 跳转过来的，
     * 栈帧为：[esp]=fn, [esp+4]=arg（push 顺序：先 arg 后 fn）
     */
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
