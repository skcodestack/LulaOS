#include <linkage.h>
#include <arch/segment.h>

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







