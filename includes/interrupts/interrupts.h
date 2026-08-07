#ifndef __INTERRUPT_H__
#define __INTERRUPT_H__

/*
    0-0x15     system used
    0x16-0x1f  reserved for system
    0x20-0xff  hardware interrupt,0x80 for system call
*/

// system default exception code
#define FAULT_DIVISTION_ERROR_CODE 0x0
#define TRAP_DEBUG_CODE 0x1
#define INTN_NMI_CODE 0x2
#define TRAP_BREAKPOINT_CODE 0x3
#define TRAP_OVERFLOW_CODE 0x4
#define FAULT_OUT_OF_BOUND_CODE 0x5
#define FAULT_INVALID_OPCODE_CODE 0x6
#define FAULT_NO_MATH_CODE 0x7
#define ABORT_DOUBLE_FAULT_CODE 0x8
#define FAULT_COPROCESSOR_SEGMENT_OVERRUN_CODE 0x9
#define FAULT_INVALID_TSS_CODE 0xA
#define FAULT_SEGMENT_NOT_PRESENT_CODE 0xB
#define FAULT_STACK_SEGMENT_FAULT_CODE 0xC
#define FAULT_GENERAL_PROTECTION_CODE 0xD
#define FAULT_PAGE_FAULT_CODE 0xE
#define FAULT_RESERVED_CODE 0xF
#define FAULT_X87_FPU_FP_ERROR_CODE 0x10
#define FAULT_ALIGNMENT_CHECK_CODE 0x11
#define ABORT_MACHINE_CHECK_CODE 0x12
#define FAULT_SIMD_FP_EXCEPTION_CODE 0x13
#define FAULT_VIRTUALIZATION_EXCEPTION_CODE 0x14
#define FAULT_CONTROL_PROTECTION_EXCEPTION_CODE 0x15 


#define FIRST_EXTERNAL_VECTOR	0x20
#define SYSCALL_VECTOR		0x80
#define	TIMER_APIC_VECTOR	0xEF  // APIC Timer 中断向量（不与 SPURIOUS/ERROR 冲突）
#define ERROR_APIC_VECTOR	0xfe // 错误向量
#define FIRST_DEVICE_VECTOR	0x31
#define FIRST_SYSTEM_VECTOR	0xef
#define NR_IRQS            224 // 设备 IRQ 数量（0x20~0xFF = 224）   
#define NR_VECTORS 256


void _init_interrupts();

struct pt_regs;
 

/* 注册/注销设备中断处理函数 */
int  request_irq(unsigned int vector,
                 void (*handler)(int irq, void *dev_id, struct pt_regs *regs),
                 const char *name,
                 void *dev_id);
void free_irq(unsigned int vector);

/* ====== 系统调用 ====== */
/* 系统调用号（Linux ABI 兼容） */
#define SYS_EXIT     1
#define SYS_FORK     2
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_GETPID   20
#define NR_SYSCALLS  256   /* 系统调用表大小 */

/* 系统调用入口（entry.S 调用） */
long do_syscall(struct pt_regs *regs);

/* 初始化系统调用表（内核启动时调用） */
void syscall_init(void);

/* 注册系统调用处理函数 */
typedef long (*syscall_fn_t)(long, long, long, long, long);
int register_syscall(unsigned int nr, syscall_fn_t fn);

#endif