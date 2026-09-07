/*
 * wait.c - 等待队列核心实现（对齐 Linux 2.6.20 kernel/sched.c sleep_on + wait.c wake_up）
 */

#include <wait.h>
#include <kernel/sched.h>
#include <printk.h>
#include <arch/x86/system.h>

/*
 * wake_up - 唤醒等待队列中的所有进程
 *
 * 持 q->lock 遍历等待队列，对每个 entry 调 wake_up_process()：
 *   1. state → TASK_RUNNING
 *   2. 重新加入运行队列（schedule 只扫描 rq->queue，不加入则永远不会被调度）
 *
 * 【与旧版的区别】不再代睡眠者从等待队列出队。
 * 出队由被唤醒进程醒来后自己完成（sleep_on 的 __remove_wait_queue /
 * finish_wait），参考 Linux wake_up → try_to_wake_up 的职责划分：
 *   wake_up 只改状态 + 入运行队列，不动等待队列。
 *
 * 好处：避免硬中断上下文去操作睡眠者栈上的 wait entry，
 * 若睡眠者尚未真正 schedule（还在运行），出队操作可能与其自身的
 * finish_wait 竞争同一链表节点。
 */
void wake_up(wait_queue_head_t *q)
{
    unsigned long flags;
    struct list_head *pos, *tmp;
    wait_queue_t *entry;

    spin_lock_irqsave(&q->lock, flags);

    list_for_each_safe(pos, tmp, &q->task_list) {
        entry = list_entry(pos, wait_queue_t, list);

        /* 置 RUNNING + 加入运行队列（内部有防重复入队检查） */
        wake_up_process(entry->task);
    }

    spin_unlock_irqrestore(&q->lock, flags);
}

/*
 * sleep_on - 当前进程睡眠并加入等待队列
 *
 * 参考 Linux 2.6.20 kernel/sched.c sleep_on() 原版结构：
 *   1. 设置 state = TASK_UNINTERRUPTIBLE
 *   2. 持 q->lock 入队
 *   3. schedule() 让出 CPU
 *   4. 醒来后自己持 q->lock 出队
 *
 * 【适用限制】与 Linux sleep_on 同样的固有限制：
 * 若事件在进入本函数之前已经触发，唤醒会丢失（wake_up 看不到还没入队的我们）。
 * 因此只适用于「事件源由调用方启动」的场景（先 sleep_on 准备，后启动事件源），
 * 或容忍丢失的事件。
 *
 * 需要「入队与睡眠之间插入动作」的驱动（如 ATA DMA 先入队再写 BM_CMD_START）
 * 请改用 prepare_to_wait / schedule / finish_wait 三段式。
 */
void sleep_on(wait_queue_head_t *q)
{
    wait_queue_t wait;
    unsigned long flags;

    init_waitqueue_entry(&wait, current);

    /* 1. 先设睡眠状态 */
    current->state = TASK_UNINTERRUPTIBLE;

    /* 2. 持锁入队：与 wake_up 的遍历互斥 */
    spin_lock_irqsave(&q->lock, flags);
    __add_wait_queue(q, &wait);
    spin_unlock_irqrestore(&q->lock, flags);

    /* 3. 让出 CPU；schedule() 检测 state != RUNNING 会将本任务摘出运行队列 */
    schedule();

    /* 4. 醒来后自己出队（wake_up 不代劳） */
    spin_lock_irqsave(&q->lock, flags);
    __remove_wait_queue(q, &wait);
    spin_unlock_irqrestore(&q->lock, flags);
}
