#ifndef _LULA_WAIT_H
#define _LULA_WAIT_H

#include <libs/list.h>
#include <thread.h>

/*
 * 等待队列 - 简化版实现
 *
 * 用于进程睡眠/唤醒机制。驱动在等待 I/O 完成时加入等待队列，
 * 中断处理函数完成 I/O 后唤醒等待的进程。
 */

/* 等待队列条目：一个等待中的进程 */
typedef struct __wait_queue {
    struct list_head list;
    struct task_struct *task;
} wait_queue_t;

/* 等待队列头：管理所有等待同一事件的进程 */
typedef struct __wait_queue_head {
    struct list_head task_list;
} wait_queue_head_t;

/* 初始化等待队列头 */
static inline void init_waitqueue_head(wait_queue_head_t *q)
{
    INIT_LIST_HEAD(&q->task_list);
}

/* 初始化等待队列条目 */
static inline void init_waitqueue_entry(wait_queue_t *entry, struct task_struct *tsk)
{
    entry->task = tsk;
}

/* 判断等待队列是否为空 */
static inline int waitqueue_active(wait_queue_head_t *q)
{
    return !list_empty(&q->task_list);
}

/* 加入等待队列（尾部插入） */
static inline void add_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    list_add_tail(&entry->list, &head->task_list);
}

/* 从等待队列移除 */
static inline void remove_wait_queue(wait_queue_head_t *head, wait_queue_t *entry)
{
    list_del(&entry->list);
}

/* 唤醒等待队列中的所有进程 */
void wake_up(wait_queue_head_t *q);

/* 当前进程睡眠，加入等待队列并让出 CPU */
void sleep_on(wait_queue_head_t *q);

#endif /* _LULA_WAIT_H */
