
#ifndef __PTRACE_H__
#define __PTRACE_H__

struct pt_regs
{
    long ebx;
	long ecx;
	long edx;
	long esi;
	long edi;
	long ebp;
	long ds;
	long es; 
	long eax;
    long func_addr;
    long error_code;
	long eip;
	long cs;
	long eflags;
	long esp;
	long ss;
};
 
#endif

