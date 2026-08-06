#include <arch/linkage.h>
#include <arch/x86/segment.h>

EBX = 0x00
ECX = 0x04
EDX = 0x08
ESI = 0x0C
EDI = 0x10
EBP = 0x14
ES = 0x18
DS = 0x1C
EAX = 0x20
FUNC = 0x24
ERROR_CODE = 0x28 
EIP = 0x2C
CS = 0x30
EFLAGS = 0x34
OLD_ESP = 0x38
OLD_SS = 0x3C



SAVE_ALL:

RESTORE_ALL:
    popl %ebx
    popl %ecx
    popl %edx
    popl %esi
    popl %edi
    popl %ebp
    popl %eax
    movl %eax,%es
    popl %eax
    movl %eax,%ds
    popl %eax
    addl $8,%esp //pop FUNC/ERROR_CODE
    iret


ENTRY(ret_from_intr)
    jmp	RESTORE_ALL

/* interrupt warpper function */

interrupt_warpper:
    pushl %eax
    movl %ds, %eax
    pushl %eax
    movl %es, %eax
    pushl %eax
    pushl %ebp
    pushl %edi
    pushl %esi
    pushl %edx
    pushl %ecx
    pushl %ebx

    cld 

    movl FUNC(%esp) ,%edi
    movl ERROR_CODE(%esp), %esi
    movl %esp,%edx

    /*push params to stack */
    pushl %esi   /*error code */
    pushl %edx   /*pt_regs */

    movl $__KERNEL_DS , %eax
    movl %eax, %ds
    movl %eax, %es

    call *%edi
    addl $8, %esp /*pop params to stack */
    jmp ret_from_intr

ENTRY(divide_error)
    pushl $0   /*error code*/
    pushl $SYMBOL_NAME(do_divide_error) 
    jmp interrupt_warpper

ENTRY(debug)
	pushl $0
	pushl $ SYMBOL_NAME(do_debug)
	jmp interrupt_warpper

ENTRY(nmi)
    pushl $0
	pushl $ SYMBOL_NAME(do_nmi)
	jmp interrupt_warpper

ENTRY(int3)
	pushl $0
	pushl $ SYMBOL_NAME(do_int3)
	jmp interrupt_warpper

ENTRY(overflow)
	pushl $0		 
	pushl $ SYMBOL_NAME(do_overflow)  
	jmp interrupt_warpper

ENTRY(bounds)
	pushl $0
	pushl $ SYMBOL_NAME(do_bounds)
	jmp interrupt_warpper

ENTRY(invalid_op)
	pushl $0
	pushl $ SYMBOL_NAME(do_invalid_op)
	jmp interrupt_warpper

ENTRY(device_not_available)
    pushl $0
	pushl $ SYMBOL_NAME(do_device_not_available)
	jmp interrupt_warpper

ENTRY(double_fault)
	pushl $ SYMBOL_NAME(do_double_fault)
	jmp interrupt_warpper

ENTRY(coprocessor_segment_overrun)
	pushl $0
	pushl $ SYMBOL_NAME(do_coprocessor_segment_overrun)
	jmp interrupt_warpper

ENTRY(invalid_TSS)
	pushl $ SYMBOL_NAME(do_invalid_TSS)
	jmp interrupt_warpper

ENTRY(segment_not_present)
	pushl $ SYMBOL_NAME(do_segment_not_present)
	jmp interrupt_warpper

ENTRY(stack_segment)
	pushl $ SYMBOL_NAME(do_stack_segment)
	jmp interrupt_warpper

ENTRY(general_protection)
	pushl $ SYMBOL_NAME(do_general_protection)
	jmp interrupt_warpper

ENTRY(page_fault)
	pushl $ SYMBOL_NAME(do_page_fault)
	jmp interrupt_warpper

ENTRY(coprocessor_error)
	pushl $0
	pushl $ SYMBOL_NAME(do_coprocessor_error)
	jmp interrupt_warpper

ENTRY(alignment_check)
	pushl $ SYMBOL_NAME(do_alignment_check)
	jmp interrupt_warpper

ENTRY(machine_check)
	pushl $0
	pushl $ SYMBOL_NAME(do_machine_check)
	jmp interrupt_warpper

ENTRY(simd_coprocessor_error)
	pushl $0
	pushl $ SYMBOL_NAME(do_simd_coprocessor_error)
	jmp interrupt_warpper

ENTRY(virtualization_exception)
	pushl $0
	pushl $ SYMBOL_NAME(do_virtualization_exception)
	jmp interrupt_warpper

/* ====== 硬件中断入口 ======
 * 硬件中断 CPU 不压 error code，此处压入向量号（vector）
 * do_IRQ 通过该值判断是哪个中断源，路由到对应设备驱动
 */

/* 向量常量（与 interrupts.h 保持一致） */
VECTOR_TIMER    = 0xEF
VECTOR_ERROR    = 0xFE

/*
 * BUILD_IRQ(n) - 为向量 n 生成独立入口桩
 * 压入向量号 n，而非 $0，使 do_IRQ 能识别中断源
 */
#define BUILD_IRQ(n)         \
    ENTRY(irq_entry_##n)     \
        pushl $n             \
        pushl $ SYMBOL_NAME(do_IRQ)  \
        jmp interrupt_warpper

BUILD_IRQ(0x20)
BUILD_IRQ(0x21)
BUILD_IRQ(0x22)
BUILD_IRQ(0x23)
BUILD_IRQ(0x24)
BUILD_IRQ(0x25)
BUILD_IRQ(0x26)
BUILD_IRQ(0x27)
BUILD_IRQ(0x28)
BUILD_IRQ(0x29)
BUILD_IRQ(0x2a)
BUILD_IRQ(0x2b)
BUILD_IRQ(0x2c)
BUILD_IRQ(0x2d)
BUILD_IRQ(0x2e)
BUILD_IRQ(0x2f)
BUILD_IRQ(0x30)
BUILD_IRQ(0x31)
BUILD_IRQ(0x32)
BUILD_IRQ(0x33)
BUILD_IRQ(0x34)
BUILD_IRQ(0x35)
BUILD_IRQ(0x36)
BUILD_IRQ(0x37)
BUILD_IRQ(0x38)
BUILD_IRQ(0x39)
BUILD_IRQ(0x3a)
BUILD_IRQ(0x3b)
BUILD_IRQ(0x3c)
BUILD_IRQ(0x3d)
BUILD_IRQ(0x3e)
BUILD_IRQ(0x3f)
BUILD_IRQ(0x40)
BUILD_IRQ(0x41)
BUILD_IRQ(0x42)
BUILD_IRQ(0x43)
BUILD_IRQ(0x44)
BUILD_IRQ(0x45)
BUILD_IRQ(0x46)
BUILD_IRQ(0x47)
BUILD_IRQ(0x48)
BUILD_IRQ(0x49)
BUILD_IRQ(0x4a)
BUILD_IRQ(0x4b)
BUILD_IRQ(0x4c)
BUILD_IRQ(0x4d)
BUILD_IRQ(0x4e)
BUILD_IRQ(0x4f)
BUILD_IRQ(0x50)
BUILD_IRQ(0x51)
BUILD_IRQ(0x52)
BUILD_IRQ(0x53)
BUILD_IRQ(0x54)
BUILD_IRQ(0x55)
BUILD_IRQ(0x56)
BUILD_IRQ(0x57)
BUILD_IRQ(0x58)
BUILD_IRQ(0x59)
BUILD_IRQ(0x5a)
BUILD_IRQ(0x5b)
BUILD_IRQ(0x5c)
BUILD_IRQ(0x5d)
BUILD_IRQ(0x5e)
BUILD_IRQ(0x5f)
BUILD_IRQ(0x60)
BUILD_IRQ(0x61)
BUILD_IRQ(0x62)
BUILD_IRQ(0x63)
BUILD_IRQ(0x64)
BUILD_IRQ(0x65)
BUILD_IRQ(0x66)
BUILD_IRQ(0x67)
BUILD_IRQ(0x68)
BUILD_IRQ(0x69)
BUILD_IRQ(0x6a)
BUILD_IRQ(0x6b)
BUILD_IRQ(0x6c)
BUILD_IRQ(0x6d)
BUILD_IRQ(0x6e)
BUILD_IRQ(0x6f)
BUILD_IRQ(0x70)
BUILD_IRQ(0x71)
BUILD_IRQ(0x72)
BUILD_IRQ(0x73)
BUILD_IRQ(0x74)
BUILD_IRQ(0x75)
BUILD_IRQ(0x76)
BUILD_IRQ(0x77)
BUILD_IRQ(0x78)
BUILD_IRQ(0x79)
BUILD_IRQ(0x7a)
BUILD_IRQ(0x7b)
BUILD_IRQ(0x7c)
BUILD_IRQ(0x7d)
BUILD_IRQ(0x7e)
BUILD_IRQ(0x7f)
/* 0x80 is SYSCALL_VECTOR, skip */
BUILD_IRQ(0x81)
BUILD_IRQ(0x82)
BUILD_IRQ(0x83)
BUILD_IRQ(0x84)
BUILD_IRQ(0x85)
BUILD_IRQ(0x86)
BUILD_IRQ(0x87)
BUILD_IRQ(0x88)
BUILD_IRQ(0x89)
BUILD_IRQ(0x8a)
BUILD_IRQ(0x8b)
BUILD_IRQ(0x8c)
BUILD_IRQ(0x8d)
BUILD_IRQ(0x8e)
BUILD_IRQ(0x8f)
BUILD_IRQ(0x90)
BUILD_IRQ(0x91)
BUILD_IRQ(0x92)
BUILD_IRQ(0x93)
BUILD_IRQ(0x94)
BUILD_IRQ(0x95)
BUILD_IRQ(0x96)
BUILD_IRQ(0x97)
BUILD_IRQ(0x98)
BUILD_IRQ(0x99)
BUILD_IRQ(0x9a)
BUILD_IRQ(0x9b)
BUILD_IRQ(0x9c)
BUILD_IRQ(0x9d)
BUILD_IRQ(0x9e)
BUILD_IRQ(0x9f)
BUILD_IRQ(0xa0)
BUILD_IRQ(0xa1)
BUILD_IRQ(0xa2)
BUILD_IRQ(0xa3)
BUILD_IRQ(0xa4)
BUILD_IRQ(0xa5)
BUILD_IRQ(0xa6)
BUILD_IRQ(0xa7)
BUILD_IRQ(0xa8)
BUILD_IRQ(0xa9)
BUILD_IRQ(0xaa)
BUILD_IRQ(0xab)
BUILD_IRQ(0xac)
BUILD_IRQ(0xad)
BUILD_IRQ(0xae)
BUILD_IRQ(0xaf)
BUILD_IRQ(0xb0)
BUILD_IRQ(0xb1)
BUILD_IRQ(0xb2)
BUILD_IRQ(0xb3)
BUILD_IRQ(0xb4)
BUILD_IRQ(0xb5)
BUILD_IRQ(0xb6)
BUILD_IRQ(0xb7)
BUILD_IRQ(0xb8)
BUILD_IRQ(0xb9)
BUILD_IRQ(0xba)
BUILD_IRQ(0xbb)
BUILD_IRQ(0xbc)
BUILD_IRQ(0xbd)
BUILD_IRQ(0xbe)
BUILD_IRQ(0xbf)
BUILD_IRQ(0xc0)
BUILD_IRQ(0xc1)
BUILD_IRQ(0xc2)
BUILD_IRQ(0xc3)
BUILD_IRQ(0xc4)
BUILD_IRQ(0xc5)
BUILD_IRQ(0xc6)
BUILD_IRQ(0xc7)
BUILD_IRQ(0xc8)
BUILD_IRQ(0xc9)
BUILD_IRQ(0xca)
BUILD_IRQ(0xcb)
BUILD_IRQ(0xcc)
BUILD_IRQ(0xcd)
BUILD_IRQ(0xce)
BUILD_IRQ(0xcf)
BUILD_IRQ(0xd0)
BUILD_IRQ(0xd1)
BUILD_IRQ(0xd2)
BUILD_IRQ(0xd3)
BUILD_IRQ(0xd4)
BUILD_IRQ(0xd5)
BUILD_IRQ(0xd6)
BUILD_IRQ(0xd7)
BUILD_IRQ(0xd8)
BUILD_IRQ(0xd9)
BUILD_IRQ(0xda)
BUILD_IRQ(0xdb)
BUILD_IRQ(0xdc)
BUILD_IRQ(0xdd)
BUILD_IRQ(0xde)
BUILD_IRQ(0xdf)
BUILD_IRQ(0xe0)
BUILD_IRQ(0xe1)
BUILD_IRQ(0xe2)
BUILD_IRQ(0xe3)
BUILD_IRQ(0xe4)
BUILD_IRQ(0xe5)
BUILD_IRQ(0xe6)
BUILD_IRQ(0xe7)
BUILD_IRQ(0xe8)
BUILD_IRQ(0xe9)
BUILD_IRQ(0xea)
BUILD_IRQ(0xeb)
BUILD_IRQ(0xec)
BUILD_IRQ(0xed)
BUILD_IRQ(0xee)
BUILD_IRQ(0xef)
BUILD_IRQ(0xf0)
BUILD_IRQ(0xf1)
BUILD_IRQ(0xf2)
BUILD_IRQ(0xf3)
BUILD_IRQ(0xf4)
BUILD_IRQ(0xf5)
BUILD_IRQ(0xf6)
BUILD_IRQ(0xf7)
BUILD_IRQ(0xf8)
BUILD_IRQ(0xf9)
BUILD_IRQ(0xfa)
BUILD_IRQ(0xfb)
BUILD_IRQ(0xfc)
BUILD_IRQ(0xfd)
BUILD_IRQ(0xfe)
BUILD_IRQ(0xff)

/* APIC Timer 专用入口（系统向量 0xEF）*/
ENTRY(apic_timer_entry)
    pushl $VECTOR_TIMER
    pushl $ SYMBOL_NAME(do_apic_timer_interrupt)
    jmp interrupt_warpper

/* APIC Error 专用入口（系统向量 0xFE）*/
ENTRY(apic_error_entry)
    pushl $VECTOR_ERROR
    pushl $ SYMBOL_NAME(do_apic_error_interrupt)
    jmp interrupt_warpper

/* ====== 系统调用入口 int 0x80 ======
 * DPL=3，用户态可直接调用
 * 传参约定（Linux ABI）:
 *   eax = 系统调用号
 *   ebx = arg1, ecx = arg2, edx = arg3, esi = arg4, edi = arg5
 *   返回值存入 eax
 */
ENTRY(system_call)
    pushl %eax
    pushl %ds
    pushl %es
    pushl %ebp
    pushl %edi
    pushl %esi
    pushl %edx
    pushl %ecx
    pushl %ebx

    cld
    movl $__KERNEL_DS, %eax
    movl %eax, %ds
    movl %eax, %es

    movl %esp, %eax          /* pt_regs * 作为参数 */
    pushl %eax
    call SYMBOL_NAME(do_syscall)
    addl $4, %esp

    movl %eax, 0x20(%esp)    /* 将返回值写入 eax 槽位 */

    popl %ebx
    popl %ecx
    popl %edx
    popl %esi
    popl %edi
    popl %ebp
    popl %eax
    movl %eax, %es
    popl %eax
    movl %eax, %ds
    popl %eax
    iret







