#ifndef _LULA_WAIT_H
#define _LULA_WAIT_H

#include <libs/list.h>
#include <arch/x86/spinlock.h>
#include <arch/x86/system.h>
#include <thread.h>
#include <kernel/sched.h>   /* current / TASK_* （sched.h 不含 wait.h，无循环） */

/*
 * 等待队列 - 对齐 Linux 2.6.20 include/linux/wait.h
 *
 * 用于进程睡眠/唤醒机制。驱动在等待 I/O 完成时加入等待队列，
 * 中断处理函数完成 I/O 后唤醒等待的进程。
 *
 * 入队/出队由 q->lock 自旋锁保护，持锁路径必须用 irqsave 变体
 * （wake_up 在硬中断上下文执行，进程态持锁时必须关中断防死锁）。
 */

/* 等待队列条目：一个等待中的进程（通常睡在等待者栈上） */
typedef struct __wait_queue {
    struct list_head list;
    struct task_struct *task;
} wait_queue_t;

/* 等待队列头：管理所有等待同一事件的进程 */
typedef struct __wait_queue_head {
    spinlock_t lock;
    struct list_head task_list;
} wait_queue_head_t;

/* 初始化等待队列头 */
static inline void init_waitqueue_head(wait_queue_head_t *q)
{
    spin_lock_init(&q->lock);
    INIT_LIST_HEAD(&q->task_list);
}

/* 初始化等待队列条目 */
static inline void init_waitqueue_entry(wait_queue_t *entry, struct task_struct *tsk)
{
    entry->task = tsk;
    INIT_LIST_HEAD(&entry->list);
}

/* 判断等待队列是否为空 */
static inline int waitqueue_active(wait_queue_head_t *q)
{
    return !list_empty(&q->task_list);
}

/* ---------- 持锁内部接口（调用者必须已持有 q->lock） ---------- */

/* 无锁入队：尾部插入（参考 Linux __add_wait_queue） */
static inline void __add_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    list_add_tail(&entry->list, &head->task_list);
}

/* 无锁出队（参考 Linux __remove_wait_queue） */
static inline void __remove_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    list_del(&entry->list);
    INIT_LIST_HEAD(&entry->list);
}

/* ---------- 对外接口：自带锁保护 ---------- */

/* 加入等待队列（尾部插入，参考 Linux add_wait_queue） */
static inline void add_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    unsigned long flags;

    spin_lock_irqsave(&head->lock, flags);
    __add_wait_queue(head, entry);
    spin_unlock_irqrestore(&head->lock, flags);
}

/* 从等待队列移除（参考 Linux remove_wait_queue） */
static inline void remove_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    unsigned long flags;

    spin_lock_irqsave(&head->lock, flags);
    __remove_wait_queue(head, entry);
    spin_unlock_irqrestore(&head->lock, flags);
}

/*
 * prepare_to_wait - 原子地「入队 + 设睡眠状态」
 *
 * 参考 Linux 2.6.20 kernel/wait.c prepare_to_wait()：
 * 持 q->lock 完成入队与 state 设置，两者之间无窗口。
 *
 * 这是 wait_event 宏的安全基石：
 *   prepare_to_wait 之后才允许启动事件源（如 DMA），
 *   事件无论何时触发，wake_up 都能看到队列中的我们。
 *
 * 与 sleep_on 的区别：sleep_on 内部自己调 schedule；
 * prepare_to_wait 把「入队」与「睡眠」解耦，
 * 调用者可在两者之间插入「启动事件源」的动作。
 */
static inline void prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
    unsigned long flags;

    spin_lock_irqsave(&q->lock, flags);
    if (list_empty(&wait->list))
        __add_wait_queue(q, wait);
    current->state = TASK_UNINTERRUPTIBLE;
    spin_unlock_irqrestore(&q->lock, flags);
}

/*
 * finish_wait - 醒来后清理：置回 RUNNING + 出队
 *
 * 参考 Linux 2.6.20 kernel/wait.c finish_wait()：
 * 出队由被唤醒进程自己完成（wake_up 不代劳），
 * 避免中断上下文操作睡眠者栈上的 entry。
 */
static inline void finish_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
    unsigned long flags;

    current->state = TASK_RUNNING;

    spin_lock_irqsave(&q->lock, flags);
    if (!list_empty(&wait->list))
        __remove_wait_queue(q, wait);
    spin_unlock_irqrestore(&q->lock, flags);
}

/* 唤醒等待队列中的所有进程 */
void wake_up(wait_queue_head_t *q);

/* 当前进程睡眠，加入等待队列并让出 CPU */
void sleep_on(wait_queue_head_t *q);

#endif /* _LULA_WAIT_H */
