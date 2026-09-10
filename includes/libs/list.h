#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H  
#include <arch/linkage.h>

/* prefetch 提示 CPU 预取，空实现不影响正确性 */
#ifndef prefetch
#define prefetch(x) ((void)(x))
#endif

struct list_head {
	struct list_head *next, *prev;
};
 
#define LIST_HEAD_INIT(name) { &(name), &(name) }
 
#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)
 
#define INIT_LIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/*
 * Insert a new entry between two known consecutive entries. 
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static __inline__ void __list_add(struct list_head * item,
	struct list_head * prev,
	struct list_head * next)
{
	next->prev = item;
	item->next = next;
	item->prev = prev;
	prev->next = item;
} 

static __inline__ void list_add(struct list_head *item, struct list_head *head)
{
	__list_add(item, head, head->next);
}

 
static __inline__ void list_add_tail(struct list_head *item, struct list_head *head)
{
	__list_add(item, head->prev, head);
}
 
static __inline__ void __list_del(struct list_head * prev,
				  struct list_head * next)
{
	next->prev = prev;
	prev->next = next;
}

 
static __inline__ void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

 
static __inline__ void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry); 
}

 
static __inline__ int list_empty(struct list_head *head)
{
	return head->next == head;
}

 
static __inline__ void list_splice(struct list_head *list, struct list_head *head)
{
	struct list_head *first = list->next;

	if (first != list) {
		struct list_head *last = list->prev;
		struct list_head *at = head->next;

		first->prev = head;
		head->next = first;

		last->next = at;
		at->prev = last;
	}
}

 
#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))
 
#define list_for_each(pos, head) \
	for (pos = (head)->next, prefetch(pos->next); pos != (head); \
        	pos = pos->next, prefetch(pos->next))
        	
 
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next) 

/*
 * list_for_each_entry - iterate over list of given type
 * @pos:    the type * to use as a loop cursor.
 * @head:   the head of the list (struct list_head *).
 * @member: the name of the list_head within the struct.
 *
 * 参考 Linux 2.6.20 include/linux/list.h
 * 使用 typeof(*pos) 避免额外传 type 参数（gnu99 支持 typeof）
 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, typeof(*pos), member);	\
	     &(pos)->member != (head);					\
	     pos = list_entry((pos)->member.next, typeof(*pos), member))

/*
 * list_for_each_entry_safe - iterate over list, safe against removal
 * @pos:    the type * to use as a loop cursor.
 * @n:      another type * to use as temporary storage.
 * @head:   the head of the list (struct list_head *).
 * @member: the name of the list_head within the struct.
 */
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, typeof(*pos), member),	\
	     n = list_entry((pos)->member.next, typeof(*pos), member);	\
	     &(pos)->member != (head);					\
	     pos = n, n = list_entry((n)->member.next, typeof(*pos), member))

#endif
