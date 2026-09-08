/*
 * LulaOS CMOS RTC 驱动
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/time.c mach_get_cmos_time()
 */

#include <arch/x86/rtc.h>
#include <arch/x86/io.h>
#include <arch/x86/apic.h>
#include <arch/x86/system.h>
#include <stddef.h>

#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

/* CMOS 寄存器索引 */
#define RTC_SEC     0x00
#define RTC_MIN     0x02
#define RTC_HOUR    0x04
#define RTC_WDAY    0x06
#define RTC_MDAY    0x07
#define RTC_MON     0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32
#define RTC_REG_A   0x0A
#define RTC_REG_B   0x0B

#define RTC_A_UIP   0x80    /* Update In Progress */
#define RTC_B_DM    0x04    /* 1=二进制, 0=BCD */
#define RTC_B_24H   0x02    /* 1=24 小时制 */

static inline unsigned char cmos_read(unsigned char reg)
{
    outb(reg, CMOS_ADDR);
    return inb(CMOS_DATA);
}

/* BCD（如 0x26 表示 26）转二进制 */
static inline unsigned char bcd2bin(unsigned char val)
{
    return (unsigned char)((val & 0x0F) + (val >> 4) * 10);
}

/*
 * rtc_read_time - 读取 RTC 当前时间
 *
 * 稳定性保证：
 *   - UIP 置位表示 RTC 正在把内部时间拷贝到影子寄存器，
 *     此刻读取会得到撕裂数据，必须等待 UIP 清零
 *   - 连续读两次，数值一致才认为有效（跨越秒边界重读）
 */
void rtc_read_time(struct rtc_time *tm)
{
    struct rtc_time t1, t2;
    unsigned char reg_b;
    int tries;

    /* 等待 UIP 清零（RTC 更新周期结束），最多约 100ms */
    for (tries = 0; tries < 10000; tries++) {
        if (!(cmos_read(RTC_REG_A) & RTC_A_UIP))
            break;
        udelay(10);
    }

    /* 读两次直到稳定 */
    do {
        t1.sec  = cmos_read(RTC_SEC);
        t1.min  = cmos_read(RTC_MIN);
        t1.hour = cmos_read(RTC_HOUR);
        t1.wday = cmos_read(RTC_WDAY);
        t1.mday = cmos_read(RTC_MDAY);
        t1.mon  = cmos_read(RTC_MON);
        t1.year = cmos_read(RTC_YEAR);
        reg_b   = cmos_read(RTC_REG_B);

        t2.sec  = cmos_read(RTC_SEC);
        t2.min  = cmos_read(RTC_MIN);
        t2.hour = cmos_read(RTC_HOUR);
        t2.wday = cmos_read(RTC_WDAY);
        t2.mday = cmos_read(RTC_MDAY);
        t2.mon  = cmos_read(RTC_MON);
        t2.year = cmos_read(RTC_YEAR);
    } while (t1.sec != t2.sec || t1.min != t2.min ||
             t1.hour != t2.hour || t1.mday != t2.mday ||
             t1.mon != t2.mon || t1.year != t2.year);

    /* BCD → 二进制 */
    if (!(reg_b & RTC_B_DM)) {
        t1.sec  = bcd2bin((unsigned char)t1.sec);
        t1.min  = bcd2bin((unsigned char)t1.min);
        t1.mday = bcd2bin((unsigned char)t1.mday);
        t1.mon  = bcd2bin((unsigned char)t1.mon);
        t1.year = bcd2bin((unsigned char)t1.year);

        /* 12 小时制：bit7=PM */
        if (!(reg_b & RTC_B_24H)) {
            unsigned char hour = (unsigned char)t1.hour;
            if (hour & 0x80) {           /* PM */
                hour = (unsigned char)((hour & 0x7F) + 12);
                if (hour >= 24)
                    hour -= 24;
            } else if (hour == 12) {     /* 12 AM = 0 点 */
                hour = 0;
            }
            t1.hour = hour;
        }
    }

    *tm = t1;
}
