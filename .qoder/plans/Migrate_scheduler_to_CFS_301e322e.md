# CFS 调度器改造计划

## 目标
将当前基于链表、时间片轮转的调度器改造成类 Linux 2.6 CFS（完全公平调度器）的简化实现。

## 改造范围
涉及文件：
- `includes/kernel/sched.h`
- `kernel/sched.c`
- 新增 `includes/libs/rbtree.h`
- 新增 `kernel/libs/rbtree.c`
- 可能涉及 `arch/x86/entry.S`（`need_resched` 偏移不变，但需确认 `task_struct` 布局变化不影响汇编）

---

## Task 1: 引入红黑树基础设施

新增 `includes/libs/rbtree.h`：
- 定义 `struct rb_node`（color, parent, left, right）
- 定义 `struct rb_root`
- 提供 `rb_link_node()`, `rb_insert_color()`, `rb_erase()`, `rb_first()`, `rb_next()`, `rb_prev()` 等接口
- 参考 Linux 2.6 `include/linux/rbtree.h`

新增 `kernel/libs/rbtree.c`：
- 实现红黑树旋转、插入平衡、删除平衡
- 参考 Linux 2.6 `lib/rbtree.c`

验证：单独写一个小型用户态测试验证 rb-tree 的 insert/erase/first 正确性。

---

## Task 2: 定义调度实体与 CFS 运行队列

### 2.1 `struct sched_entity`
在 `includes/kernel/sched.h` 中新增：

```c
struct sched_entity {
    struct rb_node      run_node;       /* CFS 红黑树节点 */
    unsigned long       vruntime;       /* 虚拟运行时间 */
    unsigned long       exec_start;     /* 本次运行起始时间 */
    unsigned long       sum_exec_runtime; /* 总运行时间 */
    unsigned long       load_weight;    /* 优先级权重 */
};
```

### 2.2 改造 `struct task_struct`
在 `task_struct` 中新增：

```c
struct sched_entity se;
```

保留：
- `state`, `pid`, `comm`
- `thread`（上下文）
- `flags`（PF_*）
- `pt_regs`

移除/重构：
- `counter` 和 `timeslice` 字段可以移除，CFS 用 `vruntime` 替代
- `priority` 可保留作为静态优先级，用于计算 `load_weight`
- `run_list` 移除，改用 `se.run_node`
- `need_resched` 保留（汇编路径依赖）

### 2.3 改造 `runqueue_t`
新增 `struct cfs_rq`：

```c
struct cfs_rq {
    struct rb_root      tasks_timeline; /* 红黑树根 */
    struct rb_node     *rb_leftmost;    /* 缓存最左节点，O(1) 取下一个任务 */
    struct task_struct *curr;           /* 当前正在运行的任务 */
    struct task_struct *idle;           /* idle 任务 */
    unsigned long       min_vruntime;   /* 队列最小 vruntime */
    unsigned int        nr_running;     /* 可运行任务数 */
    unsigned long       exec_clock;     /* 队列物理运行时钟 */
};
```

`runqueue_t` 简化为：

```c
typedef struct {
    spinlock_t  lock;
    struct cfs_rq cfs;
} runqueue_t;
```

注意：SMP 负载均衡需要访问其他 CPU 的 `cfs_rq`，需加锁保护。

---

## Task 3: 替换运行队列入队/出队操作

### 3.1 替换 `add_task_to_runqueue()` / `add_task_to_cpu()`
实现 `enqueue_task_fair(struct task_struct *p, struct cfs_rq *cfs_rq)`：
- 将 `p->se` 按 `vruntime` 插入 `cfs_rq->tasks_timeline`
- 更新 `rb_leftmost` 缓存
- `cfs_rq->nr_running++`

### 3.2 替换 `del_task_from_runqueue()`
实现 `dequeue_task_fair(struct task_struct *p, struct cfs_rq *cfs_rq)`：
- 从红黑树中删除 `p->se.run_node`
- 更新 `rb_leftmost`
- `cfs_rq->nr_running--`

### 3.3 实现 `place_entity()`
任务唤醒时 placement：
- 若使用 `sysctl_sched_child_runs_first` 风格，子进程 vruntime = parent vruntime
- 普通唤醒：`se->vruntime = max(se->vruntime, cfs_rq->min_vruntime - sysctl_sched_latency / 2)`
- 简化版：唤醒时 `se->vruntime = cfs_rq->min_vruntime`

---

## Task 4: 实现 CFS 核心调度逻辑

### 4.1 `update_curr(struct cfs_rq *cfs_rq)`
在 `scheduler_tick()` 中调用：
- `delta_exec = now - cfs_rq->curr->se.exec_start`
- `cfs_rq->curr->se.sum_exec_runtime += delta_exec`
- `cfs_rq->exec_clock += delta_exec`
- `cfs_rq->curr->se.vruntime += calc_delta_fair(delta_exec, &cfs_rq->curr->se)`
- 更新 `cfs_rq->min_vruntime`

### 4.2 `calc_delta_fair(delta, se)`
简化权重计算：
- `delta_fair = delta * NICE_0_LOAD / se->load_weight`
- LulaOS 可简化：直接用 `(delta * 1024) / load_weight`

### 4.3 `pick_next_task_fair(struct cfs_rq *cfs_rq)`
- 取 `cfs_rq->rb_leftmost`
- 通过 `rb_entry(node, struct sched_entity, run_node)` 得到 `se`
- 通过 `container_of(se, struct task_struct, se)` 得到 `task`
- 更新 `cfs_rq->curr = task`
- 更新 `task->se.exec_start = now`

### 4.4 `check_preempt_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)`
- 计算当前任务理想运行时间：`ideal_runtime = sysctl_sched_latency / cfs_rq->nr_running`
- 若 `curr->sum_exec_runtime - curr->exec_start >= ideal_runtime`，设置 `need_resched`

---

## Task 5: 改造 `schedule()` 主流程

```c
void schedule(void)
{
    struct task_struct *prev = current, *next;
    struct cfs_rq *cfs_rq = &runqueues[smp_processor_id()].cfs;
    unsigned long flags;

    if (prev->state != TASK_RUNNING) {
        /* 睡眠/退出任务不再入队 */
    } else {
        enqueue_task_fair(prev, cfs_rq);
    }

    next = pick_next_task_fair(cfs_rq);
    if (!next)
        next = cfs_rq->idle;

    if (next != prev)
        context_switch(prev, next);

    clear_need_resched(prev);
}
```

注意：退出任务（`do_exit`）和睡眠任务（`interruptible_sleep`）不要重新入队。

---

## Task 6: 改造 `scheduler_tick()`

```c
void scheduler_tick(void)
{
    struct cfs_rq *cfs_rq = &runqueues[smp_processor_id()].cfs;
    struct task_struct *curr = cfs_rq->curr;

    if (curr == cfs_rq->idle)
        return;

    update_curr(cfs_rq);
    check_preempt_tick(cfs_rq, &curr->se);
}
```

移除旧的 `counter--` 和 `counter==0` 逻辑。

---

## Task 7: 改造任务创建与退出

### 7.1 `kernel_thread()` / `sys_fork()`
- 初始化 `p->se`：
  - `se.vruntime = 0`
  - `se.sum_exec_runtime = 0`
  - `se.load_weight = prio_to_weight[p->priority]`
  - `INIT_RB_NODE(&se.run_node)`
- 调用 `place_entity()` 决定入队 vruntime
- 调用 `enqueue_task_fair(p, cfs_rq)` 加入目标 CPU

### 7.2 `do_exit()`
- 从 `cfs_rq` 中删除（如果仍在队中）
- 更新 `nr_running`
- 设置 `need_resched`，触发调度

### 7.3 `sched_init()`
- 初始化每个 CPU 的 `runqueue.lock`
- 初始化 `cfs_rq.tasks_timeline = RB_ROOT`
- 设置 `cfs_rq.idle = &init_task`
- 将 `init_task` 入队到 BSP 的 cfs_rq

---

## Task 8: SMP 与负载均衡适配

当前已有 `find_idlest_cpu()` 和 `add_task_to_cpu()`。

### 8.1 修改 `find_idlest_cpu()`
改为比较各 CPU `cfs_rq.nr_running`，返回任务数最少的 CPU。

### 8.2 修改 `add_task_to_cpu()`
- 获取目标 CPU 的 `cfs_rq`
- 上 `cfs_rq` 的锁
- 调用 `place_entity()` 和 `enqueue_task_fair()`
- 设置目标 CPU idle 的 `need_resched`

### 8.3 迁移任务时的 vruntime 归一化
当任务从一个 CPU 迁移到另一个 CPU 时：
- `se->vruntime -= src_cfs_rq->min_vruntime`
- `se->vruntime += dst_cfs_rq->min_vruntime`
- 简化版：直接 `se->vruntime = dst_cfs_rq->min_vruntime`

---

## Task 9: 验证与测试

### 9.1 静态检查
- 确认 `task_struct` 中 `need_resched` 的偏移仍为 16（`0x10`），否则需同步更新 `entry.S`
- 确认 `thread.esp`, `thread.eip`, `thread.pgd`, `flags` 偏移未变

### 9.2 编译
```bash
make clean
make all-debug
```

### 9.3 功能测试
- 单核：BSP 上 `kernel_thread(test_thread_a, ...)` 和 `test_thread_b` 能交替运行
- SMP：AP 上线后，创建的任务能均衡分布到不同 CPU
- 长时间运行：无饿死、无重复 PID、无 Trap 6

### 9.4 反汇编检查
- `schedule()` 和 `scheduler_tick()` 不再依赖旧 `counter`/`timeslice`
- `pick_next_task_fair()` 正确读取 `rb_leftmost`

---

## 可选简化
若全部实现工作量过大，可分两阶段：
1. **第一阶段**：保留优先级权重，仅将链表换成红黑树 + vruntime，不实现复杂 wakeup placement
2. **第二阶段**：加入 `min_vruntime` 归一化、迁移补偿、更精细的抢占检查