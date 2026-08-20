#include <arch/x86/idt.h>
#include <arch/linkage.h>
#include <arch/x86/ptrace.h>
#include <tty/tty.h>
#include <arch/x86/segment.h>
#include <printk.h>
#include <stddef.h>
#include <arch/x86/apic.h>
#include <interrupts/interrupts.h>
 

// 30 system interrupt
asmlinkage void divide_error(void);
asmlinkage void debug(void);
asmlinkage void nmi(void);
asmlinkage void int3(void);
asmlinkage void overflow(void);
asmlinkage void bounds(void);
asmlinkage void invalid_op(void);
asmlinkage void device_not_available(void);
asmlinkage void double_fault(void);
asmlinkage void coprocessor_segment_overrun(void);
asmlinkage void invalid_TSS(void);
asmlinkage void segment_not_present(void);
asmlinkage void stack_segment(void);
asmlinkage void general_protection(void);
asmlinkage void page_fault(void);
asmlinkage void coprocessor_error(void);
asmlinkage void alignment_check(void);
asmlinkage void machine_check(void);
asmlinkage void simd_coprocessor_error(void);
asmlinkage void virtualization_exception(void);

asmlinkage void apic_timer_entry(void);
asmlinkage void apic_error_entry(void);
asmlinkage void system_call(void);
/* 硬件中断入口桩（entry.S 中 BUILD_IRQ 宏生成）*/
asmlinkage void irq_entry_0x20(void); asmlinkage void irq_entry_0x21(void); asmlinkage void irq_entry_0x22(void); asmlinkage void irq_entry_0x23(void); asmlinkage void irq_entry_0x24(void); asmlinkage void irq_entry_0x25(void); asmlinkage void irq_entry_0x26(void); asmlinkage void irq_entry_0x27(void);
asmlinkage void irq_entry_0x28(void); asmlinkage void irq_entry_0x29(void); asmlinkage void irq_entry_0x2a(void); asmlinkage void irq_entry_0x2b(void); asmlinkage void irq_entry_0x2c(void); asmlinkage void irq_entry_0x2d(void); asmlinkage void irq_entry_0x2e(void); asmlinkage void irq_entry_0x2f(void);
asmlinkage void irq_entry_0x30(void); asmlinkage void irq_entry_0x31(void); asmlinkage void irq_entry_0x32(void); asmlinkage void irq_entry_0x33(void); asmlinkage void irq_entry_0x34(void); asmlinkage void irq_entry_0x35(void); asmlinkage void irq_entry_0x36(void); asmlinkage void irq_entry_0x37(void);
asmlinkage void irq_entry_0x38(void); asmlinkage void irq_entry_0x39(void); asmlinkage void irq_entry_0x3a(void); asmlinkage void irq_entry_0x3b(void); asmlinkage void irq_entry_0x3c(void); asmlinkage void irq_entry_0x3d(void); asmlinkage void irq_entry_0x3e(void); asmlinkage void irq_entry_0x3f(void);
asmlinkage void irq_entry_0x40(void); asmlinkage void irq_entry_0x41(void); asmlinkage void irq_entry_0x42(void); asmlinkage void irq_entry_0x43(void); asmlinkage void irq_entry_0x44(void); asmlinkage void irq_entry_0x45(void); asmlinkage void irq_entry_0x46(void); asmlinkage void irq_entry_0x47(void);
asmlinkage void irq_entry_0x48(void); asmlinkage void irq_entry_0x49(void); asmlinkage void irq_entry_0x4a(void); asmlinkage void irq_entry_0x4b(void); asmlinkage void irq_entry_0x4c(void); asmlinkage void irq_entry_0x4d(void); asmlinkage void irq_entry_0x4e(void); asmlinkage void irq_entry_0x4f(void);
asmlinkage void irq_entry_0x50(void); asmlinkage void irq_entry_0x51(void); asmlinkage void irq_entry_0x52(void); asmlinkage void irq_entry_0x53(void); asmlinkage void irq_entry_0x54(void); asmlinkage void irq_entry_0x55(void); asmlinkage void irq_entry_0x56(void); asmlinkage void irq_entry_0x57(void);
asmlinkage void irq_entry_0x58(void); asmlinkage void irq_entry_0x59(void); asmlinkage void irq_entry_0x5a(void); asmlinkage void irq_entry_0x5b(void); asmlinkage void irq_entry_0x5c(void); asmlinkage void irq_entry_0x5d(void); asmlinkage void irq_entry_0x5e(void); asmlinkage void irq_entry_0x5f(void);
asmlinkage void irq_entry_0x60(void); asmlinkage void irq_entry_0x61(void); asmlinkage void irq_entry_0x62(void); asmlinkage void irq_entry_0x63(void); asmlinkage void irq_entry_0x64(void); asmlinkage void irq_entry_0x65(void); asmlinkage void irq_entry_0x66(void); asmlinkage void irq_entry_0x67(void);
asmlinkage void irq_entry_0x68(void); asmlinkage void irq_entry_0x69(void); asmlinkage void irq_entry_0x6a(void); asmlinkage void irq_entry_0x6b(void); asmlinkage void irq_entry_0x6c(void); asmlinkage void irq_entry_0x6d(void); asmlinkage void irq_entry_0x6e(void); asmlinkage void irq_entry_0x6f(void);
asmlinkage void irq_entry_0x70(void); asmlinkage void irq_entry_0x71(void); asmlinkage void irq_entry_0x72(void); asmlinkage void irq_entry_0x73(void); asmlinkage void irq_entry_0x74(void); asmlinkage void irq_entry_0x75(void); asmlinkage void irq_entry_0x76(void); asmlinkage void irq_entry_0x77(void);
asmlinkage void irq_entry_0x78(void); asmlinkage void irq_entry_0x79(void); asmlinkage void irq_entry_0x7a(void); asmlinkage void irq_entry_0x7b(void); asmlinkage void irq_entry_0x7c(void); asmlinkage void irq_entry_0x7d(void); asmlinkage void irq_entry_0x7e(void); asmlinkage void irq_entry_0x7f(void);
/* 0x80 is SYSCALL_VECTOR */
asmlinkage void irq_entry_0x81(void); asmlinkage void irq_entry_0x82(void); asmlinkage void irq_entry_0x83(void); asmlinkage void irq_entry_0x84(void); asmlinkage void irq_entry_0x85(void); asmlinkage void irq_entry_0x86(void); asmlinkage void irq_entry_0x87(void);
asmlinkage void irq_entry_0x88(void); asmlinkage void irq_entry_0x89(void); asmlinkage void irq_entry_0x8a(void); asmlinkage void irq_entry_0x8b(void); asmlinkage void irq_entry_0x8c(void); asmlinkage void irq_entry_0x8d(void); asmlinkage void irq_entry_0x8e(void); asmlinkage void irq_entry_0x8f(void);
asmlinkage void irq_entry_0x90(void); asmlinkage void irq_entry_0x91(void); asmlinkage void irq_entry_0x92(void); asmlinkage void irq_entry_0x93(void); asmlinkage void irq_entry_0x94(void); asmlinkage void irq_entry_0x95(void); asmlinkage void irq_entry_0x96(void); asmlinkage void irq_entry_0x97(void);
asmlinkage void irq_entry_0x98(void); asmlinkage void irq_entry_0x99(void); asmlinkage void irq_entry_0x9a(void); asmlinkage void irq_entry_0x9b(void); asmlinkage void irq_entry_0x9c(void); asmlinkage void irq_entry_0x9d(void); asmlinkage void irq_entry_0x9e(void); asmlinkage void irq_entry_0x9f(void);
asmlinkage void irq_entry_0xa0(void); asmlinkage void irq_entry_0xa1(void); asmlinkage void irq_entry_0xa2(void); asmlinkage void irq_entry_0xa3(void); asmlinkage void irq_entry_0xa4(void); asmlinkage void irq_entry_0xa5(void); asmlinkage void irq_entry_0xa6(void); asmlinkage void irq_entry_0xa7(void);
asmlinkage void irq_entry_0xa8(void); asmlinkage void irq_entry_0xa9(void); asmlinkage void irq_entry_0xaa(void); asmlinkage void irq_entry_0xab(void); asmlinkage void irq_entry_0xac(void); asmlinkage void irq_entry_0xad(void); asmlinkage void irq_entry_0xae(void); asmlinkage void irq_entry_0xaf(void);
asmlinkage void irq_entry_0xb0(void); asmlinkage void irq_entry_0xb1(void); asmlinkage void irq_entry_0xb2(void); asmlinkage void irq_entry_0xb3(void); asmlinkage void irq_entry_0xb4(void); asmlinkage void irq_entry_0xb5(void); asmlinkage void irq_entry_0xb6(void); asmlinkage void irq_entry_0xb7(void);
asmlinkage void irq_entry_0xb8(void); asmlinkage void irq_entry_0xb9(void); asmlinkage void irq_entry_0xba(void); asmlinkage void irq_entry_0xbb(void); asmlinkage void irq_entry_0xbc(void); asmlinkage void irq_entry_0xbd(void); asmlinkage void irq_entry_0xbe(void); asmlinkage void irq_entry_0xbf(void);
asmlinkage void irq_entry_0xc0(void); asmlinkage void irq_entry_0xc1(void); asmlinkage void irq_entry_0xc2(void); asmlinkage void irq_entry_0xc3(void); asmlinkage void irq_entry_0xc4(void); asmlinkage void irq_entry_0xc5(void); asmlinkage void irq_entry_0xc6(void); asmlinkage void irq_entry_0xc7(void);
asmlinkage void irq_entry_0xc8(void); asmlinkage void irq_entry_0xc9(void); asmlinkage void irq_entry_0xca(void); asmlinkage void irq_entry_0xcb(void); asmlinkage void irq_entry_0xcc(void); asmlinkage void irq_entry_0xcd(void); asmlinkage void irq_entry_0xce(void); asmlinkage void irq_entry_0xcf(void);
asmlinkage void irq_entry_0xd0(void); asmlinkage void irq_entry_0xd1(void); asmlinkage void irq_entry_0xd2(void); asmlinkage void irq_entry_0xd3(void); asmlinkage void irq_entry_0xd4(void); asmlinkage void irq_entry_0xd5(void); asmlinkage void irq_entry_0xd6(void); asmlinkage void irq_entry_0xd7(void);
asmlinkage void irq_entry_0xd8(void); asmlinkage void irq_entry_0xd9(void); asmlinkage void irq_entry_0xda(void); asmlinkage void irq_entry_0xdb(void); asmlinkage void irq_entry_0xdc(void); asmlinkage void irq_entry_0xdd(void); asmlinkage void irq_entry_0xde(void); asmlinkage void irq_entry_0xdf(void);
asmlinkage void irq_entry_0xe0(void); asmlinkage void irq_entry_0xe1(void); asmlinkage void irq_entry_0xe2(void); asmlinkage void irq_entry_0xe3(void); asmlinkage void irq_entry_0xe4(void); asmlinkage void irq_entry_0xe5(void); asmlinkage void irq_entry_0xe6(void); asmlinkage void irq_entry_0xe7(void);
asmlinkage void irq_entry_0xe8(void); asmlinkage void irq_entry_0xe9(void); asmlinkage void irq_entry_0xea(void); asmlinkage void irq_entry_0xeb(void); asmlinkage void irq_entry_0xec(void); asmlinkage void irq_entry_0xed(void); asmlinkage void irq_entry_0xee(void); asmlinkage void irq_entry_0xef(void);
asmlinkage void irq_entry_0xf0(void); asmlinkage void irq_entry_0xf1(void); asmlinkage void irq_entry_0xf2(void); asmlinkage void irq_entry_0xf3(void); asmlinkage void irq_entry_0xf4(void); asmlinkage void irq_entry_0xf5(void); asmlinkage void irq_entry_0xf6(void); asmlinkage void irq_entry_0xf7(void);
asmlinkage void irq_entry_0xf8(void); asmlinkage void irq_entry_0xf9(void); asmlinkage void irq_entry_0xfa(void); asmlinkage void irq_entry_0xfb(void); asmlinkage void irq_entry_0xfc(void); asmlinkage void irq_entry_0xfd(void); asmlinkage void irq_entry_0xfe(void); asmlinkage void irq_entry_0xff(void);

/* 向量号 -> 入口桩地址映射表（0x20~0xFF, 0x80 置 NULL）*/
static void *interrupt[NR_IRQS] = {
    &irq_entry_0x20, &irq_entry_0x21, &irq_entry_0x22, &irq_entry_0x23, &irq_entry_0x24, &irq_entry_0x25, &irq_entry_0x26, &irq_entry_0x27,
    &irq_entry_0x28, &irq_entry_0x29, &irq_entry_0x2a, &irq_entry_0x2b, &irq_entry_0x2c, &irq_entry_0x2d, &irq_entry_0x2e, &irq_entry_0x2f,
    &irq_entry_0x30, &irq_entry_0x31, &irq_entry_0x32, &irq_entry_0x33, &irq_entry_0x34, &irq_entry_0x35, &irq_entry_0x36, &irq_entry_0x37,
    &irq_entry_0x38, &irq_entry_0x39, &irq_entry_0x3a, &irq_entry_0x3b, &irq_entry_0x3c, &irq_entry_0x3d, &irq_entry_0x3e, &irq_entry_0x3f,
    &irq_entry_0x40, &irq_entry_0x41, &irq_entry_0x42, &irq_entry_0x43, &irq_entry_0x44, &irq_entry_0x45, &irq_entry_0x46, &irq_entry_0x47,
    &irq_entry_0x48, &irq_entry_0x49, &irq_entry_0x4a, &irq_entry_0x4b, &irq_entry_0x4c, &irq_entry_0x4d, &irq_entry_0x4e, &irq_entry_0x4f,
    &irq_entry_0x50, &irq_entry_0x51, &irq_entry_0x52, &irq_entry_0x53, &irq_entry_0x54, &irq_entry_0x55, &irq_entry_0x56, &irq_entry_0x57,
    &irq_entry_0x58, &irq_entry_0x59, &irq_entry_0x5a, &irq_entry_0x5b, &irq_entry_0x5c, &irq_entry_0x5d, &irq_entry_0x5e, &irq_entry_0x5f,
    &irq_entry_0x60, &irq_entry_0x61, &irq_entry_0x62, &irq_entry_0x63, &irq_entry_0x64, &irq_entry_0x65, &irq_entry_0x66, &irq_entry_0x67,
    &irq_entry_0x68, &irq_entry_0x69, &irq_entry_0x6a, &irq_entry_0x6b, &irq_entry_0x6c, &irq_entry_0x6d, &irq_entry_0x6e, &irq_entry_0x6f,
    &irq_entry_0x70, &irq_entry_0x71, &irq_entry_0x72, &irq_entry_0x73, &irq_entry_0x74, &irq_entry_0x75, &irq_entry_0x76, &irq_entry_0x77,
    &irq_entry_0x78, &irq_entry_0x79, &irq_entry_0x7a, &irq_entry_0x7b, &irq_entry_0x7c, &irq_entry_0x7d, &irq_entry_0x7e, &irq_entry_0x7f,
    NULL, /* 0x80 SYSCALL_VECTOR */
    &irq_entry_0x81, &irq_entry_0x82, &irq_entry_0x83, &irq_entry_0x84, &irq_entry_0x85, &irq_entry_0x86, &irq_entry_0x87,
    &irq_entry_0x88, &irq_entry_0x89, &irq_entry_0x8a, &irq_entry_0x8b, &irq_entry_0x8c, &irq_entry_0x8d, &irq_entry_0x8e, &irq_entry_0x8f,
    &irq_entry_0x90, &irq_entry_0x91, &irq_entry_0x92, &irq_entry_0x93, &irq_entry_0x94, &irq_entry_0x95, &irq_entry_0x96, &irq_entry_0x97,
    &irq_entry_0x98, &irq_entry_0x99, &irq_entry_0x9a, &irq_entry_0x9b, &irq_entry_0x9c, &irq_entry_0x9d, &irq_entry_0x9e, &irq_entry_0x9f,
    &irq_entry_0xa0, &irq_entry_0xa1, &irq_entry_0xa2, &irq_entry_0xa3, &irq_entry_0xa4, &irq_entry_0xa5, &irq_entry_0xa6, &irq_entry_0xa7,
    &irq_entry_0xa8, &irq_entry_0xa9, &irq_entry_0xaa, &irq_entry_0xab, &irq_entry_0xac, &irq_entry_0xad, &irq_entry_0xae, &irq_entry_0xaf,
    &irq_entry_0xb0, &irq_entry_0xb1, &irq_entry_0xb2, &irq_entry_0xb3, &irq_entry_0xb4, &irq_entry_0xb5, &irq_entry_0xb6, &irq_entry_0xb7,
    &irq_entry_0xb8, &irq_entry_0xb9, &irq_entry_0xba, &irq_entry_0xbb, &irq_entry_0xbc, &irq_entry_0xbd, &irq_entry_0xbe, &irq_entry_0xbf,
    &irq_entry_0xc0, &irq_entry_0xc1, &irq_entry_0xc2, &irq_entry_0xc3, &irq_entry_0xc4, &irq_entry_0xc5, &irq_entry_0xc6, &irq_entry_0xc7,
    &irq_entry_0xc8, &irq_entry_0xc9, &irq_entry_0xca, &irq_entry_0xcb, &irq_entry_0xcc, &irq_entry_0xcd, &irq_entry_0xce, &irq_entry_0xcf,
    &irq_entry_0xd0, &irq_entry_0xd1, &irq_entry_0xd2, &irq_entry_0xd3, &irq_entry_0xd4, &irq_entry_0xd5, &irq_entry_0xd6, &irq_entry_0xd7,
    &irq_entry_0xd8, &irq_entry_0xd9, &irq_entry_0xda, &irq_entry_0xdb, &irq_entry_0xdc, &irq_entry_0xdd, &irq_entry_0xde, &irq_entry_0xdf,
    &irq_entry_0xe0, &irq_entry_0xe1, &irq_entry_0xe2, &irq_entry_0xe3, &irq_entry_0xe4, &irq_entry_0xe5, &irq_entry_0xe6, &irq_entry_0xe7,
    &irq_entry_0xe8, &irq_entry_0xe9, &irq_entry_0xea, &irq_entry_0xeb, &irq_entry_0xec, &irq_entry_0xed, &irq_entry_0xee, &irq_entry_0xef,
    &irq_entry_0xf0, &irq_entry_0xf1, &irq_entry_0xf2, &irq_entry_0xf3, &irq_entry_0xf4, &irq_entry_0xf5, &irq_entry_0xf6, &irq_entry_0xf7,
    &irq_entry_0xf8, &irq_entry_0xf9, &irq_entry_0xfa, &irq_entry_0xfb, &irq_entry_0xfc, &irq_entry_0xfd, &irq_entry_0xfe, &irq_entry_0xff,
};

void show_stack(unsigned long *esp)
{
    unsigned long *stack;
    int i;

    if (esp == NULL)
        esp = (unsigned long *)&esp;

    stack = esp;
    // for(i=0; i < 24; i++) {
    // 	if (((long) stack & (THREAD_SIZE-1)) == 0)
    // 		break;
    // 	if (i && ((i % 8) == 0))
    // 		printk("\n       ");
    // 	printk("%08lx ", *stack++);
    // }
    printk("\n");
}

void show_registers(struct pt_regs *regs)
{
    int i;
    int in_kernel = 1;
    unsigned long esp;
    unsigned short ss;

    esp = (unsigned long)(&regs->esp);
    ss = __KERNEL_DS;
    if (regs->cs & 3)
    {
        in_kernel = 0;
        esp = regs->esp;
        ss = regs->ss & 0xffff;
    }
    // printk("CPU:    %d\nEIP:    %04x:[<%08lx>]    %s\nEFLAGS: %08lx\n",
    //        smp_processor_id(), 0xffff & regs->cs, regs->eip, "", regs->eflags);
    // printk("eax: %08lx   ebx: %08lx   ecx: %08lx   edx: %08lx\n",
    //        regs->eax, regs->ebx, regs->ecx, regs->edx);
    // printk("esi: %08lx   edi: %08lx   ebp: %08lx   esp: %08lx\n",
    //        regs->esi, regs->edi, regs->ebp, esp);
    // printk("ds: %04x   es: %04x   ss: %04x\n",
    //        regs->ds & 0xffff, regs->es & 0xffff, ss);
    // printk("Process %s (pid: %d, stackpage=%08lx)",
    // 	current->comm, current->pid, 4096+(unsigned long)current);
    /*
     * When in-kernel, we also print out the stack and code at the
     * time of the fault..
     */
    if (in_kernel)
    {

        printk("\nStack: ");
        // show_stack((unsigned long *)esp);

        printk("\nCode: ");
        // if(regs->eip < PAGE_OFFSET)
        // 	goto bad;

        // 		for(i=0;i<20;i++)
        // 		{
        // 			unsigned char c;
        // 			if(__get_user(c, &((unsigned char*)regs->eip)[i])) {
        // bad:
        // 				printk(" Bad EIP value.");
        // 				break;
        // 			}
        // 			printk("%02x ", c);
        // 		}
    }
    printk("\n");
}

// default process exception handler
static void inline do_trap(int trapnr, char *str, struct pt_regs *regs, long error_code)
{
    /*
     * Fault 类型异常：regs->eip 指向故障指令本身
     * 读取故障地址处的指令字节，方便定位问题
     */
    unsigned char *ip = (unsigned char *)regs->eip;
    printk("TRAP %d: %s  error_code=%ld\n", trapnr, str, error_code);
    printk("  EIP: %08lx   CS: %04lx   EFLAGS: %08lx\n",
           regs->eip, regs->cs & 0xffff, regs->eflags);
    printk("  ESP: %08lx   SS: %08lx\n", regs->esp, regs->ss);
    printk("  EAX: %08lx  EBX: %08lx  ECX: %08lx  EDX: %08lx\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printk("  ESI: %08lx  EDI: %08lx  EBP: %08lx\n",
           regs->esi, regs->edi, regs->ebp);
    /* 打印故障地址处前后各 4 字节的指令字节码，便于反汇编定位 */
    printk("  Code: ");
    for (int i = -4; i < 8; i++)
        printk("%02x ", ip[i]);
    printk("\n");

    if (regs->cs & 3) {
        /* 用户态异常：暂不处理，直接返回（用户进程会被 iret 重试） */
        return;
    }

    /* 内核态异常：打印调用栈上下文后停机，防止 iret 回到故障指令造成无限循环 */
    printk("  *** Kernel trap - halting CPU ***\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

#define DO_HANDLE_ERROR_INFO(trapnr, str, name)                      \
    asmlinkage void do_##name(struct pt_regs *regs, long error_code) \
    {                                                                \
        do_trap(trapnr, str, regs, error_code);                      \
    }

DO_HANDLE_ERROR_INFO(0, "divide error", divide_error);
DO_HANDLE_ERROR_INFO(1, "debug", debug);
DO_HANDLE_ERROR_INFO(2, "nmi", nmi);
DO_HANDLE_ERROR_INFO(3, "int3", int3);
DO_HANDLE_ERROR_INFO(4, "overflow", overflow);
DO_HANDLE_ERROR_INFO(5, "bounds", bounds);
DO_HANDLE_ERROR_INFO(6, "invalid_op", invalid_op);
DO_HANDLE_ERROR_INFO(7, "device_not_available", device_not_available);
DO_HANDLE_ERROR_INFO(8, "double_fault", double_fault);
DO_HANDLE_ERROR_INFO(9, "coprocessor_segment_overrun", coprocessor_segment_overrun);
// DO_HANDLE_ERROR_INFO(10, "invalid_TSS", invalid_TSS);
DO_HANDLE_ERROR_INFO(11, "segment_not_present", segment_not_present);
DO_HANDLE_ERROR_INFO(12, "stack_segment", stack_segment);
DO_HANDLE_ERROR_INFO(13, "general_protection", general_protection);
DO_HANDLE_ERROR_INFO(16, "coprocessor_error", coprocessor_error);
DO_HANDLE_ERROR_INFO(17, "alignment_check", alignment_check);
DO_HANDLE_ERROR_INFO(18, "machine_check", machine_check);
DO_HANDLE_ERROR_INFO(19, "simd_coprocessor_error", simd_coprocessor_error);
DO_HANDLE_ERROR_INFO(20, "virtualization_exception", virtualization_exception);

asmlinkage void do_invalid_TSS(struct pt_regs *regs, long error_code)
{
    unsigned long *p = NULL;
    p = (unsigned long *)(regs->esp + 0x98);
    printk("do_invalid_TSS(10),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n", error_code, regs->esp, *p);

    if (error_code & 0x01)
        printk("The exception occurred during delivery of an event external to the program,such as an interrupt or an earlier exception.\n");

    if (error_code & 0x02)
        printk("Refers to a gate descriptor in the IDT;\n");
    else
        printk("Refers to a descriptor in the GDT or the current LDT;\n");

    if ((error_code & 0x02) == 0)
        if (error_code & 0x04)
            printk("Refers to a segment or gate descriptor in the LDT;\n");
        else
            printk("Refers to a descriptor in the current GDT;\n");

    printk("Segment Selector Index:%#010x\n", error_code & 0xfff8);
}

void _set_idt_entry(uint8_t index, uint16_t selector, void* func, uint16_t type_attr)
{
    uint32_t offset = (uint32_t) func;
    uint64_t desciptor = type_attr | IDT_OFFSET_H(offset);
    desciptor <<= 32;
    desciptor |= IDT_OFFSET_L(offset) | IDT_SEGMENT_SELECTOR(selector);
    _idt[index] = desciptor;
}

void _set_task_gate_entry(uint8_t index)
{
    _set_idt_entry(index, __KERNEL_CS, 0, IDT_TYPE_ATTR_TASK_GATE);
}

void _set_interrupt_gate_entry(uint8_t index, void* func)
{
    _set_idt_entry(index, __KERNEL_CS, func, IDT_TYPE_ATTR_INTERRUPT_GATE_32BIT);
}

void _set_trap_gate_entry(uint8_t index, void* func)
{
    _set_idt_entry(index, __KERNEL_CS, func, IDT_TYPE_ATTR_TRAP_GATE_32BIT);
}
void _set_system_gate_entry(uint8_t index, void* func)
{
    _set_idt_entry(index, __KERNEL_CS, func, IDT_TYPE_ATTR_SYSTEM_GATE_32BIT);
}

void _init_idt()
{
    // 30 system interrupt
    _set_trap_gate_entry(0, &divide_error);
    _set_trap_gate_entry(1, &debug);
    _set_interrupt_gate_entry(2, &nmi);
    _set_system_gate_entry(3, &int3);
    _set_system_gate_entry(4, &overflow);
    _set_system_gate_entry(5, &bounds);
    _set_interrupt_gate_entry(6, &invalid_op);   /* 中断门：自动清零 IF，防止 trap handler 中被嵌套中断 */
    _set_trap_gate_entry(7, &device_not_available);
    _set_trap_gate_entry(8, &double_fault);
    _set_trap_gate_entry(9, &coprocessor_segment_overrun);
    _set_trap_gate_entry(10, &invalid_TSS);
    _set_trap_gate_entry(11, &segment_not_present);
    _set_trap_gate_entry(12, &stack_segment);
    _set_trap_gate_entry(13, &general_protection);
    _set_trap_gate_entry(14, &page_fault);
    // 15 Intel reserved. Do not use.
    _set_trap_gate_entry(16, &coprocessor_error);
    _set_trap_gate_entry(17, &alignment_check);
    _set_trap_gate_entry(18, &machine_check);
    _set_trap_gate_entry(19, &simd_coprocessor_error);
    // _set_trap_gate_entry(20, &virtualization_exception);

    // 覆盖向量空间：0x20 ~ 0x3f，设备 IRQ 向量
    for (int i = 0; i < NR_IRQS; i++) {
        int vector = FIRST_EXTERNAL_VECTOR + i;
        // if(vector > FIRST_SYSTEM_VECTOR){
        //     break;
        // }
        if (vector == SYSCALL_VECTOR)  // 跳过 0x80
            continue;
        _set_interrupt_gate_entry(vector, interrupt[i]);
    }
    printk("init_idt");

    // APIC Timer 硬件中断门
    _set_interrupt_gate_entry(TIMER_APIC_VECTOR, &apic_timer_entry);

    // // APIC Error 硬件中断门 
    _set_interrupt_gate_entry(ERROR_APIC_VECTOR, &apic_error_entry); 
     

    // system call：系统门（DPL=3，用户态可调用）
    _set_system_gate_entry(SYSCALL_VECTOR, &system_call);
    
}
