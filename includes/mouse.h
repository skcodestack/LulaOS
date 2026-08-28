/*
 * LulaOS PS/2 鼠标驱动
 *
 * kernel/mouse.c：
 *   - 仅注册 Platform 驱动（名称 PNP0F13）
 *   - 设备由 ACPI DSDT 枚举或 i8042 控制器回退注册
 *   - probe 时调用 request_irq() 注册 IRQ12 处理函数
 *   - PS/2 控制器初始化已在 i8042_probe() 中完成
 */

#ifndef __MOUSE_H__
#define __MOUSE_H__

/*
 * mouse_init - 注册鼠标 Platform 驱动
 *
 * 只注册驱动，不注册设备。设备由 ACPI 或 i8042 控制器提供。
 * 匹配成功后自动调用 mouse_probe() 注册中断。
 *
 * 须在 i8042_init()（控制器初始化完毕）之后调用。
 */
void mouse_init(void);

#endif /* __MOUSE_H__ */
