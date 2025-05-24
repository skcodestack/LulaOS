#include <arch/idt.h>
#include <linkage.h>
#include <arch/ptrace.h>
#include <tty/tty.h>
#include <arch/segment.h>
#include <printk.h>
#include <stddef.h>

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
    printk("CPU:    %d\nEIP:    %04x:[<%08lx>]    %s\nEFLAGS: %08lx\n",
           smp_processor_id(), 0xffff & regs->cs, regs->eip, print_tainted(), regs->eflags);
    printk("eax: %08lx   ebx: %08lx   ecx: %08lx   edx: %08lx\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printk("esi: %08lx   edi: %08lx   ebp: %08lx   esp: %08lx\n",
           regs->esi, regs->edi, regs->ebp, esp);
    printk("ds: %04x   es: %04x   ss: %04x\n",
           regs->ds & 0xffff, regs->es & 0xffff, ss);
    // printk("Process %s (pid: %d, stackpage=%08lx)",
    // 	current->comm, current->pid, 4096+(unsigned long)current);
    /*
     * When in-kernel, we also print out the stack and code at the
     * time of the fault..
     */
    if (in_kernel)
    {

        printk("\nStack: ");
        show_stack((unsigned long *)esp);

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
    unsigned long *p = NULL;
    p = (unsigned long *)(regs->esp + 0x98);
    printk("trap:%d %s,ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n", trapnr, str, error_code, regs->esp, *p);

    if (!(regs->cs & 3))
        goto kernel_trap;

kernel_trap:
    show_registers(regs);
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

void _set_idt_entry(uint8_t index, uint16_t selector, uint32_t offset, uint16_t type_attr)
{
    uint64_t desciptor = type_attr | IDT_OFFSET_H(offset);
    desciptor <<= 32;
    desciptor |= IDT_OFFSET_L(offset) | IDT_SEGMENT_SELECTOR(selector);
    _idt[index] = desciptor;
}

void _set_task_gate_entry(uint8_t index, uint16_t selector)
{
    _set_idt_entry(index, selector, 0, IDT_TYPE_ATTR_TASK_GATE);
}

void _set_interrupt_gate_entry(uint8_t index, uint16_t selector, uint32_t offset)
{
    _set_idt_entry(index, selector, offset, IDT_TYPE_ATTR_INTERRUPT_GATE_32BIT);
}

void _set_trap_gate_entry(uint8_t index, uint16_t selector, uint32_t offset)
{
    _set_idt_entry(index, selector, offset, IDT_TYPE_ATTR_TRAP_GATE_32BIT);
}
void _set_system_gate_entry(uint8_t index, uint16_t selector, uint32_t offset)
{
    _set_idt_entry(index, selector, offset, IDT_TYPE_ATTR_SYSTEM_GATE_32BIT);
}

void _init_idt()
{
    // 30 system interrupt
    _set_trap_gate_entry(0, &divide_error);
    _set_trap_gate_entry(1, &debug);
    set_intr_gate(2, &nmi);
    _set_system_gate_entry(3, &int3);
    _set_system_gate_entry(4, &overflow);
    _set_system_gate_entry(5, &bounds);
    _set_trap_gate_entry(6, &invalid_op);
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
    _set_trap_gate_entry(20, &virtualization_exception);

    // system call
}
