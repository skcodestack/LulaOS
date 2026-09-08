/*
 * LulaOS 时间子系统
 *
 * 参考 Linux 2.6.20 include/linux/time.h + kernel/time.c：
 *   - jiffies：全局 tick 计数（BSP 的 APIC Timer 每 1/HZ 秒递增一次）
 *   - xtime：墙上时钟（由 RTC 在启动时初始化）
 *   - mktime()：年月日时分秒 → Unix 时间戳（移植自 Linux 2.6.20）
 *
 * ext2 文件系统的时间戳（atime/mtime/ctime）直接使用 get_seconds()。
 */

#ifndef __LULA_TIME_H
#define __LULA_TIME_H

#define HZ              100
#define NSEC_PER_SEC    1000000000L
#define TICK_NSEC       (NSEC_PER_SEC / HZ)

/* 1970-01-01 00:00:00 UTC 起的秒数 */
typedef long time_t;

struct timespec {
    long tv_sec;     /* 秒 */
    long tv_nsec;    /* 纳秒 */
};

struct timeval {
    long tv_sec;     /* 秒 */
    long tv_usec;    /* 微秒 */
};

/* 全局 tick 计数（volatile：中断上下文与进程上下文共享） */
extern unsigned long volatile jiffies;

/* 墙上时钟 */
extern struct timespec xtime;

/*
 * mktime - 年月日时分秒 → Unix 时间戳
 *
 * 移植自 Linux 2.6.20 kernel/time.c mktime()，
 * 算法把日期换算为自 1970-01-01 起的天数再合成秒数。
 */
unsigned long mktime(const unsigned int year, const unsigned int mon,
                     const unsigned int day, const unsigned int hour,
                     const unsigned int min, const unsigned int sec);

/*
 * do_timer - timer 中断顶层（仅 BSP 调用，保证全局节拍唯一）
 *   jiffies++ 并推进 xtime
 */
void do_timer(void);

/*
 * time_init - 初始化墙上时钟
 *   读取 CMOS RTC 的真实日期时间，换算为 Unix 时间戳写入 xtime
 */
void time_init(void);

/* 当前时间（秒） */
unsigned long get_seconds(void);

#endif /* __LULA_TIME_H */
