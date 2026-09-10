#include <interrupts/interrupts.h>
#include <arch/x86/ptrace.h>
#include <arch/x86/uaccess.h>   /* copy_from_user, EFAULT */
#include <printk.h>
#include <libs/string.h>

/* 系统调用表：syscall_table[nr] = handler */
static syscall_fn_t syscall_table[NR_SYSCALLS];

/* ====== 内置系统调用实现 ====== */

/*
 * sys_write(fd, buf, count, _, _)
 *
 * 通过 copy_from_user 将用户缓冲区数据拷贝到内核栈缓冲后再输出。
 *
 * 流程（Linux 2.6.20 标准路径）：
 *   1. 校验 count 合法性（<= 0 返回 -EINVAL，过大则截断到 WRITE_BUF_MAX）
 *   2. copy_from_user(kbuf, buf, count)：
 *      - access_ok(buf, count) 检查用户地址 < TASK_SIZE
 *      - 校验失败 → 返回未拷贝字节数（>0）→ sys_write 返回 -EFAULT
 *      - 校验成功 → memcpy 到内核缓冲区
 *   3. 逐字符 printk 输出内核缓冲区内容
 *   4. 返回实际写入字节数
 *
 * 当前尚未引入 VFS，fd 参数暂忽略，直接走 printk 控制台输出。
 * VFS 接入后（Task 8）改为 vfs_write(fd, kbuf, count)。
 */
#define WRITE_BUF_MAX  1024

static long sys_write(long fd, long buf, long count, long _d, long _e)
{
    (void)fd; (void)_d; (void)_e;

    if (count <= 0)
        return -1;  /* -EINVAL */

    /* 限制单次写入量，避免内核栈溢出 */
    if (count > WRITE_BUF_MAX)
        count = WRITE_BUF_MAX;

    /* 内核栈缓冲区（最大 WRITE_BUF_MAX 字节） */
    char kbuf[WRITE_BUF_MAX];

    /* copy_from_user：校验用户地址并拷贝；返回未拷贝字节数 */
    unsigned long uncopied = copy_from_user(kbuf, (const void *)buf,
                                            (unsigned long)count);
    if (uncopied)
        return -EFAULT;  /* 用户地址非法 */

    /* 从内核缓冲区输出（不再直接访问用户指针） */
    for (long i = 0; i < count && kbuf[i]; i++)
        printk("%c", kbuf[i]);

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
