#include <interrupts/interrupts.h>
#include <arch/x86/apic.h>
#include <arch/x86/ptrace.h>
#include <kernel/sched.h>
#include <printk.h>
#include <stddef.h>

/* IRQ 描述符：每个向量对应一个驱动处理函数 */
struct irq_action {
    void (*handler)(int irq, void *dev_id, struct pt_regs *regs);
    void *dev_id;
    const char *name;
};

static struct irq_action irq_desc[NR_IRQS];

void _init_interrupts()
{
    for (int i = 0; i < NR_IRQS; i++) {
        irq_desc[i].handler = NULL;
        irq_desc[i].dev_id  = NULL;
        irq_desc[i].name    = NULL;
    }
    _init_apic();
    syscall_init();
}

/*
 * request_irq - 注册设备中断处理函数
 * @vector: 中断向量号（0x20~0x3f）
 * @handler: 驱动提供的中断处理函数
 * @name: 设备名称（调试用）
 * @dev_id: 设备私有数据，中断时原样传给 handler
 *
 * 返回 0 成功，-1 失败
 *
 * 注册成功后自动解除对应 IOAPIC RTE 的屏蔽，使能硬件中断
 */
int request_irq(unsigned int vector,
                void (*handler)(int irq, void *dev_id, struct pt_regs *regs),
                const char *name,
                void *dev_id)
{
    unsigned int idx = vector - FIRST_EXTERNAL_VECTOR;

    if (vector < FIRST_EXTERNAL_VECTOR || idx >= NR_IRQS)
        return -1;
    if (irq_desc[idx].handler != NULL)
        return -1;  // 已被占用

    irq_desc[idx].handler = handler;
    irq_desc[idx].dev_id  = dev_id;
    irq_desc[idx].name    = name;

    /* 注册成功，解除 IOAPIC RTE 屏蔽，使能硬件中断 */
    ioapic_enable_irq(vector);
    return 0;
}

/*
 * free_irq - 注销中断处理函数，并重新屏蔽对应 GSI
 * @vector: 中断向量号
 */
void free_irq(unsigned int vector)
{
    unsigned int idx = vector - FIRST_EXTERNAL_VECTOR;
    if (vector < FIRST_EXTERNAL_VECTOR || idx >= NR_IRQS)
        return;

    /* 先屏蔽硬件中断，再清除 handler，避免悬空中断触发 */
    ioapic_disable_irq(vector);

    irq_desc[idx].handler = NULL;
    irq_desc[idx].dev_id  = NULL;
    irq_desc[idx].name    = NULL;
}

/*
 * do_IRQ - 硬件中断总入口（由 entry.S 各 irq_entry_XX 调用）
 *
 * error_code 参数实际保存的是向量号（由 BUILD_IRQ 宏压入）
 */
asmlinkage void do_IRQ(struct pt_regs *regs, long error_code)
{
    int vector = error_code;  // 向量号
    unsigned int idx = vector - FIRST_EXTERNAL_VECTOR;

    // 发送 EOI，通知 LAPIC 中断已接受
    apic_write(APIC_EOI, 0);

    if (vector < FIRST_EXTERNAL_VECTOR || idx >= NR_IRQS) {
        printk("do_IRQ: unexpected vector %d\n", vector);
        return;
    }

    struct irq_action *action = &irq_desc[idx];
    if (action->handler) {
        action->handler(vector, action->dev_id, regs);
    } else {
        printk("do_IRQ: vector %d (idx=%d) no handler registered\n", vector, idx);
    }
}

/*
 * do_apic_timer_interrupt - APIC Timer 中断处理函数
 *
 * 由 entry.S 中 apic_timer_entry 调用，向量 = TIMER_APIC_VECTOR (0xEF)
 * 每次 tick 调用 scheduler_tick() 更新时间片，驱动调度器
 */
void do_apic_timer_interrupt(struct pt_regs *regs, long error_code)
{
    /* EOI 必须在任何可能耗时的操作之前发送，
     * 否则 ISR 位会阻塞同级及低优先级中断 */
    apic_write(APIC_EOI, 0);

    /* 驱动调度器：递减时间片，必要时置位 need_resched */
    scheduler_tick();
}

/*
 * do_apic_error_interrupt - APIC Error 中断处理函数
 *
 * 由 entry.S 中 apic_error_entry 调用，向量 = ERROR_APIC_VECTOR (0xFE)
 */
void do_apic_error_interrupt(struct pt_regs *regs, long error_code)
{
    unsigned int lvt_err = apic_read(APIC_ERR);
    printk("APIC Error interrupt: LVT ERR=%#x\n", lvt_err);
    apic_write(APIC_EOI, 0);
}
 