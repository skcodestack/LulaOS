// #ifndef _SEMAPHORE_H
// #define _SEMAPHORE_H

// #include <arch/linkage.h>   

// #include <arch/x86/system.h>
// #include <arch/x86/atomic.h>
// #include <wait.h>

 
// struct semaphore {
// 	atomic_t count;
// 	int sleepers;		 
// 	wait_queue_head_t wait; 
// };
 
 
// #define __SEMAPHORE_INITIALIZER(name,count) \
// { ATOMIC_INIT(count), 0, __WAIT_QUEUE_HEAD_INITIALIZER((name).wait)}

// #define __MUTEX_INITIALIZER(name) \
// 	__SEMAPHORE_INITIALIZER(name,1)

// #define __DECLARE_SEMAPHORE_GENERIC(name,count) \
// 	struct semaphore name = __SEMAPHORE_INITIALIZER(name,count)

// #define DECLARE_MUTEX(name) __DECLARE_SEMAPHORE_GENERIC(name,1)
// #define DECLARE_MUTEX_LOCKED(name) __DECLARE_SEMAPHORE_GENERIC(name,0)

 
// static inline void sema_init (struct semaphore *sem, int val)
// { 
// 	atomic_set(&sem->count, val);
// 	sem->sleepers = 0;
// 	init_waitqueue_head(&sem->wait); 
// }

 
// static inline void init_MUTEX (struct semaphore *sem)
// {
// 	sema_init(sem, 1); 
// }

 
// static inline void init_MUTEX_LOCKED (struct semaphore *sem)
// {
// 	sema_init(sem, 0);
// }

// //semaphore.c
// asmlinkage void __down_failed(void /* special register calling convention */);
// asmlinkage int  __down_failed_interruptible(void  /* params in registers */);
// asmlinkage int  __down_failed_trylock(void  /* params in registers */);
// asmlinkage void __up_wakeup(void /* special register calling convention */);

// asmlinkage void __down(struct semaphore * sem);
// asmlinkage int  __down_interruptible(struct semaphore * sem);
// asmlinkage int  __down_trylock(struct semaphore * sem);
// asmlinkage void __up(struct semaphore * sem);

 
// static inline void down(struct semaphore * sem)
// { 
// 	__asm__ __volatile__(
// 		"# atomic down operation\n\t"
// 		LOCK "decl %0\n\t"     
// 		"js 2f\n"
// 		"1:\n"
// 		".section .text.lock,\"ax\"\n"
// 		"2:\tcall __down_failed\n\t"
// 		"jmp 1b\n"
// 		".previous"
// 		:"=m" (sem->count)
// 		:"c" (sem)
// 		:"memory");
// }
 
// static inline int down_interruptible(struct semaphore * sem)
// {
// 	int result;
 

// 	__asm__ __volatile__(
// 		"# atomic interruptible down operation\n\t"
// 		LOCK "decl %1\n\t"      
// 		"js 2f\n\t"
// 		"xorl %0,%0\n"
// 		"1:\n"
// 		".section .text.lock,\"ax\"\n"
// 		"2:\tcall __down_failed_interruptible\n\t"
// 		"jmp 1b\n"
// 		".previous"
// 		:"=a" (result), "=m" (sem->count)
// 		:"c" (sem)
// 		:"memory");
// 	return result;
// }
 
// static inline int down_trylock(struct semaphore * sem)
// {
// 	int result;
 
// 	__asm__ __volatile__(
// 		"# atomic interruptible down operation\n\t"
// 		LOCK "decl %1\n\t"      
// 		"js 2f\n\t"
// 		"xorl %0,%0\n"
// 		"1:\n"
// 		".section .text.lock,\"ax\"\n"
// 		"2:\tcall __down_failed_trylock\n\t"
// 		"jmp 1b\n"
// 		".previous"
// 		:"=a" (result), "=m" (sem->count)
// 		:"c" (sem)
// 		:"memory");
// 	return result;
// }
 
// static inline void up(struct semaphore * sem)
// { 

 
// 	__asm__ __volatile__(
// 		"# atomic up operation\n\t"
// 		LOCK "incl %0\n\t"     
// 		"jle 2f\n"
// 		"1:\n"
// 		".section .text.lock,\"ax\"\n"
// 		"2:\tcall __up_wakeup\n\t"
// 		"jmp 1b\n"
// 		".previous"
// 		:"=m" (sem->count)
// 		:"c" (sem)
// 		:"memory");
// }
 
// #endif
