/*
 * LulaOS PS/2 键盘驱动实现
 *
 * 中断路径：
 *   PS/2 控制器 → ISA IRQ1 → IOAPIC GSI1 → 向量 0x32
 *   → entry.S irq_entry_0x32 → interrupt_warpper → do_IRQ()
 *   → keyboard_handler()：读端口 0x60，转 ASCII，printk 输出
 *
 * 扫描码集：Set 1（PC/AT 默认），Make Code 范围 0x01~0x58，
 * Break Code（松开）= Make Code | 0x80，本驱动忽略 Break Code。
 *
 * 仅支持单字符按键（字母/数字/符号），Shift/Ctrl/Alt 修饰键
 * 和 F1~F12 功能键不做处理（打印为 [SCAN:xx] 便于调试）。
 */

#include <keyboard.h>
#include <device/platform.h>
#include <interrupts/interrupts.h>
#include <arch/x86/io.h>
#include <arch/x86/ptrace.h>
#include <printk.h>
#include <stddef.h>

/* PS/2 键盘 I/O 端口 */
#define KBD_DATA_PORT    0x60   /* 扫描码数据端口 */
#define KBD_STATUS_PORT  0x64   /* 状态寄存器端口 */

/* 键盘中断向量：ISA IRQ1 → GSI 1 → FIRST_DEVICE_VECTOR+1 */
#define KEYBOARD_VECTOR  (FIRST_DEVICE_VECTOR + 1)

/* ========== Set 1 扫描码 → ASCII 映射表 ==========
 *
 * 索引 = 扫描码（Make Code），值 = 对应 ASCII 字符
 * '\0' 表示无对应字符（功能键/修饰键）
 * 仅列出 0x01~0x58，0x00 保留为 '\0'
 */
static const char scancode_to_ascii[128] = {
    '\0',  /* 0x00 */
    '\0',  /* 0x01  ESC */
    '1',   /* 0x02 */
    '2',   /* 0x03 */
    '3',   /* 0x04 */
    '4',   /* 0x05 */
    '5',   /* 0x06 */
    '6',   /* 0x07 */
    '7',   /* 0x08 */
    '8',   /* 0x09 */
    '9',   /* 0x0A */
    '0',   /* 0x0B */
    '-',   /* 0x0C */
    '=',   /* 0x0D */
    '\b',  /* 0x0E  Backspace */
    '\t',  /* 0x0F  Tab */
    'q',   /* 0x10 */
    'w',   /* 0x11 */
    'e',   /* 0x12 */
    'r',   /* 0x13 */
    't',   /* 0x14 */
    'y',   /* 0x15 */
    'u',   /* 0x16 */
    'i',   /* 0x17 */
    'o',   /* 0x18 */
    'p',   /* 0x19 */
    '[',   /* 0x1A */
    ']',   /* 0x1B */
    '\n',  /* 0x1C  Enter */
    '\0',  /* 0x1D  Left Ctrl */
    'a',   /* 0x1E */
    's',   /* 0x1F */
    'd',   /* 0x20 */
    'f',   /* 0x21 */
    'g',   /* 0x22 */
    'h',   /* 0x23 */
    'j',   /* 0x24 */
    'k',   /* 0x25 */
    'l',   /* 0x26 */
    ';',   /* 0x27 */
    '\'',  /* 0x28 */
    '`',   /* 0x29 */
    '\0',  /* 0x2A  Left Shift */
    '\\',  /* 0x2B */
    'z',   /* 0x2C */
    'x',   /* 0x2D */
    'c',   /* 0x2E */
    'v',   /* 0x2F */
    'b',   /* 0x30 */
    'n',   /* 0x31 */
    'm',   /* 0x32 */
    ',',   /* 0x33 */
    '.',   /* 0x34 */
    '/',   /* 0x35 */
    '\0',  /* 0x36  Right Shift */
    '*',   /* 0x37  Keypad * */
    '\0',  /* 0x38  Left Alt */
    ' ',   /* 0x39  Space */
    '\0',  /* 0x3A  CapsLock */
    '\0',  /* 0x3B  F1  */
    '\0',  /* 0x3C  F2  */
    '\0',  /* 0x3D  F3  */
    '\0',  /* 0x3E  F4  */
    '\0',  /* 0x3F  F5  */
    '\0',  /* 0x40  F6  */
    '\0',  /* 0x41  F7  */
    '\0',  /* 0x42  F8  */
    '\0',  /* 0x43  F9  */
    '\0',  /* 0x44  F10 */
    '\0',  /* 0x45  NumLock */
    '\0',  /* 0x46  ScrollLock */
    '\0',  /* 0x47  Keypad 7 / Home     */
    '\0',  /* 0x48  Keypad 8 / Up       */
    '\0',  /* 0x49  Keypad 9 / PgUp     */
    '-',   /* 0x4A  Keypad - */
    '\0',  /* 0x4B  Keypad 4 / Left     */
    '\0',  /* 0x4C  Keypad 5            */
    '\0',  /* 0x4D  Keypad 6 / Right    */
    '+',   /* 0x4E  Keypad + */
    '\0',  /* 0x4F  Keypad 1 / End      */
    '\0',  /* 0x50  Keypad 2 / Down     */
    '\0',  /* 0x51  Keypad 3 / PgDn     */
    '\0',  /* 0x52  Keypad 0 / Ins      */
    '\0',  /* 0x53  Keypad . / Del      */
    '\0',  /* 0x54  SysRq               */
    '\0',  /* 0x55                      */
    '\0',  /* 0x56                      */
    '\0',  /* 0x57  F11 */
    '\0',  /* 0x58  F12 */
};

/*
 * keyboard_handler - 键盘中断顶半部处理函数
 *
 * 由 do_IRQ() 在向量 0x32 触发时调用。
 * 流程：
 *   1. 从端口 0x60 读取扫描码
 *   2. 若为 Break Code（bit7=1，松开），直接忽略
 *   3. 若为 Make Code（按下），查表转 ASCII
 *   4. 可打印字符 → printk 输出；功能键 → 打印 [SCAN:xx]
 */
static void keyboard_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    unsigned char status;
    unsigned char scancode;
    char ch;

    (void)irq;
    (void)dev_id;
    (void)regs;

    /*
     * 检查状态寄存器：
     *   bit5=1 表示数据来自鼠标（AUX 端口），不应由键盘处理
     *   bit0=1 表示输出缓冲区有数据可读
     * 与鼠标共享 PS/2 控制器，必须区分数据来源
     */
    status = inb(KBD_STATUS_PORT);
    if (status & 0x20)
        return;   /* 鼠标数据，跳过 */

    /* 读取扫描码（读 0x60 同时清除中断请求） */
    scancode = inb(KBD_DATA_PORT);

    /* Break Code（松开）：bit7=1，忽略 */
    if (scancode & 0x80)
        return;

    /* Make Code（按下）：查表转 ASCII */
    ch = scancode_to_ascii[scancode & 0x7F];

    if (ch == '\n') {
        printk("\n");
    } else if (ch == '\b') {
        /* Backspace：输出退格序列 */
        printk("\b");
    } else if (ch == '\t') {
        printk("    ");   /* Tab：4空格 */
    } else if (ch != '\0') {
        /* 可打印字符 */
        printk("%c", ch);
    }
    /* 功能键/修饰键（ch=='\0'）：静默忽略 */
}

/* ========== Platform 设备/驱动定义 ========== */

static struct platform_resource kbd_resources[] = {
    { .start = KBD_DATA_PORT, .end = KBD_STATUS_PORT, .flags = IORESOURCE_IO },
    { .start = KEYBOARD_VECTOR, .end = KEYBOARD_VECTOR, .flags = IORESOURCE_IRQ },
};

static struct platform_device keyboard_device = {
    .dev.name = "ps2-keyboard",
    .id = -1,
    .resource = kbd_resources,
    .num_resources = 2,
};

/*
 * keyboard_probe - 键盘 Platform 驱动 probe
 *
 * 匹配成功后注册键盘中断处理函数。
 */
static int keyboard_probe(struct platform_device *pdev)
{
    int ret = request_irq(KEYBOARD_VECTOR, keyboard_handler,
                          "keyboard", NULL);
    if (ret == 0)
        printk("keyboard: IRQ1 registered (vector=%#x)\n", KEYBOARD_VECTOR);
    else
        printk("keyboard: failed to register IRQ1 (vector=%#x)\n",
               KEYBOARD_VECTOR);
    return ret;
}

static struct platform_driver keyboard_driver = {
    .driver.name = "ps2-keyboard",
    .probe = keyboard_probe,
};

/*
 * keyboard_init - 注册键盘 Platform 设备和驱动
 *
 * 在统一设备模型下，设备与驱动通过名称匹配，匹配成功后
 * 自动调用 keyboard_probe() 注册中断。
 */
void keyboard_init(void)
{
    platform_device_register(&keyboard_device);
    platform_driver_register(&keyboard_driver);
}
