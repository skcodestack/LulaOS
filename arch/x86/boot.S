#include "multiboot.h"

.section .multiboot
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHKSUM 

.section .bss
    /* Accoding to   System V ABI, the stack should be 16-byte aligned */
    .align 16
    stack_bottom:
        .skip 0x4000,0  // 16KB
    stack_top:


.section .text
    .global start_
    start_:
        movl $stack_top, %esp
       
        call _kernel_init

         
        lgdt _gdt_pointer

        movw $0x10,%ax
        movw %ax,%ds
        movw %ax,%es
        movw %ax,%fs
        movw %ax,%gs
        movw %ax,%ss

        movl $entry_kernel, %eax
        pushw $0x08                     
	    pushl %eax 
        retf

    entry_kernel:
        pushl %ebx   // push multiboot info addr
        call _kernel_main

        cli
    
    loop_:
        hlt
        jmp loop_





.section .data 
    .globl _gdt
    _gdt:
        .quad 0x0000000000000000 /*0 NULL descriptor 00*/
        .quad 0x00cf9a000000ffff /*1 KERNEL Code 32-bit Segment 08*/
        .quad 0x00cf92000000ffff /*2 KERNEL Data 32-bit Segment 10*/
        .quad 0x00cffa000000ffff /*3 USER Code 32-bit Segment 18*/
        .quad 0x00cff2000000ffff /*4 USER Data 32-bit Segment 20*/
        .fill 10,8,0					 
    _gdt_end:

    _gdt_pointer:
    _gdt_limit:	.word	_gdt_end - _gdt - 1  
    _gdt_base:	.long	_gdt


