/*
 * wait.c - 等待队列核心实现
 *
 * sleep_on: 当前进程加入等待队列并睡眠，让出 CPU
 * wake_up:  唤醒等待队列中的所有进程
 */

#include <wait.h>
#include <kernel/sched.h>
#include <printk.h>
#include <arch/x86/system.h>

/*
 * wake_up - 唤醒等待队列中的所有进程
 *
 * 遍历等待队列，对每个进程：
 *   1. 将状态改为 TASK_RUNNING
 *   2. 重新加入运行队列（schedule 只扫描 rq->queue，不加入则永远不会被调度）
 *   3. 从等待队列移除
 *
 * 【关键】仅修改 state 是不够的！sleep_on/schedule 路径将任务从 runqueue
 * 移除（TASK_UNINTERRUPTIBLE 的任务不在 run queue 中），如果 wake_up
 * 不重新加入，调度器扫描不到该任务，任务会永久睡眠。
 */
void wake_up(wait_queue_head_t *q)
{
    struct list_head *pos, *tmp;
    wait_queue_t *entry;

    list_for_each_safe(pos, tmp, &q->task_list) {
        entry = list_entry(pos, wait_queue_t, list);

        /* 将进程状态改为可运行 */
        entry->task->state = TASK_RUNNING;

        /*
         * 重新加入运行队列。
         * schedule() 只扫描 rq->queue 链表挑选下一个任务，
         * 不加入则永远不会被调度器选中。
         */
        add_task_to_runqueue(entry->task);

        /* 从等待队列移除 */
        list_del(&entry->list);
    }
}

/*
 * sleep_on - 当前进程睡眠并加入等待队列
 *
 * 流程：
 *   1. 关中断，防止 IRQ 在 add_wait_queue 与 schedule 之间触发
 *   2. 创建等待队列条目，指向当前进程
 *   3. 加入等待队列
 *   4. 设置当前进程状态为 TASK_UNINTERRUPTIBLE（不可中断睡眠）
 *   5. 调用 schedule() 让出 CPU（内部会开中断）
 *   6. 被唤醒后返回
 *
 * 【关键】必须先设置 state 再开中断/启动 DMA，否则存在竞态：
 *   错误顺序:  start_DMA → [IRQ 触发, wake_up 找不到 entry] → sleep_on → 永久睡眠
 *   正确顺序:  add_wait_queue + state=SLEEPING → start_DMA → schedule()
 *
 * 关中断保护 [add_wait_queue + state 设置] 这段临界区，
 * 确保 IRQ 触发时 wake_up 一定能找到 entry 并唤醒。
 */
void sleep_on(wait_queue_head_t *q)
{
    wait_queue_t entry;
    unsigned long flags;

    /* 关中断：保护 [加入队列 + 设置状态] 不被 IRQ 打断 */
    local_irq_save(flags);

    /* 初始化等待队列条目 */
    init_waitqueue_entry(&entry, current);

    /* 加入等待队列（必须在设置 state 之前，wake_up 才能找到我们） */
    add_wait_queue(q, &entry);

    /* 设置当前进程为不可中断睡眠状态 */
    current->state = TASK_UNINTERRUPTIBLE;

    /*
     * 开中断，允许 IRQ 触发。
     * 若 IRQ 已经触发（在关中断期间排队），开中断后立即交付，
     * wake_up 会将 state 改为 TASK_RUNNING，schedule() 不会真正睡眠。
     */
    local_irq_restore(flags);

    /* 让出 CPU，调度器选择其他进程运行 */
    schedule();

    /* 被唤醒后执行到这里，entry 已被 wake_up 从队列移除 */
}
