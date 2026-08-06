#define __ASM__ 1
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/page.h>
#include <arch/x86/segment.h>
#include <arch/linkage.h>
#include <arch/x86/thread_info.h>

#define MB_FLAGS    (MULTIBOOT_MEMORY_INFO | MULTIBOOT_PAGE_ALIGN)
/* 内存bitmap ,存放在  内核+PGD+PDE后*/
#define INIT_MAP_BEYOND_END	(128*1024)
#define PAGE_OFFSET_INDEX (__PAGE_OFFSET >> 20) /* page table index */

.section .multiboot
    .long MULTIBOOT_MAGIC
    .long MB_FLAGS
    .long CHECKSUM(MB_FLAGS)

.section .bss
    /* Accoding to   System V ABI, the stack should be 16-byte aligned */
    .align 16
    stack_bottom:
        .skip 0x4000,0  // 16KB
    stack_top:
 
.section .text
    .global start_
    .type start_,@function 
    start_: 

        movl $(stack_top-__PAGE_OFFSET) , %esp

        /*拷贝multiboot参数 */ 
        /*cld 
        movl %ebx,%esi
        movl $(multiboot_params-__PAGE_OFFSET),%edi 
        movl 4096,%ecx
        shrl $2,%ecx
        rep movsl   / movsl 将 DS:ESI 指向的源双字数据，复制到 ES:EDI 指向的目标地址 /
        
        movl $(multi_boot_size-__PAGE_OFFSET),%ecx
        andl $3,%ecx
        jz copy_end 
        rep movsb 
        */
        movl $(multiboot_params_addr-__PAGE_OFFSET),%edi
        movl %ebx,(%edi)
 

copy_end:
        /* 初始化页表   */ 
        cld
        movl $(pg0 - __PAGE_OFFSET),%edi 
        movl $(swapper_pg_dir - __PAGE_OFFSET), %edx
        movl $0x007, %eax			
1:
        leal 0x007(%edi),%ecx
        movl %ecx,(%edx)
        movl %ecx,PAGE_OFFSET_INDEX(%edx)
        addl $4,%edx 
        movl $1024, %ecx
        
2:        
        stosl               /*stosl  --  EAX 寄存器的值存储到 ES:EDI 指向的内存地址，并根据方向标志（DF）自动调整 EDI 寄存器的值*/ 
        addl $0x1000,%eax
        loop 2b
        leal (INIT_MAP_BEYOND_END+0x007)(%edi),%ebp
        cmpl %ebp,%eax
        jb 1b   
        movl %edi,(init_pg_tables_end - __PAGE_OFFSET)

        xorl %ebx,%ebx				/* This is the boot CPU (BSP) */
	    jmp 3f

/* ap start up  */
.global startup_ap
startup_ap:
        hlt
        jmp $
3:       
        /* load page table */ 
        movl $(swapper_pg_dir-__PAGE_OFFSET),%eax
        movl %eax,%cr3
        movl %cr0,%eax
        orl $0x80000000,%eax
        movl %eax,%cr0

        movl $page_after,%eax 
        pushl %eax
        ret

page_after:
        
        call _kernel_init
        /* load GDT */  
        lgdt _gdt_pointer
        /* load IDT */
        lidt _idt_pointer   

        movw $__KERNEL_DS,%ax
        movw %ax,%ds
        movw %ax,%es
        movw %ax,%fs
        movw %ax,%gs
        movw %ax,%ss
        movl $stack_start,%esp   

        movl $entry_kernel, %eax
        pushw $__KERNEL_CS                     
	    pushl %eax 
        retf

    entry_kernel:
        /*pushl $multiboot_params   // push multiboot info addr*/ 
        call _kernel_main
        cli
    
    loop_:
        hlt
        jmp loop_ 



.section .data
    /* gdt table */ 
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

    /* idt table -- 265 entry,total 256*8=2048 byte */
    .global _idt
    _idt:
        .fill 256,8,0
    _idt_end:

    _idt_pointer:
    _idt_limit:	.word	_idt_end - _idt - 1
    _idt_base:	.long	_idt


    ENTRY(stack_start)
        .long init_stack_start - 8092

    init_stack_start:
        .fill 8092,1,0

    /* page table dir */
.section .bss.page_table
    .global swapper_pg_dir
    swapper_pg_dir:
        .fill 1024,4,0

    .global empty_zero_page
    empty_zero_page:
        .fill 4096,1,0

.section .bss.mb_params   
    .global temp_multiboot_params
    temp_multiboot_params:
        .fill 4096,1,0


        




