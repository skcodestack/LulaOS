/*
 * wait.c - 等待队列核心实现
 *
 * sleep_on: 当前进程加入等待队列并睡眠，让出 CPU
 * wake_up:  唤醒等待队列中的所有进程
 */

#include <wait.h>
#include <kernel/sched.h>
#include <printk.h>

/*
 * wake_up - 唤醒等待队列中的所有进程
 *
 * 遍历等待队列，将每个进程的状态从 TASK_UNINTERRUPTIBLE 改为 TASK_RUNNING，
 * 然后从队列中移除。调度器会在下次调度时选择这些进程运行。
 */
void wake_up(wait_queue_head_t *q)
{
    struct list_head *pos, *tmp;
    wait_queue_t *entry;

    list_for_each_safe(pos, tmp, &q->task_list) {
        entry = list_entry(pos, wait_queue_t, list);
        
        /* 将进程状态改为可运行 */
        entry->task->state = TASK_RUNNING;
        
        /* 从等待队列移除 */
        list_del(&entry->list);
    }
}

/*
 * sleep_on - 当前进程睡眠并加入等待队列
 *
 * 流程：
 *   1. 创建等待队列条目，指向当前进程
 *   2. 加入等待队列
 *   3. 设置当前进程状态为 TASK_UNINTERRUPTIBLE（不可中断睡眠）
 *   4. 调用 schedule() 让出 CPU
 *   5. 被唤醒后返回（中断处理函数调用 wake_up）
 *
 * 注意：此函数返回后，等待队列条目已不在队列中（被 wake_up 移除）。
 */
void sleep_on(wait_queue_head_t *q)
{
    wait_queue_t entry;
    
    /* 初始化等待队列条目 */
    init_waitqueue_entry(&entry, current);
    
    /* 加入等待队列 */
    add_wait_queue(q, &entry);
    
    /* 设置当前进程为不可中断睡眠状态 */
    current->state = TASK_UNINTERRUPTIBLE;
    
    /* 让出 CPU，调度器选择其他进程运行 */
    schedule();
    
    /* 被唤醒后执行到这里，entry 已被 wake_up 从队列移除 */
}
