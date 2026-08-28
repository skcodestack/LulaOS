/*
 * LulaOS PS/2 控制器（i8042）驱动
 *
 * 参考 Linux drivers/input/serio/i8042.c
 *
 * PS/2 控制器管理键盘和鼠标两个端口：
 *   - 键盘端口（IRQ1，I/O 0x60/0x64）
 *   - AUX 鼠标端口（IRQ12，I/O 0x60/0x64）
 *
 * 控制器作为 Platform 设备注册，probe 时完成：
 *   1. 自检与端口检测
 *   2. 配置字节读写（开启键盘/鼠标中断）
 *   3. 鼠标初始化（SET_DEFAULTS + ENABLE）
 *
 * 键盘和鼠标驱动只需注册中断处理函数。
 */

#ifndef __I8042_H__
#define __I8042_H__

/* i8042 控制器是否已就绪 */
extern int i8042_ready;

/*
 * i8042_init - 注册 PS/2 控制器 Platform 设备和驱动
 *
 * probe 完成控制器硬件初始化。
 * 须在 platform_bus_init() 和 acpi_register_platform_devices() 之后调用。
 */
void i8042_init(void);

#endif /* __I8042_H__ */
