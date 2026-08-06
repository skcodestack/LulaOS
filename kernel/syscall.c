#include <interrupts/interrupts.h>
#include <arch/x86/ptrace.h>
#include <printk.h>
#include <libs/string.h>

/* 系统调用表：syscall_table[nr] = handler */
static syscall_fn_t syscall_table[NR_SYSCALLS];

/* ====== 内置系统调用实现 ====== */

/*
 * sys_write(fd, buf, count, _, _)
 * 简化版：忽略 fd，将 buf 内容通过 printk 输出
 * 返回写入字节数
 */
static long sys_write(long fd, long buf, long count, long _d, long _e)
{
    (void)fd; (void)_d; (void)_e;
    if (!buf || count <= 0)
        return -1;
    const char *s = (const char *)buf;
    for (long i = 0; i < count && s[i]; i++)
        printk("%c", s[i]);
    return count;
}

/*
 * sys_exit(error_code, ...)
 * 当前为占位实现：仅打印并 halt
 */
static long sys_exit(long code, long _b, long _c, long _d, long _e)
{
    (void)_b; (void)_c; (void)_d; (void)_e;
    printk("sys_exit: code=%d\n", (int)code);
    /* 无进程管理，直接停机 */
    asm volatile("cli; hlt");
    return 0; /* unreachable */
}

/*
 * sys_getpid()
 * 占位：返回固定 PID=1（内核进程）
 */
static long sys_getpid(long _a, long _b, long _c, long _d, long _e)
{
    (void)_a; (void)_b; (void)_c; (void)_d; (void)_e;
    return 1;
}

/* ====== 初始化与注册 ====== */

/*
 * syscall_init() - 初始化系统调用表并注册内置调用
 * 由内核启动时调用（在 _init_interrupts 之后）
 */
void syscall_init(void)
{
    for (int i = 0; i < NR_SYSCALLS; i++)
        syscall_table[i] = NULL;

    register_syscall(SYS_WRITE,  sys_write);
    register_syscall(SYS_EXIT,   sys_exit);
    register_syscall(SYS_GETPID, sys_getpid);

    printk("syscall: %d built-in syscalls registered\n", 3);
}

/*
 * register_syscall(nr, fn) - 注册一个系统调用处理函数
 * 返回 0 成功，-1 失败
 */
int register_syscall(unsigned int nr, syscall_fn_t fn)
{
    if (nr >= NR_SYSCALLS)
        return -1;
    syscall_table[nr] = fn;
    return 0;
}

/*
 * do_syscall(regs) - 系统调用总分发器（由 entry.S system_call 调用）
 *
 * regs->eax 存放调用号（进入时保存），返回值写回 regs->eax
 * 参数寄存器：ebx=arg1, ecx=arg2, edx=arg3, esi=arg4, edi=arg5
 */
long do_syscall(struct pt_regs *regs)
{
    unsigned int nr = (unsigned int)regs->eax;

    if (nr >= NR_SYSCALLS || syscall_table[nr] == NULL) {
        printk("do_syscall: invalid syscall number %d\n", nr);
        return -1; /* -ENOSYS */
    }

    long ret = syscall_table[nr](regs->ebx, regs->ecx, regs->edx,
                                  regs->esi, regs->edi);
    return ret;
}
