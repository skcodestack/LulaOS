/*
 * LulaOS PS/2 控制器（i8042）Platform 驱动
 *
 * 参考 Linux drivers/input/serio/i8042.c
 *
 * PS/2 控制器是键盘和鼠标共享的硬件控制器，端口 0x60（数据）和 0x64（状态/命令）。
 * 本驱动作为 Platform 设备注册，probe 时完成全部控制器初始化：
 *
 *   1. 控制器自检（CMD_SELFTEST 0xAA → 期望响应 0x55）
 *   2. 键盘端口检测（CMD_KBD_TEST 0xAB → 期望响应 0x00）
 *   3. AUX 端口启用（CMD_ENABLE_AUX 0xA8）
 *   4. 配置字节读-改-写（开启键盘 IRQ1 + 鼠标 IRQ12）
 *   5. 鼠标设备初始化（SET_DEFAULTS + ENABLE_DATA_REPORTING）
 *
 * 初始化完成后，键盘和鼠标驱动可各自注册中断处理函数。
 */

#include <i8042.h>
#include <device/platform.h>
#include <arch/x86/io.h>
#include <printk.h>
#include <stddef.h>

/* PS/2 控制器 I/O 端口 */
#define I8042_DATA_PORT    0x60   /* 数据端口（读/写） */
#define I8042_STATUS_PORT  0x64   /* 状态寄存器（读） */
#define I8042_CMD_PORT     0x64   /* 命令端口（写） */

/* 控制器命令 */
#define I8042_CMD_READ_CFG     0x20   /* 读取配置字节 */
#define I8042_CMD_WRITE_CFG    0x60   /* 写入配置字节 */
#define I8042_CMD_DISABLE_AUX  0xA7   /* 禁用 AUX（鼠标）端口 */
#define I8042_CMD_ENABLE_AUX   0xA8   /* 启用 AUX（鼠标）端口 */
#define I8042_CMD_SELFTEST     0xAA   /* 控制器自检 */
#define I8042_CMD_KBD_TEST     0xAB   /* 键盘端口检测 */
#define I8042_CMD_WRITE_MOUSE  0xD4   /* 下一字节发往鼠标 */

/* 鼠标命令（需通过 0xD4 前缀发送） */
#define MOUSE_CMD_SET_DEFAULTS 0xF6   /* 恢复默认设置 */
#define MOUSE_CMD_ENABLE       0xF4   /* 启用数据上报 */

/* 等待超时计数 */
#define I8042_TIMEOUT  100000

/* 控制器就绪标志，供键盘/鼠标驱动检查 */
int i8042_ready = 0;

/* ======================== 端口操作辅助 ======================== */

/*
 * i8042_wait_input - 等待输入缓冲区空闲
 *
 * 状态寄存器 bit1=1 表示输入缓冲区满（不可写入）。
 * 返回：0 成功，-1 超时
 */
static int i8042_wait_input(void)
{
    int timeout = I8042_TIMEOUT;
    while (inb(I8042_STATUS_PORT) & 0x02) {
        if (--timeout <= 0)
            return -1;
    }
    return 0;
}

/*
 * i8042_wait_output - 等待输出缓冲区就绪
 *
 * 状态寄存器 bit0=1 表示输出缓冲区有数据可读。
 * 返回：0 成功，-1 超时
 */
static int i8042_wait_output(void)
{
    int timeout = I8042_TIMEOUT;
    while (!(inb(I8042_STATUS_PORT) & 0x01)) {
        if (--timeout <= 0)
            return -1;
    }
    return 0;
}

/*
 * i8042_read_response - 读取控制器响应字节
 *
 * 返回：响应字节，或 -1 超时
 */
static int i8042_read_response(void)
{
    if (i8042_wait_output() < 0)
        return -1;
    return (int)inb(I8042_DATA_PORT);
}

/*
 * i8042_mouse_send_cmd - 向鼠标发送命令字节
 *
 * 需先发 0xD4 前缀，控制器才会将下一字节转发给 AUX 端口。
 * 返回：0 成功，-1 超时
 */
static int i8042_mouse_send_cmd(unsigned char cmd)
{
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_CMD_PORT, I8042_CMD_WRITE_MOUSE);
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_DATA_PORT, cmd);
    return 0;
}

/* ======================== 控制器初始化 ======================== */

/* 前向声明 */
static void i8042_register_child_devices(void);

/*
 * i8042_controller_init - 完成 PS/2 控制器全部初始化
 *
 * 流程：
 *   1. 控制器自检
 *   2. 键盘端口检测
 *   3. 启用 AUX 端口
 *   4. 读-改-写配置字节（开启 KBD + AUX 中断）
 *   5. 鼠标初始化（SET_DEFAULTS + ENABLE）
 *
 * 返回：0 成功，-1 失败（控制器不可用）
 */
static int i8042_controller_init(void)
{
    int resp;
    unsigned char cfg;

    /* 1. 控制器自检 */
    if (i8042_wait_input() < 0) {
        printk("i8042: controller not ready\n");
        return -1;
    }
    outb(I8042_CMD_PORT, I8042_CMD_SELFTEST);
    resp = i8042_read_response();
    if (resp != 0x55) {
        printk("i8042: self-test failed (got %#x, expected 0x55)\n", resp);
        return -1;
    }
    printk("i8042: controller self-test OK\n");

    /* 2. 键盘端口检测 */
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_CMD_PORT, I8042_CMD_KBD_TEST);
    resp = i8042_read_response();
    if (resp != 0x00) {
        printk("i8042: keyboard port test failed (got %#x)\n", resp);
        /* 不致命，继续 */
    } else {
        printk("i8042: keyboard port OK\n");
    }

    /* 3. 启用 AUX（鼠标）端口 */
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_CMD_PORT, I8042_CMD_ENABLE_AUX);

    /* 4. 读取配置字节 */
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_CMD_PORT, I8042_CMD_READ_CFG);
    resp = i8042_read_response();
    if (resp < 0) {
        printk("i8042: failed to read config\n");
        return -1;
    }
    cfg = (unsigned char)resp;
    printk("i8042: config byte = %#x\n", cfg);

    /* 开启键盘中断（bit0=1, IRQ1）和鼠标中断（bit1=1, IRQ12） */
    cfg |= 0x03;
    /* 确保键盘和鼠标时钟未禁用 */
    cfg &= ~0x30;   /* bit4=0: KBD clock enable, bit5=0: AUX clock enable */

    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_CMD_PORT, I8042_CMD_WRITE_CFG);
    if (i8042_wait_input() < 0)
        return -1;
    outb(I8042_DATA_PORT, cfg);
    printk("i8042: config updated to %#x (KBD+AUX IRQ enabled)\n", cfg);

    /* 5. 鼠标初始化 */
    /* 5a. SET_DEFAULTS */
    if (i8042_mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS) < 0) {
        printk("i8042: mouse SET_DEFAULTS timeout (no mouse?)\n");
        goto no_mouse;
    }
    resp = i8042_read_response();
    if (resp < 0 || (unsigned char)resp != 0xFA) {
        printk("i8042: mouse SET_DEFAULTS NACK (got %#x)\n", resp);
        goto no_mouse;
    }

    /* 5b. ENABLE（开始上报） */
    if (i8042_mouse_send_cmd(MOUSE_CMD_ENABLE) < 0) {
        printk("i8042: mouse ENABLE timeout\n");
        goto no_mouse;
    }
    resp = i8042_read_response();
    if (resp < 0 || (unsigned char)resp != 0xFA) {
        printk("i8042: mouse ENABLE NACK (got %#x)\n", resp);
        goto no_mouse;
    }
    printk("i8042: mouse initialized OK\n");

no_mouse:
    printk("i8042: controller ready\n");
    i8042_ready = 1;

    /* 注册键盘/鼠标子设备（若 ACPI 未发现） */
    i8042_register_child_devices();

    return 0;
}

/* ======================== 子设备回退注册 ======================== */

/*
 * 若 ACPI DSDT 扫描未发现键盘/鼠标设备，由 i8042 控制器补充注册。
 * 这样即使 ACPI AML 解析器尚未完整支持 Scope 递归，键盘/鼠标仍能工作。
 */

/* 键盘回退设备 */
static struct platform_resource kbd_fallback_res[] = {
    { .start = I8042_DATA_PORT, .end = I8042_STATUS_PORT, .flags = IORESOURCE_IO },
};
static struct platform_device kbd_fallback_dev = {
    .dev.name = "PNP0303",
    .id = -1,
    .resource = kbd_fallback_res,
    .num_resources = 1,
};

/* 鼠标回退设备 */
static struct platform_resource mouse_fallback_res[] = {
    { .start = I8042_DATA_PORT, .end = I8042_STATUS_PORT, .flags = IORESOURCE_IO },
};
static struct platform_device mouse_fallback_dev = {
    .dev.name = "PNP0F13",
    .id = -1,
    .resource = mouse_fallback_res,
    .num_resources = 1,
};

static void i8042_register_child_devices(void)
{
    if (!platform_find_device("PNP0303")) {
        printk("i8042: ACPI未发现键盘设备，注册回退设备 PNP0303\n");
        platform_device_register(&kbd_fallback_dev);
    }
    if (!platform_find_device("PNP0F13")) {
        printk("i8042: ACPI未发现鼠标设备，注册回退设备 PNP0F13\n");
        platform_device_register(&mouse_fallback_dev);
    }
}

/* ======================== Platform 设备/驱动 ======================== */

static struct platform_resource i8042_resources[] = {
    { .start = I8042_DATA_PORT, .end = I8042_CMD_PORT, .flags = IORESOURCE_IO },
};

static struct platform_device i8042_device = {
    .dev.name = "i8042",
    .id = -1,
    .resource = i8042_resources,
    .num_resources = 1,
};

/*
 * i8042_probe - PS/2 控制器 Platform 驱动 probe
 *
 * 匹配成功后执行控制器全部硬件初始化。
 */
static int i8042_probe(struct platform_device *pdev)
{
    (void)pdev;
    printk("i8042: probing PS/2 controller...\n");
    return i8042_controller_init();
}

static struct platform_driver i8042_driver = {
    .driver.name = "i8042",
    .probe = i8042_probe,
};

/*
 * i8042_init - 注册 PS/2 控制器 Platform 设备和驱动
 */
void i8042_init(void)
{
    platform_device_register(&i8042_device);
    platform_driver_register(&i8042_driver);
}
