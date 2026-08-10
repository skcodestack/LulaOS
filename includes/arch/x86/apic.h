#ifndef __APIC_H__
#define __APIC_H__
#include <arch/x86/highmem.h>
#include <arch/linkage.h>

struct pt_regs; // forward declaration


#define	APIC_DEFAULT_PHYS_BASE	0xfee00000       /// default phy addr
#define APIC_BASE (fix_to_virt(FIX_APIC_BASE))  /// remapping virt addr



// Id寄存器
#define	APIC_ID		0x20
#define	APIC_ID_MASK		(0x0F<<24)
#define	GET_APIC_ID(x)		(((x)>>24)&0x0F)

//version
#define APIC_VERSION     0x30
#define APIC_VERSION_MASK 0xFF
#define GET_APIC_VERSION(x) (x&0xFF)
#define GET_MAX_LVT_ENTRY(x) ((x>>16)&0xFF)
#define GET_SVR_SUPPORT12(x) ((x>>24)&0x1)


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

// ICR
#define	APIC_ICRLO	0x300
#define	APIC_ICRHI	0x310
//LVT TIMER寄存器
#define	APIC_TIMER	0x320
#define	APIC_TIMER_INITCNT	0x380   // 初始计数寄存器
#define	APIC_TIMER_CURRCNT	0x390   // 当前计数寄存器
#define	APIC_TIMER_DIVIDE	0x3E0   // 分频配置寄存器

//LVT  LINT0寄存器
#define	APIC_LINT0	0x350
//LVT  LINT1寄存器
#define APIC_LINT1	0x360
//LVT  错误寄存器
#define	APIC_ERR	0x370    



/// vector mask
#define APIC_VECTOR_MASK 0xFF

/// 交付传递方式
#define	APIC_DM_FIXED		0x00000
#define	APIC_DM_LOWEST		0x00100
#define	APIC_DM_SMI		0x00200
#define	APIC_DM_REMRD		0x00300
#define	APIC_DM_NMI		0x00400
#define	APIC_DM_INIT		0x00500
#define	APIC_DM_STARTUP		0x00600
#define	APIC_DM_EXTINT		0x00700
/// 交付传递状态
#define APIC_DS_IDLE 0x00000
#define APIC_DS_PENDING (1<<12)
/// local 中断屏蔽位 
#define	APIC_LVT_MASKED	(1<<16)
/// timer 计数模式
#define APIC_TM_ONESHOT 0x00000
#define APIC_TM_PERIODIC (1<<17)
#define APIC_TM_TSC_DEADLINE (1<<18)
/// 目的地模式
#define APIC_DEST_MODE_PHY 0x00000 
#define APIC_DEST_MODE_LOGIC (1<<11)
/// level 
#define APIC_LEVEL_DEASSERT 0x00000 
#define APIC_LEVEL_ASSERT (1<<14)
/// 触发模式
#define APIC_TRIGGER_MODE_EDGE 0x00000
#define APIC_TRIGGER_MODE_LEVEL (1<<15)


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
#define IOAPIC_BASE(idx) (fix_to_virt(FIX_IO_APIC_BASE_0 + (idx)))
#define IOAPIC_REG_SEL(base) *((volatile unsigned int*)((base) + 0x00))
#define IOAPIC_REG_WIN(base) *((volatile unsigned int*)((base) + 0x10))
#define IOAPIC_REG_EOI(base) *((volatile unsigned int*)((base) + 0x40))


#define	IOAPIC_ID		0x00
#define	GET_IOAPIC_ID(x)	(((x)>>24)&0x0F)

#define	IOAPIC_VERSION		0x01
#define GET_IOAPIC_VERSION(x) (x & 0x0F)
#define GET_IOAPIC_RTE_COUNT(x) ((x>>16) & 0xFF)

struct ioapic_rte_entry{
	/* 低 32 位 */
	unsigned int vector:        8;   /* [0:7]   中断向量 */
	unsigned int delivery_mode: 3;   /* [8:10]  交付模式 */
	unsigned int dest_mode:     1;   /* [11]    目的地模式 0=物理 1=逻辑 */
	unsigned int delivery_status:1;  /* [12]    交付状态（只读） */
	unsigned int pin_polarity:  1;   /* [13]    引脚极性 0=高有效 1=低有效 */
	unsigned int irr_flag:      1;   /* [14]    IRR（只读） */
	unsigned int trigger_mode:  1;   /* [15]    触发模式 0=边缘 1=电平 */
	unsigned int mask:          1;   /* [16]    屏蔽位 1=屏蔽 */
	unsigned int reserved_low:  15;  /* [17:31] 保留，必须为0 */
	/* 高 32 位 */
	unsigned int reserved_high: 24;  /* [32:55] 保留，必须为0 */
	union 
	{
		struct 
		{
			unsigned int physical_dest	:  4;  /* 物理目标 APIC ID */
			unsigned int __reserved_2	:  4;
		} physical;
		struct 
		{
			unsigned int logical_dest	:  8; /* 逻辑目标 */
		} logical;
	} dest;                              /* [56:63] 目标 CPU */
};

static __inline__ void ioapic_write(unsigned long base, unsigned int reg, unsigned int value){
	IOAPIC_REG_SEL(base) = reg;
	IOAPIC_REG_WIN(base) = value;
}

static __inline__ unsigned int ioapic_read(unsigned long base, unsigned int reg){
	IOAPIC_REG_SEL(base) = reg;
	return IOAPIC_REG_WIN(base);
}

// 写入 IOAPIC RTE 表项（先高后低）
void __ioapic_write_entry(unsigned long base, int pin, struct ioapic_rte_entry entry);

/* IOAPIC RTE 配置函数 */
void ioapic_set_rte(unsigned int gsi, unsigned int vector,
                    unsigned int trigger, unsigned int polarity,
                    unsigned int dest_apic, unsigned int mask);
void ioapic_enable_irq(unsigned int vector);
void ioapic_disable_irq(unsigned int vector);

/* PIT 8254 定时器常量（用于 APIC Timer 校准） */
#define PIT_CH2_DATA        0x42    // Channel 2 数据端口
#define PIT_CMD             0x43    // 命令寄存器
#define PIT_CH2_GATE        0x61    // NMI/Channel2 控制端口
#define PIT_FREQ            1193182 // PIT 基础频率（Hz）
#define APIC_CAL_MS         10      // 校准时间窗口（毫秒）
#define APIC_CAL_PIT_COUNT  ((PIT_FREQ * APIC_CAL_MS) / 1000)  // PIT 校准计数值

void calibrate_apic_timer(void);
void local_apic_init_ap(void);
void do_apic_timer_interrupt(struct pt_regs *regs, long error_code);
void do_apic_error_interrupt(struct pt_regs *regs, long error_code);

/* IPI / SMP 启动 */
void send_ipi(int apic_id, int vector);
void send_startup_ipi(int apic_id, unsigned int startup_page);

/* 微秒延迟（基于 PIT Channel 2 轮询） */
void udelay(unsigned int us);

void _init_apic();
#endif