
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
	long es;        /* RESTORE_ALL 先 pop ES 再 pop DS，与 entry.S 布局一致 */
	long ds;
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

