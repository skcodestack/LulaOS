#ifndef __APIC_H__
#define __APIC_H__
#include <arch/x86/highmem.h>
#include <arch/linkage.h>


#define	APIC_DEFAULT_PHYS_BASE	0xfee00000       /// default phy addr
#define APIC_BASE (fix_to_virt(FIX_APIC_BASE))  /// remapping virt addr

// Id寄存器
#define		APIC_ID		0x20
#define		APIC_ID_MASK		(0x0F<<24)
#define		GET_APIC_ID(x)		(((x)>>24)&0x0F)



static __inline__ void apic_write(unsigned long reg, unsigned long v)
{
	*((volatile unsigned long *)(APIC_BASE+reg)) = v;
}

static __inline__ void apic_write_atomic(unsigned long reg, unsigned long v)
{
	xchg((volatile unsigned long *)(APIC_BASE+reg), v);
}

static __inline__ unsigned long apic_read(unsigned long reg)
{
	return *((volatile unsigned long *)(APIC_BASE+reg));
}

void _init_apic();
#endif