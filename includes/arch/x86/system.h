#ifndef __SYSTEM_H__
#define __SYSTEM_H__
#include <arch/x86/page.h>

#define LOCK "lock ; " 

#define local_irq_save(x)	__asm__ __volatile__("pushfl ; popl %0 ; cli":"=g" (x):   :"memory")
#define local_irq_restore(x)	__restore_flags(x)
#define local_irq_disable()	__cli()
#define local_irq_enable()	__sti()

#define barrier() __asm__ __volatile__("": : :"memory")

#define load_cr3(x) \
	__asm__ __volatile__("movl %0,%%cr3": :"r" (__pa(x)))

#endif