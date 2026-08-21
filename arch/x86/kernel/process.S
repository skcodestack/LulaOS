#include <arch/linkage.h>

/*
 * kernel_thread_helper - 新内核线程的首次执行入口
 *
 * 由 ret_from_fork 直接跳转过来，此时栈顶是 kernel_thread 构造的 [fn, arg]。
 * 这里不使用 C 函数序言，直接操作入口栈：弹出 fn/arg，调用 fn(arg)，
 * 函数返回后调用 do_exit() 退出。
 */
ENTRY(kernel_thread_helper)
    sti                 /* 新线程初始处于关中断状态，需显式开启 */
    popl %eax           /* fn  */
    popl %edx           /* arg */
    pushl %edx          /* 压入 arg 作为 fn 的参数 */
    call *%eax          /* fn(arg) */
    addl $4, %esp       /* 清理参数 */
    pushl %eax          /* fn 返回值作为 do_exit(code) 的参数 */
    call SYMBOL_NAME(do_exit)

    /* do_exit 不应返回，若返回则停机 */
1:
    hlt
    jmp 1b
