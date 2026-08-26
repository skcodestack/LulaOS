/*
 * LulaOS PS/2 鼠标驱动
 *
 * kernel/mouse.c：
 *   - ISA IRQ12 → GSI 12 → 向量 FIRST_DEVICE_VECTOR+12 (0x3D)
 *   - 通过 request_irq() 注册顶半部处理函数
 *   - 从 I/O 端口 0x60 读取鼠标数据包（3字节），解析位移和按键
 *   - 与键盘共享 PS/2 控制器端口，需正确区分数据来源
 *
 * 须在 keyboard_init() 之后调用，避免初始化顺序问题。
 */

#ifndef __MOUSE_H__
#define __MOUSE_H__

/*
 * mouse_init - 初始化 PS/2 鼠标并注册中断处理函数
 *
 * 流程：
 *   1. 启用 PS/2 控制器的 AUX（鼠标）端口
 *   2. 读取并修改配置字节，开启 AUX 中断（bit1=1）
 *   3. 向鼠标发送 SET_DEFAULTS + ENABLE_DATA_REPORTING 命令
 *   4. 调用 request_irq() 注册 IRQ12 处理函数
 *
 * 须在 _init_interrupts()（IOAPIC 初始化完毕）之后调用。
 */
void mouse_init(void);

#endif /* __MOUSE_H__ */
