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
    /*
     * 中断返回前检查 need_resched
     *
     * 参考 Linux 2.6.20 arch/i386/kernel/entry.S ret_from_intr:
     *   检查 TIF_NEED_RESCHED 标志，若置位则调用 schedule()
     *
     * current 宏通过 esp & ~(THREAD_SIZE-1) 定位 task_struct
     * need_resched 在 task_struct 偏移 16 (0x10)
     */
    movl    %esp, %eax
    andl    $~(8192-1), %eax        /* eax = &task_struct (esp & ~0x1FFF) */
    cmpl    $0, 16(%eax)            /* need_resched 偏移 16 */
    jnz     ret_from_intr_resched
    jmp     RESTORE_ALL
ret_from_intr_resched:
    call    SYMBOL_NAME(schedule)
    jmp     RESTORE_ALL

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

/* 向量常量（与 interrupts.h 保持一致，使用 .equ 避免 C 预处理器干扰） */
.equ VECTOR_TIMER, 0xEF
.equ VECTOR_ERROR, 0xFE

/*
 * BUILD_IRQ n - 为向量 n 生成独立入口桩
 * 压入向量号 n，而非 $0，使 do_IRQ 能识别中断源
 * 使用 GAS .macro 保证每条指令独占一行
 */
.macro BUILD_IRQ n
    .globl irq_entry_\n
    irq_entry_\()\n:
        pushl $\n
        pushl $do_IRQ
        jmp interrupt_warpper
.endm

BUILD_IRQ 0x20
BUILD_IRQ 0x21
BUILD_IRQ 0x22
BUILD_IRQ 0x23
BUILD_IRQ 0x24
BUILD_IRQ 0x25
BUILD_IRQ 0x26
BUILD_IRQ 0x27
BUILD_IRQ 0x28
BUILD_IRQ 0x29
BUILD_IRQ 0x2a
BUILD_IRQ 0x2b
BUILD_IRQ 0x2c
BUILD_IRQ 0x2d
BUILD_IRQ 0x2e
BUILD_IRQ 0x2f
BUILD_IRQ 0x30
BUILD_IRQ 0x31
BUILD_IRQ 0x32
BUILD_IRQ 0x33
BUILD_IRQ 0x34
BUILD_IRQ 0x35
BUILD_IRQ 0x36
BUILD_IRQ 0x37
BUILD_IRQ 0x38
BUILD_IRQ 0x39
BUILD_IRQ 0x3a
BUILD_IRQ 0x3b
BUILD_IRQ 0x3c
BUILD_IRQ 0x3d
BUILD_IRQ 0x3e
BUILD_IRQ 0x3f
BUILD_IRQ 0x40
BUILD_IRQ 0x41
BUILD_IRQ 0x42
BUILD_IRQ 0x43
BUILD_IRQ 0x44
BUILD_IRQ 0x45
BUILD_IRQ 0x46
BUILD_IRQ 0x47
BUILD_IRQ 0x48
BUILD_IRQ 0x49
BUILD_IRQ 0x4a
BUILD_IRQ 0x4b
BUILD_IRQ 0x4c
BUILD_IRQ 0x4d
BUILD_IRQ 0x4e
BUILD_IRQ 0x4f
BUILD_IRQ 0x50
BUILD_IRQ 0x51
BUILD_IRQ 0x52
BUILD_IRQ 0x53
BUILD_IRQ 0x54
BUILD_IRQ 0x55
BUILD_IRQ 0x56
BUILD_IRQ 0x57
BUILD_IRQ 0x58
BUILD_IRQ 0x59
BUILD_IRQ 0x5a
BUILD_IRQ 0x5b
BUILD_IRQ 0x5c
BUILD_IRQ 0x5d
BUILD_IRQ 0x5e
BUILD_IRQ 0x5f
BUILD_IRQ 0x60
BUILD_IRQ 0x61
BUILD_IRQ 0x62
BUILD_IRQ 0x63
BUILD_IRQ 0x64
BUILD_IRQ 0x65
BUILD_IRQ 0x66
BUILD_IRQ 0x67
BUILD_IRQ 0x68
BUILD_IRQ 0x69
BUILD_IRQ 0x6a
BUILD_IRQ 0x6b
BUILD_IRQ 0x6c
BUILD_IRQ 0x6d
BUILD_IRQ 0x6e
BUILD_IRQ 0x6f
BUILD_IRQ 0x70
BUILD_IRQ 0x71
BUILD_IRQ 0x72
BUILD_IRQ 0x73
BUILD_IRQ 0x74
BUILD_IRQ 0x75
BUILD_IRQ 0x76
BUILD_IRQ 0x77
BUILD_IRQ 0x78
BUILD_IRQ 0x79
BUILD_IRQ 0x7a
BUILD_IRQ 0x7b
BUILD_IRQ 0x7c
BUILD_IRQ 0x7d
BUILD_IRQ 0x7e
BUILD_IRQ 0x7f
/* 0x80 is SYSCALL_VECTOR, skip */
BUILD_IRQ 0x81
BUILD_IRQ 0x82
BUILD_IRQ 0x83
BUILD_IRQ 0x84
BUILD_IRQ 0x85
BUILD_IRQ 0x86
BUILD_IRQ 0x87
BUILD_IRQ 0x88
BUILD_IRQ 0x89
BUILD_IRQ 0x8a
BUILD_IRQ 0x8b
BUILD_IRQ 0x8c
BUILD_IRQ 0x8d
BUILD_IRQ 0x8e
BUILD_IRQ 0x8f
BUILD_IRQ 0x90
BUILD_IRQ 0x91
BUILD_IRQ 0x92
BUILD_IRQ 0x93
BUILD_IRQ 0x94
BUILD_IRQ 0x95
BUILD_IRQ 0x96
BUILD_IRQ 0x97
BUILD_IRQ 0x98
BUILD_IRQ 0x99
BUILD_IRQ 0x9a
BUILD_IRQ 0x9b
BUILD_IRQ 0x9c
BUILD_IRQ 0x9d
BUILD_IRQ 0x9e
BUILD_IRQ 0x9f
BUILD_IRQ 0xa0
BUILD_IRQ 0xa1
BUILD_IRQ 0xa2
BUILD_IRQ 0xa3
BUILD_IRQ 0xa4
BUILD_IRQ 0xa5
BUILD_IRQ 0xa6
BUILD_IRQ 0xa7
BUILD_IRQ 0xa8
BUILD_IRQ 0xa9
BUILD_IRQ 0xaa
BUILD_IRQ 0xab
BUILD_IRQ 0xac
BUILD_IRQ 0xad
BUILD_IRQ 0xae
BUILD_IRQ 0xaf
BUILD_IRQ 0xb0
BUILD_IRQ 0xb1
BUILD_IRQ 0xb2
BUILD_IRQ 0xb3
BUILD_IRQ 0xb4
BUILD_IRQ 0xb5
BUILD_IRQ 0xb6
BUILD_IRQ 0xb7
BUILD_IRQ 0xb8
BUILD_IRQ 0xb9
BUILD_IRQ 0xba
BUILD_IRQ 0xbb
BUILD_IRQ 0xbc
BUILD_IRQ 0xbd
BUILD_IRQ 0xbe
BUILD_IRQ 0xbf
BUILD_IRQ 0xc0
BUILD_IRQ 0xc1
BUILD_IRQ 0xc2
BUILD_IRQ 0xc3
BUILD_IRQ 0xc4
BUILD_IRQ 0xc5
BUILD_IRQ 0xc6
BUILD_IRQ 0xc7
BUILD_IRQ 0xc8
BUILD_IRQ 0xc9
BUILD_IRQ 0xca
BUILD_IRQ 0xcb
BUILD_IRQ 0xcc
BUILD_IRQ 0xcd
BUILD_IRQ 0xce
BUILD_IRQ 0xcf
BUILD_IRQ 0xd0
BUILD_IRQ 0xd1
BUILD_IRQ 0xd2
BUILD_IRQ 0xd3
BUILD_IRQ 0xd4
BUILD_IRQ 0xd5
BUILD_IRQ 0xd6
BUILD_IRQ 0xd7
BUILD_IRQ 0xd8
BUILD_IRQ 0xd9
BUILD_IRQ 0xda
BUILD_IRQ 0xdb
BUILD_IRQ 0xdc
BUILD_IRQ 0xdd
BUILD_IRQ 0xde
BUILD_IRQ 0xdf
BUILD_IRQ 0xe0
BUILD_IRQ 0xe1
BUILD_IRQ 0xe2
BUILD_IRQ 0xe3
BUILD_IRQ 0xe4
BUILD_IRQ 0xe5
BUILD_IRQ 0xe6
BUILD_IRQ 0xe7
BUILD_IRQ 0xe8
BUILD_IRQ 0xe9
BUILD_IRQ 0xea
BUILD_IRQ 0xeb
BUILD_IRQ 0xec
BUILD_IRQ 0xed
BUILD_IRQ 0xee
BUILD_IRQ 0xef
BUILD_IRQ 0xf0
BUILD_IRQ 0xf1
BUILD_IRQ 0xf2
BUILD_IRQ 0xf3
BUILD_IRQ 0xf4
BUILD_IRQ 0xf5
BUILD_IRQ 0xf6
BUILD_IRQ 0xf7
BUILD_IRQ 0xf8
BUILD_IRQ 0xf9
BUILD_IRQ 0xfa
BUILD_IRQ 0xfb
BUILD_IRQ 0xfc
BUILD_IRQ 0xfd
BUILD_IRQ 0xfe
BUILD_IRQ 0xff

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


/* ====== 进程上下文切换 ======
 *
 * void __switch_to(struct task_struct *prev, struct task_struct *next)
 *
 * 调用方（schedule）已将 prev / next 依次压栈：
 *   [esp+8]  = prev           （先压）
 *   [esp+4]  = next           （后压）
 *   [esp]    = 返回地址（由 call __switch_to 压入）
 *
 * 上下文保存（prev）：
 *   1. pushfl              保存 eflags
 *   2. pushl %ebp/%ebx/%esi/%edi  保存被调用者保存寄存器
 *   3. movl %esp → prev->thread.esp
 *   4. 将返回地址存入 prev->thread.eip
 *
 * 上下文恢复（next）：
 *   5. movl next->thread.esp → %esp
 *   6. popl %edi/%esi/%ebx/%ebp
 *   7. popfl
 *   8. 加载 next 的页目录（CR3）
 *   9. 若 PF_NEVER_STARTED → jmp ret_from_fork
 *  10. ret → 弹出 next->thread.eip → 跳转到 schedule() 调用点
 *
 * current 宏通过 esp & ~0x1FFF 自动更新，无需显式写入。
 *
 * task_struct 字段偏移（十进制）：
 *   thread.esp  = 56  (0x38)
 *   thread.eip  = 60  (0x3C)
 *   thread.pgd  = 84  (0x54)
 *   flags       = 92  (0x5C)
 */
ENTRY(__switch_to)
    /* 保存 eflags */
    pushfl

    /* 保存 callee-saved 寄存器（GCC cdecl 约定） */
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    /* 保存当前栈指针到 prev->thread.esp
     * prev 参数在 5次 push 前位于 [esp+8]，5次 push 后偏移 +20+8=注意：
     *   push 前：[esp+4]=next, [esp+8]=prev
     *   push fl：+4； push ebp：+4； push ebx：+4； push esi：+4； push edi：+4
     *   共 +20，加上 call 帧的返回地址 +4，共 +24 前 push 前橈位匹配
     *   实际：[esp+0]=edi, [esp+4]=esi, [esp+8]=ebx, [esp+12]=ebp,
     *           [esp+16]=eflags, [esp+20]=ret_addr, [esp+24]=prev, [esp+28]=next
     */
    movl 24(%esp), %eax       /* eax = prev */
    movl %esp, 56(%eax)       /* prev->thread.esp (offset 0x38) */

    /* 保存返回地址到 prev->thread.eip */
    movl 20(%esp), %ecx       /* ecx = ret_addr */
    movl %ecx, 60(%eax)       /* prev->thread.eip (offset 0x3C) */

    /* 恢复 next 的栈指针 */
    movl 28(%esp), %edx       /* edx = next */
    movl 56(%edx), %esp       /* %esp = next->thread.esp */

    /* 恢复 callee-saved 寄存器 */
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    /* 恢复 eflags */
    popfl

    /*
     * current 宏通过 esp & ~0x1FFF 自动更新，无需显式写入。
     * 切换到 next 的栈后，get_current() 將返回 next.
     */

    /* 切换 CR3 页目录（仅当 next->thread.pgd != 0 时） */
    movl 84(%edx), %ecx       /* next->thread.pgd (offset 0x54) */
    testl %ecx, %ecx
    jz 1f
    movl %ecx, %cr3
1:

    /* 若任务从未启动（PF_NEVER_STARTED），跳到 ret_from_fork */
    testl $0x00000001, 92(%edx)  /* next->flags (offset 0x5C) */
    jz 2f
    jmp SYMBOL_NAME(ret_from_fork)
2:

    /* 正常返回：从 next->thread.eip 处继续执行 */
    ret


/* ====== 新进程/线程的首次执行入口 ======
 *
 * __switch_to 检测到 PF_NEVER_STARTED 后跳转到此。
 * 任务的内核栈已由 kernel_thread() / sys_fork() 初始化：
 *   - kernel_thread: eip = kernel_thread_helper, esp 指向 [fn, arg]
 *   - sys_fork:      eip = ret_from_intr, esp 指向 pt_regs
 *
 * 实现：通过 esp & ~0x1FFF 获取 current，
 *   清除 PF_NEVER_STARTED，然后跳转到 thread.eip。
 *
 * task_struct 偏移：
 *   thread.eip  = 60  (0x3C)
 *   flags       = 92  (0x5C)
 */
ENTRY(ret_from_fork)
    /* 通过 esp & ~0x1FFF 直接获取 current task_struct */
    movl    %esp, %eax
    andl    $~(8192-1), %eax      /* eax = current (esp & ~0x1FFF) */

    /* 清除 PF_NEVER_STARTED 标志 */
    andl    $~0x00000001, 92(%eax) /* current->flags (offset 0x5C) */

    /* 跳转到 thread.eip 保存的地址（kernel_thread_helper 或 ret_from_intr） */
    movl    60(%eax), %ecx         /* current->thread.eip (offset 0x3C) */
    jmp     *%ecx


/* ====== SMP AP Trampoline ======
 *
 * BSP 将此代码复制到物理地址 0x1000（SIPI 目标页），
 * 然后发送 INIT-SIPI-SIPI 启动每个 AP。
 *
 * AP 执行路径：
 *   实模式(16-bit) → 保护模式(32-bit) → 开启分页 → 调用 start_secondary()
 *
 * trampoline 页（物理 0x1000）内数据布局：
 *   代码结束后紧跟 GDT（24字节）+ GDTR（6字节），无 .org 填充
 *   0x1000 + 0x300 : AP 内核栈顶（虚拟地址，4 字节，BSP 写入）
 *   0x1000 + 0x304 : start_secondary 入口地址（4 字节，BSP 写入）
 */

#define TRAMP_PAGE_PHYS     0x1000
#define TRAMP_STACK_OFF     0x300
#define TRAMP_ENTRY_OFF     0x304
#define __PAGE_OFFSET_ASM   0xC0000000

.section .trampoline, "ax"

ENTRY(smp_trampoline_start)

    .code16
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* bx = trampoline 页基址（物理 0x1000，即 AP 入口地址） */
    movw $TRAMP_PAGE_PHYS, %bx

    /* 加载临时 GDT（GDTR 在页内偏移由链接器符号决定） */
    lgdt (tramp_gdtr - smp_trampoline_start)(%bx)

    /* 进入保护模式：设置 CR0.PE */
    movl %cr0, %eax
    orb  $1, %al
    movl %eax, %cr0

    /* 远跳转到 32 位代码段
     * .code16 下 ljmp 偏移字段为 16 位，不能直接用节标签（虚拟地址 >0xFFFF）
     * 使用命名标签 tramp_pm32 计算节内偏移，加上物理基址得到正确目标
     * （不能用局部标签 1f，在 ljmp 立即数表达式中解析可能指向错误位置） */
    ljmp $0x08, $(TRAMP_PAGE_PHYS + (tramp_pm32 - smp_trampoline_start))

    .code32
    /* ===== 32 位保护模式（尚未开启分页） ===== */
    .globl tramp_pm32
tramp_pm32:

    /* 【关键】先加载数据段寄存器，切换到 32 位平坦段（base=0, limit=4G）
     * 否则 DS 仍是实模式残留（limit=0xFFFF），后续任何 >0xFFFF 的物理地址访问都会 #GP */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 加载 BSP 的正式 GDT 和 IDT（物理地址 = 虚拟地址 - PAGE_OFFSET） */
    lgdt (_gdt_pointer - __PAGE_OFFSET_ASM)
    lidt (_idt_pointer - __PAGE_OFFSET_ASM)

    /* 开启分页（所有 CPU 共享 swapper_pg_dir） */
    movl $(swapper_pg_dir - __PAGE_OFFSET_ASM), %eax
    movl %eax, %cr3
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    /* 远跳转确保 CS = __KERNEL_CS（使用虚拟地址，分页已开启） */
    ljmp $__KERNEL_CS, $1f
1:
    movw $__KERNEL_DS, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 从 trampoline 页读取 AP 内核栈顶（虚拟地址，由 BSP 写入）
     * 分页已开启，必须使用内核虚拟地址（phys + PAGE_OFFSET），否则访问物理地址会触发页错误 */
    movl (__PAGE_OFFSET_ASM + TRAMP_PAGE_PHYS + TRAMP_STACK_OFF), %esp

    /* 调用 AP C 入口 start_secondary() */
    call SYMBOL_NAME(start_secondary)

    /* 不应返回 */
2:  hlt
    jmp 2b

/* ===== Trampoline 数据区 =====
 * GDT/GDTR 紧跟代码之后，不使用 .org 填充。
 * smp_init() 通过链接器符号 _trampoline_gdt/_trampoline_gdtr 定位其偏移。 */

/* 临时 GDT（3 个描述符，24 字节） */
    .globl tramp_gdt
tramp_gdt:
    .quad 0x0000000000000000   /* [0] null */
    .quad 0x00cf9a000000ffff   /* [1] KCS: kernel code, base=0, limit=4G */
    .quad 0x00cf92000000ffff   /* [2] KDS: kernel data, base=0, limit=4G */

/* GDTR 指针（6 字节：limit + base）
 * base 字段由 smp_init() 运行时用正确物理地址覆盖 */
    .globl tramp_gdtr
tramp_gdtr:
    .word (3 * 8 - 1)                                        /* GDT limit */
    .long (TRAMP_PAGE_PHYS + (tramp_gdt - smp_trampoline_start))  /* GDT base（物理地址） */

    .code32
ENTRY(smp_trampoline_end)


