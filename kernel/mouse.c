/*
 * LulaOS PS/2 鼠标驱动实现
 *
 * 中断路径：
 *   PS/2 控制器 → ISA IRQ12 → IOAPIC GSI12 → 向量 0x3D
 *   → entry.S irq_entry_0x3D → interrupt_wrapper → do_IRQ()
 *   → mouse_handler()：读端口 0x60，解析 3 字节数据包
 *
 * PS/2 鼠标数据包格式（标准 3 字节）：
 *   Byte 0: [YO XO YS XS 1 MB RB LB]  溢出/符号/按键
 *   Byte 1: X 位移（有符号，符号位 = Byte0.bit4）
 *   Byte 2: Y 位移（有符号，符号位 = Byte0.bit5）
 *
 * 与键盘共享 PS/2 控制器端口（0x60/0x64），通过状态寄存器
 * bit 5 区分数据来源：bit5=1 为鼠标，bit5=0 为键盘。
 */

#include <mouse.h>
#include <interrupts/interrupts.h>
#include <arch/x86/io.h>
#include <arch/x86/ptrace.h>
#include <printk.h>
#include <stddef.h>

/* PS/2 控制器 I/O 端口 */
#define PS2_DATA_PORT    0x60   /* 数据端口（键盘/鼠标共用） */
#define PS2_STATUS_PORT  0x64   /* 状态寄存器端口 */
#define PS2_CMD_PORT     0x64   /* 命令端口（写） */

/* 鼠标中断向量：ISA IRQ12 → GSI 12 → FIRST_DEVICE_VECTOR+12 */
#define MOUSE_VECTOR     (FIRST_DEVICE_VECTOR + 12)   /* 0x3D */

/* PS/2 控制器命令 */
#define PS2_CMD_ENABLE_AUX    0xA8   /* 启用 AUX（鼠标）端口 */
#define PS2_CMD_READ_CFG      0x20   /* 读取配置字节 */
#define PS2_CMD_WRITE_CFG     0x60   /* 写入配置字节 */
#define PS2_CMD_WRITE_MOUSE   0xD4   /* 下一字节发往鼠标 */

/* 鼠标命令（需通过 0xD4 前缀发送） */
#define MOUSE_CMD_ENABLE      0xF4   /* 启用数据上报 */
#define MOUSE_CMD_SET_DEFAULTS 0xF6  /* 恢复默认设置（100 采样/秒，4:1 缩放） */

/* 数据包解析状态机 */
static unsigned char mouse_cycle = 0;    /* 当前字节序号 0/1/2 */
static signed char   mouse_byte[3];      /* 3 字节缓冲区 */

/* ========== PS/2 端口辅助函数 ========== */

/* 等待超时计数（防止死循环） */
#define PS2_TIMEOUT  100000

/*
 * ps2_wait_input - 等待 PS/2 控制器输入缓冲区空闲
 *
 * 状态寄存器 bit1=1 表示输入缓冲区满（不可写入），
 * 循环等待直到 bit1=0。
 * 返回：0 成功，-1 超时
 */
static int ps2_wait_input(void)
{
    int timeout = PS2_TIMEOUT;
    while (inb(PS2_STATUS_PORT) & 0x02) {
        if (--timeout <= 0)
            return -1;
    }
    return 0;
}

/*
 * ps2_wait_output - 等待 PS/2 控制器输出缓冲区就绪
 *
 * 状态寄存器 bit0=1 表示输出缓冲区有数据可读，
 * 循环等待直到 bit0=1。
 * 返回：0 成功，-1 超时
 */
static int ps2_wait_output(void)
{
    int timeout = PS2_TIMEOUT;
    while (!(inb(PS2_STATUS_PORT) & 0x01)) {
        if (--timeout <= 0)
            return -1;
    }
    return 0;
}

/*
 * mouse_send_cmd - 向鼠标发送命令字节
 *
 * PS/2 控制器端口 0x60 默认与键盘通信，向鼠标发命令需要
 * 先发送 0xD4 前缀，控制器才会将下一字节转发给鼠标。
 * 返回：0 成功，-1 超时
 */
static int mouse_send_cmd(unsigned char cmd)
{
    if (ps2_wait_input() < 0)
        return -1;
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_MOUSE);   /* 告诉控制器：下一字节发鼠标 */
    if (ps2_wait_input() < 0)
        return -1;
    outb(PS2_DATA_PORT, cmd);                    /* 发送实际命令 */
    return 0;
}

/*
 * mouse_read_response - 读取鼠标响应字节
 *
 * 等待输出缓冲区就绪后读取数据端口。
 * 正常 ACK 响应为 0xFA。
 * 返回：响应字节，或 -1 超时
 */
static int mouse_read_response(void)
{
    if (ps2_wait_output() < 0)
        return -1;
    return (int)inb(PS2_DATA_PORT);
}

/* ========== 中断处理函数 ========== */

/*
 * mouse_handler - 鼠标中断顶半部处理函数
 *
 * 由 do_IRQ() 在向量 0x3D 触发时调用。
 * 流程：
 *   1. 检查状态寄存器：bit5=1（鼠标数据）且 bit0=1（有数据）
 *   2. 从端口 0x60 读取字节，进入状态机解析
 *   3. 收齐 3 字节后解析位移和按键，printk 输出
 *
 * 数据包同步：Byte 0 的 bit3 必须为 1（硬件规范），用于检测
 * 是否从数据包开头开始读取，防止中间字节误同步。
 */
static void mouse_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    unsigned char status;
    signed char data;
    int dx, dy;
    int left, right, middle;

    (void)irq;
    (void)dev_id;
    (void)regs;

    /* 检查状态：bit5=1 才是鼠标数据，bit0=1 表示有数据可读 */
    status = inb(PS2_STATUS_PORT);
    if (!(status & 0x21))
        return;

    /* 读取数据字节 */
    data = (signed char)inb(PS2_DATA_PORT);

    /* 状态机：按 3 字节一组解析 */
    switch (mouse_cycle) {
    case 0:
        /* Byte 0：bit3 必须为 1（同步位），否则丢弃 */
        if (!(data & 0x08))
            return;
        mouse_byte[0] = data;
        mouse_cycle = 1;
        break;

    case 1:
        /* Byte 1：X 位移 */
        mouse_byte[1] = data;
        mouse_cycle = 2;
        break;

    case 2:
        /* Byte 2：Y 位移 —— 数据包完整，解析 */
        mouse_byte[2] = data;
        mouse_cycle = 0;

        /* X 位移：Byte1 是 8 位有符号，Byte0.bit4 是符号扩展位 */
        dx = (int)(signed char)mouse_byte[1];
        if (mouse_byte[0] & 0x10)
            dx |= 0xFFFFFF00;

        /* Y 位移：Byte2 是 8 位有符号，Byte0.bit5 是符号扩展位 */
        dy = (int)(signed char)mouse_byte[2];
        if (mouse_byte[0] & 0x20)
            dy |= 0xFFFFFF00;

        /* 按键状态 */
        left   = mouse_byte[0] & 0x01;
        right  = (mouse_byte[0] & 0x02) >> 1;
        middle = (mouse_byte[0] & 0x04) >> 2;

        /* 有位移或按键时才输出，避免刷屏 */
        if (dx || dy || left || right || middle)
            printk("mouse: dx=%d dy=%d L=%d R=%d M=%d\n",
                   dx, dy, left, right, middle);
        break;
    }
}

/* ========== 初始化 ========== */

/*
 * mouse_init - 初始化 PS/2 鼠标并注册中断
 *
 * 流程：
 *   1. 发送 ENABLE_AUX 命令，启用控制器的第二端口
 *   2. 读取控制器配置字节，置位 bit1（开启 AUX IRQ），
 *      清除 bit5（确保鼠标时钟未禁用），写回配置
 *   3. 向鼠标发送 SET_DEFAULTS（恢复 100Hz/4:1 缩放）
 *   4. 向鼠标发送 ENABLE（开始数据上报）
 *   5. 调用 request_irq() 注册 IRQ12 处理函数
 */
void mouse_init(void)
{
    unsigned char cfg;
    int ret;
    int resp;

    /* 1. 启用 AUX（鼠标）端口 */
    if (ps2_wait_input() < 0) {
        printk("mouse: PS/2 controller not ready, skip init\n");
        return;
    }
    outb(PS2_CMD_PORT, PS2_CMD_ENABLE_AUX);

    /* 2. 读-改-写配置字节：开启 AUX 中断 */
    if (ps2_wait_input() < 0)
        goto fail;
    outb(PS2_CMD_PORT, PS2_CMD_READ_CFG);
    resp = mouse_read_response();
    if (resp < 0)
        goto fail;
    cfg = (unsigned char)resp;

    cfg |= 0x02;    /* bit1=1: 开启 AUX 端口中断（IRQ12） */
    cfg &= ~0x20;   /* bit5=0: 确保鼠标时钟未禁用 */

    if (ps2_wait_input() < 0)
        goto fail;
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CFG);
    if (ps2_wait_input() < 0)
        goto fail;
    outb(PS2_DATA_PORT, cfg);

    /* 3. 向鼠标发送 SET_DEFAULTS */
    if (mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS) < 0)
        goto fail;
    resp = mouse_read_response();
    if (resp < 0 || (unsigned char)resp != 0xFA) {
        printk("mouse: no mouse detected (ACK=%#x)\n", resp);
        goto fail;
    }

    /* 4. 向鼠标发送 ENABLE（开始上报） */
    if (mouse_send_cmd(MOUSE_CMD_ENABLE) < 0)
        goto fail;
    resp = mouse_read_response();
    if (resp < 0 || (unsigned char)resp != 0xFA) {
        printk("mouse: enable failed (ACK=%#x)\n", resp);
        goto fail;
    }

    /* 5. 注册 IRQ12 处理函数 */
    ret = request_irq(MOUSE_VECTOR, mouse_handler, "mouse", NULL);
    if (ret == 0)
        printk("mouse: IRQ12 registered (vector=%#x)\n", MOUSE_VECTOR);
    else
        printk("mouse: failed to register IRQ12 (vector=%#x)\n",
               MOUSE_VECTOR);
    return;

fail:
    printk("mouse: init failed, PS/2 mouse not available\n");
}
