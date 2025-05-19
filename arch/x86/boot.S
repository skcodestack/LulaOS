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
        /*
           1.load gdt
           2.load idt 
           3.enable page 
        */
        call _kernel_init
        pushl %ebx   // push multiboot info addr
        call _kernel_main

        cli
    
    loop_:
        hlt
        jmp loop_




