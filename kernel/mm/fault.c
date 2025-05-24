#include <linkage.h>
#include <arch/ptrace.h>
#include <stddef.h>
#include <printk.h>

asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
    unsigned long * p = NULL;
	unsigned long cr2 = 0;
	//cr2 保存 发生缺页异常错误的线性地址
	__asm__	__volatile__("movl	%%cr2,	%0":"=r"(cr2)::"memory");

	p = (unsigned long *)(regs->esp + 0x98);
	printk("do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",error_code , regs->esp , *p);

	if(!(error_code & 0x01))
		printk("Page Not-Present,\t");

	if(error_code & 0x02)
		printk("Write Cause Fault,\t");
	else
		printk("Read Cause Fault,\t");

	if(error_code & 0x04)
		printk("Fault in user(3)\t");
	else
		printk("Fault in supervisor(0,1,2)\t");

	if(error_code & 0x08)
		printk(",Reserved Bit Cause Fault\t");

	if(error_code & 0x10)
		printk(",Instruction fetch Cause Fault");

	printk("\n");

	printk("CR2:%#018lx\n",cr2);
}