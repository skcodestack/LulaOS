#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

/* spinlock_t 必须在所有 #include 之前定义，以打破循环包含链：
 * spinlock.h → rwlock.h/atomic.h → system.h → page.h → mm.h → mmzone.h → spinlock.h */
typedef struct {
	volatile unsigned int lock;
} spinlock_t;

#include <arch/x86/rwlock.h>
#include <arch/x86/atomic.h>
#include <arch/x86/system.h>
 

 
#define SPIN_LOCK_UNLOCKED (spinlock_t) { 1 } 
 
#define spin_lock_init(x)	do { *(x) = SPIN_LOCK_UNLOCKED; } while(0) 
 
#define spin_is_locked(x)	(*(volatile char *)(&(x)->lock) <= 0)
 
#define spin_unlock_wait(x)	do { barrier(); } while(spin_is_locked(x))
 
#define read_unlock(rw)		asm volatile(read_unlock_string)
 
#define write_unlock(rw)	asm volatile(write_unlock_string)

/*
  lock
*/
#define spin_lock_string \
	"\n1:\t" \
	"lock ; decb %0\n\t" \			  	
	"js 2f\n" \						   
	".section .text.lock,\"ax\"\n" \   
	"2:\t" \
	"cmpb $0,%0\n\t" \				   
	"rep;nop\n\t" \					  
	"jle 2b\n\t" \					   
	"jmp 1b\n" \					  
	".previous"

 
 


#define read_unlock_string \
	"lock;   incl %0" \
		:"=m" ((rw)->lock) : : "memory"

#define write_unlock_string \
	"lock ; addl $" RW_LOCK_BIAS_STR \
		",%0":"=m" ((rw)->lock) : : "memory"
 
// #define spin_unlock_string \
// 	"movb $1,%0" \
// 		:"=m" (lock->lock) : : "memory"


// static inline void spin_unlock(spinlock_t *lock)
// { 
// 	__asm__ __volatile__(
// 		spin_unlock_string
// 	);
// }
 

 
#define spin_unlock_string \
	"xchgb %b0, %1" \
		:"=q" (oldval), "=m" (lock->lock) \
		:"0" (oldval) : "memory"


 
static inline void spin_unlock(spinlock_t *lock)
{
	char oldval = 1; 
	__asm__ __volatile__(
		spin_unlock_string
	);
}

 
static inline int spin_trylock(spinlock_t *lock)
{
	char oldval;
	__asm__ __volatile__(
		"xchgb %b0,%1"
		:"=q" (oldval), "=m" (lock->lock)
		:"0" (0) : "memory");
	return oldval > 0;
}
 
static inline void spin_lock(spinlock_t *lock)
{ 
	__asm__ __volatile__(
		spin_lock_string
		:"=m" (lock->lock) : : "memory");
}


 
typedef struct {
	volatile unsigned int lock; 
} rwlock_t;
 
 
#define RW_LOCK_UNLOCKED (rwlock_t) { RW_LOCK_BIAS  }
 
#define rwlock_init(x)	do { *(x) = RW_LOCK_UNLOCKED; } while(0)

 
static inline void read_lock(rwlock_t *rw)
{ 
	__build_read_lock(rw, "__read_lock_failed");
}

 
static inline void write_lock(rwlock_t *rw)
{ 
	__build_write_lock(rw, "__write_lock_failed");
}

 
static inline int write_trylock(rwlock_t *lock)
{
	atomic_t *count = (atomic_t *)lock;
	if (atomic_sub_and_test(RW_LOCK_BIAS, count))
		return 1;
	atomic_add(RW_LOCK_BIAS, count);
	return 0;
}



#define spin_lock_irqsave(lock, flags)		do { local_irq_save(flags);       spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags)	do { spin_unlock(lock);  local_irq_restore(flags); } while (0)


#endif 
