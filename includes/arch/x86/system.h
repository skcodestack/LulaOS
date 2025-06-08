#ifndef __SYSTEM_H__
#define __SYSTEM_H__
#include <arch/x86/page.h>

#define LOCK "lock ; " 

#define barrier() __asm__ __volatile__("": : :"memory")

#define load_cr3(x) \
	__asm__ __volatile__("movl %0,%%cr3": :"r" (__pa(x)))

#endif