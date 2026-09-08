/*
 * LulaOS 时间子系统实现
 *
 * 参考 Linux 2.6.20 kernel/timer.c update_times() + kernel/time.c mktime()
 */

#include <time.h>
#include <printk.h>
#include <arch/x86/rtc.h>

/* 全局 tick 计数：APIC Timer 每 10ms 递增一次（HZ=100） */
unsigned long volatile jiffies = 0;

/* 墙上时钟：time_init() 从 RTC 初始化 */
struct timespec xtime = { .tv_sec = 0, .tv_nsec = 0 };

/*
 * mktime - 年月日时分秒 → Unix 时间戳
 *
 * 移植自 Linux 2.6.20 kernel/time.c：
 * 把月份平移到 3 月（使 2 月置于年末，闰日进位自然发生），
 * 累计天数 *24h + 时分秒合成总秒数。
 */
unsigned long mktime(const unsigned int year0, const unsigned int mon0,
                     const unsigned int day, const unsigned int hour,
                     const unsigned int min, const unsigned int sec)
{
    unsigned int mon = mon0, year = year0;

    /* 1、2 月视为上一年的 13、14 月 */
    if (0 >= (int)(mon -= 2)) {
        mon += 12;      /* Puts Feb last since it has leap day */
        year -= 1;
    }

    return ((((unsigned long)(year / 4 - year / 100 + year / 400 +
            367 * mon / 12 + day) +
            year * 365 - 719499       /* 1970-01-01 偏移 */
        ) * 24 + hour                  /* 小时 */
        ) * 60 + min                   /* 分钟 */
        ) * 60 + sec;                  /* 秒 */
}

/*
 * do_timer - timer 中断顶层
 *
 * 由 do_apic_timer_interrupt() 在 BSP 上调用（多 CPU 各自的 timer
 * 都以 10ms 周期触发，若都累加 jiffies 会快 4 倍，故仅 BSP 计时）。
 *
 * 参考 Linux 2.6.20 kernel/timer.c update_times()：
 *   jiffies++；xtime.tv_nsec += TICK_NSEC，进位到秒。
 */
void do_timer(void)
{
    jiffies++;

    xtime.tv_nsec += TICK_NSEC;
    while (xtime.tv_nsec >= NSEC_PER_SEC) {
        xtime.tv_nsec -= NSEC_PER_SEC;
        xtime.tv_sec++;
    }
}

unsigned long get_seconds(void)
{
    return xtime.tv_sec;
}

/*
 * time_init - 初始化墙上时钟
 *
 * 读取 CMOS RTC：年月日时分秒 → mktime() → xtime
 */
void time_init(void)
{
    struct rtc_time tm;

    rtc_read_time(&tm);

    /* RTC 只有两位年（如 26 表示 2026），补上世纪 2000 */
    xtime.tv_sec = mktime(tm.year + 2000, tm.mon, tm.mday,
                          tm.hour, tm.min, tm.sec);
    xtime.tv_nsec = 0;

    printk("time: RTC %04d-%02d-%02d %02d:%02d:%02d UTC"
           " (unix time %lu, HZ=%d)\n",
           tm.year + 2000, tm.mon, tm.mday,
           tm.hour, tm.min, tm.sec, xtime.tv_sec, HZ);
}
