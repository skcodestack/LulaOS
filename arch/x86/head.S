

; .section .text
;     .global k_start_
;     .type k_start_,@function 
;     k_start_: 
        

;         call _kernel_init
;         /* load GDT */  
;         lgdt _gdt_pointer

;         /* load IDT */
;         lidt _idt_pointer

;         movw $0x10,%ax
;         movw %ax,%ds
;         movw %ax,%es
;         movw %ax,%fs
;         movw %ax,%gs
;         movw %ax,%ss

;         movl $entry_kernel, %eax
;         pushw $0x08                     
; 	    pushl %eax 
;         retf

;     entry_kernel:
;         pushl %ebx   // push multiboot info addr 
;         call _kernel_main
;         cli
    
;     loop_:
;         hlt
;         jmp loop_





; .section .data
;     /* gdt table */ 
;     .globl _gdt
;     _gdt:
;         .quad 0x0000000000000000 /*0 NULL descriptor 00*/
;         .quad 0x00cf9a000000ffff /*1 KERNEL Code 32-bit Segment 08*/
;         .quad 0x00cf92000000ffff /*2 KERNEL Data 32-bit Segment 10*/
;         .quad 0x00cffa000000ffff /*3 USER Code 32-bit Segment 18*/
;         .quad 0x00cff2000000ffff /*4 USER Data 32-bit Segment 20*/
;         .fill 10,8,0					 
;     _gdt_end:

;     _gdt_pointer:
;     _gdt_limit:	.word	_gdt_end - _gdt - 1  
;     _gdt_base:	.long	_gdt

;     /* idt table -- 265 entry,total 256*8=2048 byte */
;     .global _idt
;     _idt:
;         .fill 256,8,0
;     _idt_end:

;     _idt_pointer:
;     _idt_limit:	.word	_idt_end - _idt - 1
;     _idt_base:	.long	_idt

;     /* page table dir */
;     section .bss.page_table
;     .global swapper_pg_dir
;     swapper_pg_dir:
;         .fill 1024,4,0

;     .global empty_zero_page
;     empty_zero_page:
;         .fill 4096,1,0


