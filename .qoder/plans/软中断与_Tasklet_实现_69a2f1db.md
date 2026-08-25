# LulaOS 软中断（softirq）与 Tasklet 实现计划

## 设计概述

参考 Linux 2.6.20 的 softirq 子系统，在 LulaOS 中实现：
- per-CPU `softirq_pending` 位掩码（`in_preempt_count` 字段复用为中断嵌套计数）
- 6 个软中断向量，其中 `TASKLET_SOFTIRQ` 驱动 tasklet 执行
- `irq_exit()` 路径自动检测并执行 pending softirq
- Tasklet 链表 + TASKLET_SOFTIRQ 组合，支持 `tasklet_schedule()` / `tasklet_hi_schedule()`

---

## Task 1：新建头文件 `includes/kernel/softirq.h`

新增软中断与 tasklet 的全部数据结构及 API 声明：

```c
/* 软中断向量编号 */
enum {
    HI_SOFTIRQ = 0,
    TIMER_SOFTIRQ,
    NET_TX_SOFTIRQ,
    NET_RX_SOFTIRQ,
    BLOCK_SOFTIRQ,
    TASKLET_SOFTIRQ,
    NR_SOFTIRQS
};

/* softirq_action */
struct softirq_action {
    void (*action)(struct softirq_action *);
    void *data;
};

/* open_softirq / raise_softirq / do_softirq / irq_enter / irq_exit */

/* tasklet_struct */
struct tasklet_struct {
    struct tasklet_struct *next;
    unsigned long state;       /* 0=normal, 1=scheduled, 2=disabled */
    void (*func)(unsigned long);
    unsigned long data;
};

/* tasklet_init / tasklet_schedule / tasklet_hi_schedule / tasklet_disable / tasklet_enable */
/* softirq_init() */
```

---

## Task 2：新建实现文件 `kernel/softirq.c`

核心实现逻辑：

**数据结构：**
- `static struct softirq_action softirq_vec[NR_SOFTIRQS]`
- `static unsigned long softirq_pending[NR_CPUS]`（per-CPU 位掩码）
- `static int in_irq[NR_CPUS]`（中断嵌套计数，`irq_enter` ++ / `irq_exit` --）

**核心函数：**

| 函数 | 行为 |
|------|------|
| `open_softirq(nr, action, data)` | 注册向量 |
| `raise_softirq(nr)` | 原子置位 `softirq_pending[cpu]` |
| `do_softirq()` | 读+清零 pending，开中断，依次执行各置位向量的 action，关中断 |
| `irq_enter()` | `in_irq[cpu]++` |
| `irq_exit()` | `in_irq[cpu]--`；若 ==0 且 pending!=0 则调用 `do_softirq()` |
| `softirq_init()` | 注册 TASKLET_SOFTIRQ → `tasklet_action`，HI_SOFTIRQ → `tasklet_hi_action` |

**Tasklet 实现：**
- `tasklet_head` / `tasklet_hi_head`：per-CPU 链表头（简化为全局单链表，`local_irq_save` 保护）
- `tasklet_schedule(t)`：将 t 链入 `tasklet_head`，`raise_softirq(TASKLET_SOFTIRQ)`
- `tasklet_hi_schedule(t)`：链入 `tasklet_hi_head`，`raise_softirq(HI_SOFTIRQ)`
- `tasklet_action()`：遍历 `tasklet_head`，对 state==1 的执行 `func(data)` 并清除 state
- `tasklet_hi_action()`：同上，操作 `tasklet_hi_head`

**ksoftirqd 内核线程（防软中断风暴）：**

`do_softirq()` 执行一轮后，若 `softirq_pending` 再次被置位（handler 内部又重新 raise），则不再循环执行，而是唤醒 `ksoftirqd` 在进程上下文中处理，避免长时间霸占 CPU。

数据结构：
```c
/* 每个 CPU 一个 ksoftirqd 线程 */
static struct task_struct *ksoftirqd_thread[NR_CPUS];
```

线程主循环（每个 CPU 独立）：
```c
static int ksoftirqd(void *arg)
{
    int cpu = (int)(long)arg;
    for (;;) {
        /* 无 pending 时睡眠，等待 raise_softirq 唤醒 */
        if (!softirq_pending[cpu])
            interruptible_sleep();

        /* 有 pending，循环处理直到清空 */
        while (softirq_pending[cpu])
            do_softirq();  /* 执行当前 pending */
    }
}
```

`do_softirq()` 修改逻辑：
```c
void do_softirq(void)
{
    unsigned long pending;
    int cpu = smp_processor_id();

    pending = softirq_pending[cpu];
    if (!pending) return;

    softirq_pending[cpu] = 0;  /* 读+清零 */

    /* 开中断，依次执行各置位向量 */
    local_irq_enable();
    for (int i = 0; i < NR_SOFTIRQS; i++)
        if (pending & (1 << i) && softirq_vec[i].action)
            softirq_vec[i].action(&softirq_vec[i]);
    local_irq_disable();

    /* 执行期间若又被 raise，唤醒 ksoftirqd 处理，不在中断上下文循环 */
    if (softirq_pending[cpu] && ksoftirqd_thread[cpu]) {
        wake_up_process(ksoftirqd_thread[cpu]);
    }
}
```

`softirq_init()` 中创建 ksoftirqd：
```c
void softirq_init(void)
{
    /* ... 注册 TASKLET_SOFTIRQ / HI_SOFTIRQ ... */

    /* 为每个在线 CPU 创建 ksoftirqd 内核线程 */
    for (int cpu = 0; cpu < NR_CPUS; cpu++) {
        if (!(cpu_online_map & (1 << cpu)))
            continue;
        ksoftirqd_thread[cpu] = kernel_thread(ksoftirqd, (void*)(long)cpu, KT_BALANCE);
    }
}
```

---

## Task 3：修改 `kernel/interrupts/interrupts.c`

在 `do_IRQ()` 中加入 `irq_enter()` / `irq_exit()` 包裹：

```c
asmlinkage void do_IRQ(struct pt_regs *regs, long error_code)
{
    irq_enter();
    /* 原有 EOI + handler 调度逻辑 */
    irq_exit();   /* 内部自动调用 do_softirq()（若有 pending）*/
}
```

同样对 `do_apic_timer_interrupt()` 和 `do_apic_error_interrupt()` 加入 `irq_enter/irq_exit`。

在 `_init_interrupts()` 中调用 `softirq_init()`，并删除 `kernel.c` 中被注释的 `/* softirq_init(); */`。

---

## Task 4：修改 `kernel/kernel.c`

- 删除被注释的 `/* softirq_init(); */` 行（`softirq_init` 已在 `_init_interrupts()` 中调用）
- 保持其余逻辑不变

---

## Task 5：修改 `Makefile`

将 `kernel/softirq.c` 加入编译目标（OBJS 列表）。

---

## 执行顺序总结

```
1. includes/kernel/softirq.h   [新建]
2. kernel/softirq.c            [新建]
3. kernel/interrupts/interrupts.c  [修改：do_IRQ 加入 irq_enter/irq_exit]
4. kernel/kernel.c             [修改：删除注释行]
5. Makefile                    [修改：加入编译目标]
```

---

## 数据流示意

**正常路径（软中断一次处理完毕）：**
```
硬件中断
  → entry.S irq_entry_XX
  → interrupt_warpper
    → do_IRQ()
      → irq_enter()           // in_irq++
      → handler(...)          // 可调用 raise_softirq(TASKLET_SOFTIRQ)
      → irq_exit()            // in_irq--；若 in_irq==0 && pending → do_softirq()
        → do_softirq()        // pending 清零，执行完毕无残留
  → ret_from_intr
    → RESTORE_ALL → iret
```

**软中断风暴路径（handler 内反复 raise）：**
```
do_softirq() 一轮结束后
  → 检测到 softirq_pending 再次置位
  → wake_up_process(ksoftirqd_thread[cpu])
    → ksoftirqd 被调度运行（进程上下文）
      → while (pending) do_softirq()  // 持续处理直至耗尽
      → interruptible_sleep()         // 无 pending 时睡眠
```
