#ifndef __UACCESS_H__
#define __UACCESS_H__

/*
 * uaccess.h - 用户态内存访问接口
 *
 * 参考 Linux 2.6.20：内核与用户进程地址空间隔离后，
 * 系统调用入口处必须对用户指针做合法性校验，再执行拷贝。
 *
 * LulaOS 当前阶段（尚未引入 demand paging / exception table），
 * 采用"先校验后 memcpy"的保守策略：
 *   - access_ok() 检查地址范围必须完全落在用户空间（< PAGE_OFFSET）
 *   - copy_from_user / copy_to_user 校验通过后用 memcpy 完成实际拷贝
 *   - 拷贝失败返回未拷贝字节数（与 Linux ABI 一致），调用方据此返回 -EFAULT
 *
 * 后续引入 exception table 后可替换为带 fixup 的汇编实现，
 * 处理用户页缺页（demand paging）与非法地址（-EFAULT）。
 */

#include <arch/x86/page.h>     /* PAGE_OFFSET */
#include <libs/memcpy.h>       /* memcpy, memset */

/*
 * TASK_SIZE - 用户进程可用虚拟地址空间上限
 * 在 32 位 x86 Linux 中 = 0xC0000000（3GB 分界），与 PAGE_OFFSET 一致
 */
#define TASK_SIZE   PAGE_OFFSET

/*
 * access_ok - 校验用户地址区间是否合法
 * @addr: 用户空间起始地址
 * @size: 访问字节数
 *
 * 条件：addr != NULL，且 [addr, addr+size) 整个区间落在 [0, TASK_SIZE) 内
 * 返回：1 = 合法，0 = 非法
 *
 * 注意防止 size 溢出：addr + size 可能回绕，用 addr > TASK_SIZE - size 规避
 */
static inline int access_ok(const void *addr, unsigned long size)
{
    unsigned long uaddr = (unsigned long)addr;
    if (size == 0)
        return 1;
    if (uaddr == 0)
        return 0;
    if (size > TASK_SIZE)
        return 0;
    if (uaddr > TASK_SIZE - size)
        return 0;
    return 1;
}

/*
 * copy_from_user - 从用户空间拷贝数据到内核缓冲区
 * @to:   内核目标缓冲区（必须为有效内核地址）
 * @from: 用户空间源地址（必须通过 access_ok 校验）
 * @n:    拷贝字节数
 *
 * 返回：未拷贝的字节数（0 = 全部成功，n = 完全失败）
 *
 * Linux 2.6.20 原始实现使用异常表（exception table）处理页错误，
 * 此处简化为：先做地址校验，校验失败直接返回 n（一个字节都没拷贝），
 * 校验成功则用 memcpy 一次性拷贝。
 */
static inline unsigned long copy_from_user(void *to, const void *from,
                                           unsigned long n)
{
    if (n == 0)
        return 0;
    if (!access_ok(from, n)) {
        /* 非法用户地址，全部失败，并清零目标缓冲区（防止内核读取未初始化数据） */
        memset(to, 0, n);
        return n;
    }
    memcpy(to, from, n);
    return 0;
}

/*
 * copy_to_user - 从内核缓冲区拷贝数据到用户空间
 * @to:   用户空间目标地址（必须通过 access_ok 校验）
 * @from: 内核源缓冲区
 * @n:    拷贝字节数
 *
 * 返回：未拷贝的字节数（0 = 成功，n = 失败）
 */
static inline unsigned long copy_to_user(void *to, const void *from,
                                         unsigned long n)
{
    if (n == 0)
        return 0;
    if (!access_ok(to, n))
        return n;
    memcpy(to, from, n);
    return 0;
}

/*
 * strncpy_from_user - 从用户空间拷贝 C 字符串到内核
 * @dst:   内核目标缓冲区
 * @src:   用户空间源字符串地址
 * @count: 最大拷贝字节数（含结尾 '\0'）
 *
 * 返回：
 *   > 0  : 实际拷贝的字符数（不含 '\0'）
 *   = 0  : src 为空串，仅写入 '\0'
 *   < 0  : -1 表示 src 地址非法（access_ok 失败）
 *
 * 逐字节拷贝，遇到 '\0' 提前停止；始终保证 dst 以 '\0' 结尾。
 */
static inline long strncpy_from_user(char *dst, const char *src, long count)
{
    if (count <= 0)
        return 0;
    /* 先对最大可能范围做粗略校验（最多 count 字节） */
    if (!access_ok(src, 1))
        return -1;
    long i;
    for (i = 0; i < count - 1; i++) {
        /* 逐字节访问前校验，防止 src 字符串跨越 TASK_SIZE 边界 */
        if ((unsigned long)(src + i) >= TASK_SIZE) {
            dst[i] = '\0';
            return -1;
        }
        dst[i] = src[i];
        if (src[i] == '\0')
            return i;  /* 含 '\0' 在内，实际字符数为 i */
    }
    dst[i] = '\0';
    return i;
}

/*
 * EFAULT 错误码（与 Linux errno 一致）
 * 系统调用中 copy_from_user/copy_to_user 失败时使用
 */
#define EFAULT  14

#endif /* __UACCESS_H__ */
