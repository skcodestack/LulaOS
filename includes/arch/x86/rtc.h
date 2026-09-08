/*
 * LulaOS CMOS RTC 接口
 *
 * 参考 Linux 2.6.20 arch/i386/kernel/time.c mach_get_cmos_time()
 *
 * MC146818 兼容 RTC 通过 I/O 端口 0x70（索引）/0x71（数据）访问：
 *   0x00 秒   0x02 分   0x04 时   0x06 星期
 *   0x07 日   0x08 月   0x09 年（两位） 0x32 世纪（部分机器缺失）
 *   0x0A Status A：bit7 UIP（更新中，须等待清零后再读）
 *   0x0B Status B：bit2=1 二进制格式（0=BCD），bit1=1 24 小时制
 */

#ifndef __ARCH_X86_RTC_H
#define __ARCH_X86_RTC_H

struct rtc_time {
    unsigned int sec;    /* 秒 0-59 */
    unsigned int min;    /* 分 0-59 */
    unsigned int hour;   /* 时 0-23 */
    unsigned int mday;   /* 日 1-31 */
    unsigned int mon;    /* 月 1-12 */
    unsigned int year;   /* 两位年（如 26 = 2026），世纪由调用方补 */
    unsigned int wday;   /* 星期 0-6 */
};

/*
 * rtc_read_time - 读取 CMOS RTC 当前时间
 *
 * 读取策略（参考 Linux）：
 *   1. 等 Status A 的 UIP 清零（RTC 不在更新影子寄存器）
 *   2. 读两次取值稳定为止（防止跨越秒边界读到撕裂数据）
 *   3. 按 Status B 的 BCD/二进制、12/24 小时制换算
 */
void rtc_read_time(struct rtc_time *tm);

#endif /* __ARCH_X86_RTC_H */
