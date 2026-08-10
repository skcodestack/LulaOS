#ifndef __SYSTEM_H__
#define __SYSTEM_H__

/* LOCK 前缀必须在 page.h 之前定义，
 * 否则经 page.h → mm.h → spinlock.h → rwlock.h 的包含链
 * 到达 rwlock.h 时 LOCK 尚未定义，导致内联汇编编译失败 */
#define LOCK "lock ; "

#include <arch/x86/page.h>


#define __save_flags(x)		__asm__ __volatile__("pushfl ; popl %0":"=g" (x): )
#define __restore_flags(x) 	__asm__ __volatile__("pushl %0 ; popfl":  :"g" (x):"memory", "cc")
#define __cli() 		__asm__ __volatile__("cli": : :"memory")
#define __sti()			__asm__ __volatile__("sti": : :"memory") 
//sti打开中断，hlt 进入停机状态
#define safe_halt()		__asm__ __volatile__("sti; hlt": : :"memory")

#define local_irq_save(x)	__asm__ __volatile__("pushfl ; popl %0 ; cli":"=g" (x):   :"memory")
#define local_irq_restore(x)	__restore_flags(x)
#define local_irq_disable()	__cli()
#define local_irq_enable()	__sti()

#define barrier() __asm__ __volatile__("": : :"memory")

#define load_cr3(x) \
	__asm__ __volatile__("movl %0,%%cr3": :"r" (__pa(x)))

#define cli() __cli()
#define sti() __sti()
#define save_flags(x) __save_flags(x)
#define restore_flags(x) __restore_flags(x)
#endif