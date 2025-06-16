// #ifndef _LINUX_WAIT_H
// #define _LINUX_WAIT_H

// #define WNOHANG		0x00000001
// #define WUNTRACED	0x00000002

// #define __WNOTHREAD	0x20000000	/* Don't wait on children of other threads in this group */
// #define __WALL		0x40000000	/* Wait on all children, regardless of type */
// #define __WCLONE	0x80000000	/* Wait only on non-SIGCHLD children */
 

// #include <libs/list.h>
// #include <arch/x86/spinlock.h>
// #include <arch/x86/page.h>
// #include <sched.h>
 
// struct __wait_queue {
// 	unsigned int flags;
// 	struct task_struct * task;
// 	struct list_head task_list; 
// };
// typedef struct __wait_queue wait_queue_t;

 
// #define wq_lock_t spinlock_t
// #define WAITQUEUE_RW_LOCK_UNLOCKED SPIN_LOCK_UNLOCKED

// #define wq_read_lock spin_lock
// #define wq_read_lock_irqsave spin_lock_irqsave
// #define wq_read_unlock spin_unlock
// #define wq_read_unlock_irqrestore spin_unlock_irqrestore
// #define wq_write_lock_irq spin_lock_irq
// #define wq_write_lock_irqsave spin_lock_irqsave
// #define wq_write_unlock_irqrestore spin_unlock_irqrestore
// #define wq_write_unlock spin_unlock 

 
// struct __wait_queue_head {
// 	wq_lock_t lock;
// 	struct list_head task_list;
// };
// typedef struct __wait_queue_head wait_queue_head_t;
 

// #define __WAITQUEUE_INITIALIZER(name, tsk) {				\
// 	task:		tsk,						\
// 	task_list:	{ NULL, NULL }}

// #define DECLARE_WAITQUEUE(name, tsk)					\
// 	wait_queue_t name = __WAITQUEUE_INITIALIZER(name, tsk)

// #define __WAIT_QUEUE_HEAD_INITIALIZER(name) {				\
// 	lock:		WAITQUEUE_RW_LOCK_UNLOCKED,			\
// 	task_list:	{ &(name).task_list, &(name).task_list }}

// #define DECLARE_WAIT_QUEUE_HEAD(name) \
// 	wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)


 
// static inline void init_waitqueue_head(wait_queue_head_t *q)
// { 
// 	q->lock = WAITQUEUE_RW_LOCK_UNLOCKED;
// 	INIT_LIST_HEAD(&q->task_list); 
// }


 
// static inline void init_waitqueue_entry(wait_queue_t *q, struct task_struct *p)
// { 
// 	q->flags = 0;
// 	q->task = p; 
// }

 
// static inline int waitqueue_active(wait_queue_head_t *q)
// { 
// 	return !list_empty(&q->task_list);
// }

 
// static inline void __add_wait_queue(wait_queue_head_t *head, wait_queue_t *new)
// { 
// 	list_add(&new->task_list, &head->task_list);
// }

 
// static inline void __add_wait_queue_tail(wait_queue_head_t *head,
// 						wait_queue_t *new)
// { 
// 	list_add_tail(&new->task_list, &head->task_list);
// }

 
// static inline void __remove_wait_queue(wait_queue_head_t *head,
// 							wait_queue_t *old)
// { 
// 	list_del(&old->task_list);
// } 

// #endif
