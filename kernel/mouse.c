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
#include <device/platform.h>
#include <interrupts/interrupts.h>
#include <arch/x86/io.h>
#include <arch/x86/ptrace.h>
#include <printk.h>
#include <stddef.h>

/* PS/2 控制器 I/O 端口 */
#define PS2_DATA_PORT    0x60   /* 数据端口（键盘/鼠标共用） */
#define PS2_STATUS_PORT  0x64   /* 状态寄存器端口 */

/* 鼠标中断向量：ISA IRQ12 → GSI 12 → FIRST_DEVICE_VECTOR+12 */
#define MOUSE_VECTOR     (FIRST_DEVICE_VECTOR + 12)   /* 0x3D */

/* 数据包解析状态机 */
static unsigned char mouse_cycle = 0;    /* 当前字节序号 0/1/2 */
static signed char   mouse_byte[3];      /* 3 字节缓冲区 */

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

/* ========== Platform 驱动定义 ==========
 *
 * 鼠标 Platform 设备由 ACPI DSDT 枚举（HID=PNP0F13）或
 * i8042 控制器驱动注册。本驱动只负责匹配设备并注册中断处理函数。
 * PS/2 控制器的全部硬件初始化已在 i8042_probe() 中完成。
 */

/*
 * mouse_probe - 鼠标 Platform 驱动 probe
 *
 * 匹配成功后注册鼠标中断处理函数。
 * 此时 i8042 控制器已完成初始化，鼠标端口可用。
 */
static int mouse_probe(struct platform_device *pdev)
{
    int ret = request_irq(MOUSE_VECTOR, mouse_handler, "mouse", NULL);
    if (ret == 0)
        printk("mouse: IRQ12 registered (vector=%#x)\n", MOUSE_VECTOR);
    else
        printk("mouse: failed to register IRQ12 (vector=%#x)\n",
               MOUSE_VECTOR);
    return ret;
}

static struct platform_driver mouse_driver = {
    .driver.name = "PNP0F13",   /* ACPI 鼠标 HID */
    .probe = mouse_probe,
};

/*
 * mouse_init - 注册鼠标 Platform 驱动
 *
 * 设备由 ACPI DSDT 枚举或 i8042 控制器注册，本函数只注册驱动。
 * 匹配成功后自动调用 mouse_probe() 注册中断。
 */
void mouse_init(void)
{
    platform_driver_register(&mouse_driver);
}
