#ifndef __APIC_H__
#define __APIC_H__
#include <arch/x86/highmem.h>
#include <arch/linkage.h>


#define	APIC_DEFAULT_PHYS_BASE	0xfee00000       /// default phy addr
#define APIC_BASE (fix_to_virt(FIX_APIC_BASE))  /// remapping virt addr


#define APIC_VECTOR_MASK 0xFF

// Id寄存器
#define	APIC_ID		0x20
#define	APIC_ID_MASK		(0x0F<<24)
#define	GET_APIC_ID(x)		(((x)>>24)&0x0F)

//version
#define APIC_VERSION     0x30

//TPR寄存器 task优先级
#define	APIC_TPR	0x80
#define	APIC_TPR_MASK		0xFF
#define APIC_TPR_VALUE(p,sp) ((p<<4) + sp)

//EOI寄存器
#define	APIC_EOI	0xB0
#define	APIC_EIO_ACK		0x0		/* Write this to the EOI register */

//SVR寄存器 伪中断向量（bit8：APIC使能，bit9:焦点处理器检测,bit12:EOI使能）
#define	APIC_SPIV	0xF0
#define	APIC_SPIV_FOCUS_DISABLED	(1<<9)
#define	APIC_SPIV_APIC_ENABLED		(1<<8)
#define SPURIOUS_APIC_VECTOR	0xff

//LVT  LINT0寄存器
#define	APIC_LINT0	0x350
//LVT  LINT1寄存器
#define APIC_LINT1	0x360
//LVT  错误寄存器
#define	APIC_ERR	0x370
#define ERROR_APIC_VECTOR	0xfe     

#define	APIC_LVT_MASKED	(1<<16)

/// 
#define	APIC_DM_FIXED		0x00000
#define	APIC_DM_LOWEST		0x00100
#define	APIC_DM_SMI		0x00200
#define	APIC_DM_REMRD		0x00300
#define	APIC_DM_NMI		0x00400
#define	APIC_DM_INIT		0x00500
#define	APIC_DM_STARTUP		0x00600
#define	APIC_DM_EXTINT		0x00700

#define APIC_DEST_SELF      0x00040000  // 发送给自己
#define APIC_DEST_ALL       0x00080000  // 发送给所有处理器（包括自己）
#define APIC_DEST_ALL_EXCL  0x000C0000  // 发送给所有处理器（no 自己）


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



/*    ioapic  */
#define IOAPIC_DEFAULT_PHYS_BASE 0xFEC00000
#define IOAPIC_BASE (fix_to_virt(FIX_IO_APIC_BASE_0))
#define IOAPIC_REG_SEL *((volatile unsigned int*)(IOAPIC_BASE + 0x00))
#define IOAPIC_REG_WIN *((volatile unsigned int*)(IOAPIC_BASE + 0x10))
#define IOAPIC_REG_EOI *((volatile unsigned int*)(IOAPIC_BASE + 0x40))


#define	IOAPIC_ID		0x00
#define	GET_IOAPIC_ID(x)	(((x)>>24)&0x0F)

#define	IOAPIC_VERSION		0x01
#define GET_IOAPIC_VERSION(x) (x & 0x0F)
#define GET_IOAPIC_RTE_COUNT(x) ((x>>16) & 0xFF)

struct ioapic_rte_entry{
	unsigned int vector: 8;
	unsigned int delivery_mode: 3;
	unsigned int dest_mode: 1;
	unsigned int delivery_status:1;
	unsigned int level_trigger:1;
	unsigned int irr_flag:1;
	unsigned int trigger_mode:1;
	unsigned int mask:1;
	unsigned int resver1: 32;
	unsigned int resver2: 7;
	union 
	{
		struct 
		{
			unsigned int physical_dest	:  4;
			unsigned int __reserved_2	:  4;
		} physical;
		struct 
		{
			unsigned int logical_dest	:  8; 
		} logical;
	} dest; 
};

static __inline__ void ioapic_write(unsigned int reg, unsigned int value){
	IOAPIC_REG_SEL = reg;
	IOAPIC_REG_WIN = value;
}

static __inline__ unsigned int ioapic_read(unsigned int reg){
	IOAPIC_REG_SEL = reg;
	return IOAPIC_REG_WIN;
}


void _init_apic();
#endif