#include <linkage.h>
#include <tty/tty.h>
#include <stdarg.h>
#include <printk.h>
#include <libs/vsprintf.h>

void printk(const char *fmt, ...)
{
    va_list args;
    static char printk_buf[4096]={0};
    char *p;

    va_start(args, fmt);
    vsprintf(printk_buf, fmt, args);
    va_end(args);

    tty_put_string(printk_buf);
}
