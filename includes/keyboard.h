/*
 * LulaOS PS/2 键盘驱动
 *
 * kernel/keyboard.c：
 *   - ISA IRQ1 → GSI 1 → 向量 FIRST_DEVICE_VECTOR+1 (0x32)
 *   - 通过 request_irq() 注册顶半部处理函数
 *   - 从 I/O 端口 0x60 读取扫描码，转换为 ASCII 后经 printk 输出
 *   - 仅支持 Make Code（按下），Break Code（松开）被忽略
 *   - 扫描码集：Set 1（PC/AT 默认）
 */

#ifndef __KEYBOARD_H__
#define __KEYBOARD_H__

/*
 * keyboard_init - 注册键盘中断处理函数
 *
 * 调用 request_irq() 注册 KEYBOARD_VECTOR (0x32) 的处理函数，
 * 成功后 IOAPIC RTE 自动解除屏蔽，键盘中断开始触发。
 *
 * 须在 _init_interrupts()（IOAPIC 初始化完毕）之后调用。
 */
void keyboard_init(void);

#endif /* __KEYBOARD_H__ */
