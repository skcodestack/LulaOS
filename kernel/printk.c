#include <arch/linkage.h>
#include <tty/tty.h>
#include <stdarg.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <arch/x86/spinlock.h>

/*
 * SMP 安全的 printk 实现：
 *
 * 1. spinlock 互斥：多 CPU 并发调用时，只有持锁的 CPU 能执行格式化和输出，
 *    其他 CPU 在 spin_lock 处自旋等待，避免 printk_buf 和 TTY 输出被破坏。
 *
 * 2. local_irq_save/restore：在获取锁之前关本 CPU 中断，
 *    防止同 CPU 上的中断 handler 也调用 printk 导致死锁
 *    （本 CPU 已持有锁 → 中断 handler 试图获取同一把锁 → 永久自旋）。
 */
static spinlock_t printk_lock = SPIN_LOCK_UNLOCKED;
static char printk_buf[4096];

void printk(const char *fmt, ...)
{
    unsigned long flags;
    va_list args;

    spin_lock_irqsave(&printk_lock, flags);

    va_start(args, fmt);
    vsprintf(printk_buf, fmt, args);
    va_end(args);

    tty_put_string(printk_buf);

    spin_unlock_irqrestore(&printk_lock, flags);
}
