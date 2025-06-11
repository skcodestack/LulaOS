#ifndef __BITOPS_H__
#define __BITOPS_H__
#include <arch/linkage.h>
#include <arch/x86/system.h>

#define ADDR(x) (*(volatile long *) (x))

/**
 * clear nr bit,and return old value
 * 
 * btrl -- test and clear 
 * sbbl --  dest-src-zf 
 */
static __inline__ int test_and_clear_bit(int nr, volatile void *addr){
    int oldbit;
    __asm__ __volatile__(LOCK 
                          "btrl %2,%1\n\tsbbl %0,%0"
                            :"=r" (oldbit),"=m" (ADDR(addr))
                            :"Ir" (nr) : "memory");
    return oldbit;
}

static  __inline__ int  test_and_set_bit(int nr, volatile void *addr){
    int oldbit;
   __asm__ __volatile__( LOCK
                        "btsl %2,%1\n\tsbbl %0,%0"
                        :"=r" (oldbit),"=m" (ADDR(addr))
                        :"Ir" (nr) : "memory");
	return oldbit;
}

static __inline__ int test_bit(int nr, volatile void *addr){ 
    int oldbit;

	__asm__ __volatile__(
		"btl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit)
		:"m" (ADDR(addr)),"Ir" (nr));
	return oldbit;
}

static __inline__ void set_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__( LOCK
		"btsl %1,%0"
		:"=m" (ADDR(addr))
		:"Ir" (nr));
}

static __inline__ void clear_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__( LOCK
		"btrl %1,%0"
		:"=m" (ADDR(addr))
		:"Ir" (nr));
}

static __inline__ int __test_and_change_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btcl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR(addr))
		:"Ir" (nr) : "memory");
	return oldbit;
}

#endif