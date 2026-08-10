#ifndef __THREAD_H__
#define __THREAD_H__

#define NR_CPUS    32

/* 进程切换时保存的 CPU 寄存器上下文（仅 callee-saved 寄存器） */
typedef struct thread_struct {
    unsigned long esp;      /* 保存的内核栈指针（pushal 后） */
    unsigned long eip;      /* 返回地址（switch_to 的 ret 弹出） */
    unsigned long ebp;
    unsigned long ebx;
    unsigned long esi;
    unsigned long edi;
    unsigned long esp0;     /* 内核栈顶（TSS 兼容，暂保留） */
    unsigned long pgd;      /* CR3 页目录基地址 */
    unsigned long flags;    /* PF_* 标志 */
} thread_struct;

#define INIT_THREAD { \
    .esp  = 0, \
    .eip  = 0, \
    .ebp  = 0, \
    .ebx  = 0, \
    .esi  = 0, \
    .edi  = 0, \
    .esp0 = 0, \
    .pgd  = 0, \
    .flags = 0 \
}

#endif
