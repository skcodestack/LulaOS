# BSP/AP 任务执行修复

## Task 1 - slab.h：新增 kmalloc/kfree

**文件**: `includes/mm/slab.h`

新增 GFP 标志位和通用分配函数声明：
```c
#define GFP_KERNEL  0x0001
#define GFP_ATOMIC  0x0002

/* 通用大小缓存（32/64/128/256/512/1024/2048/4096/8192 字节） */
extern kmem_cache_t *malloc_caches[9];

void *kmalloc(unsigned int size, unsigned int flags);
void  kfree(const void *obj);
```

## Task 2 - slab.c：实现通用缓存池 + kmalloc/kfree

**文件**: `kernel/mm/slab.c`

- 定义 `malloc_caches[9]`，对应 32/64/128/256/512/1024/2048/4096/8192 字节
- 在 `kmem_cache_init()` 末尾创建这 9 个缓存
- 实现 `kmalloc(size, flags)`：查表找最小满足 size 的缓存，调用 `kmem_cache_alloc`
- 实现 `kfree(obj)`：通过 `virt_to_page(obj)->virtual` 反查 slab，找所属缓存 free

> **注意**：slab.c 中 kmem_cache_grow 使用 `__alloc_pages(0, 0)`（单页 4KB），
> 对于 8192B 的缓存无法用单页实现。需要让 8192 缓存使用 `__alloc_pages(1, 0)` 分配 2 页，
> 并在 kmem_cache_create 中传入 order 参数，或直接对 THREAD_SIZE 大小单独走 2 页分配路径。
> 简化方案：kmalloc(THREAD_SIZE) 特判 size==8192 时调用 `__alloc_pages(1, 0)` 返回 2 页对齐地址。

## Task 3 - sched.h：重构 task_struct / thread_union

**文件**: `includes/kernel/sched.h`

```c
// 删除旧 current 宏和 current_p[] 数组
// 新增：

#define THREAD_SIZE     8192
#define THREAD_MASK     (~(THREAD_SIZE - 1))   /* ~0x1FFF */

/* thread_union：task_struct 在低地址，内核栈在高地址向下增长 */
union thread_union {
    struct task_struct task;                        /* 位于 union 起始 */
    unsigned long stack[THREAD_SIZE / sizeof(long)]; /* 占满整个 8KB */
};

/* current 宏：esp & ~0x1FFF 得到 thread_union 基址 = task_struct 地址 */
static inline struct task_struct *get_current(void) {
    struct task_struct *cur;
    __asm__("andl %%esp, %0" : "=r"(cur) : "0"(~(THREAD_SIZE-1)));
    return cur;
}
#define current  get_current()
```

删除 `extern struct task_struct *current_p[NR_CPUS];`

- BSP 的 init_thread_union 通过 .data.init_task 节放置，8K 对齐
- init_task 独立声明为 extern（实体在 kernel.c 或 sched.c）

## Task 4 - linker.lds：新增 .data.init_task（8K 对齐）

**文件**: `linker.lds`

在 `.data` 段之前插入：
```
.data.init_task BLOCK(8K) : AT(ADDR(.data.init_task) - 0xC0000000)
{
    *(.data.init_task)
}
```

## Task 5 - sched.c：init_thread_union + init_task + 改造 sched_init/schedule

**文件**: `kernel/sched.c`

删除 `struct task_struct *current_p[NR_CPUS]`。

新增：
```c
union thread_union init_thread_union
    __attribute__((__section__(".data.init_task")))
    __attribute__((aligned(THREAD_SIZE))) = {
        .task = INIT_TASK(init_thread_union.task)
    };

struct task_struct *init_task_ptr = &init_thread_union.task;
```

改造 `sched_init`：
- 移除对 `current_p[]` 的赋值
- 每个 CPU 的 `rq->idle = &init_thread_union.task`（BSP idle）
- AP idle task 在 `smp_init` 中分配，sched_init 只处理 BSP

改造 `schedule`：
- 删除 `current_p[cpu]` 相关赋值
- `__switch_to` 中不再需要第三个 `current_p_addr` 参数（current 宏自动通过 esp 计算）

## Task 6 - sched.c：改造 sys_fork / kernel_thread

**文件**: `kernel/sched.c`

参考 Linux fork.c `dup_task_struct`：
```c
// sys_fork
union thread_union *tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
struct task_struct *p = &tu->task;
// 内核栈顶 = (unsigned long)tu + THREAD_SIZE

// kernel_thread
union thread_union *tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
struct task_struct *p = &tu->task;
```

释放时使用 `kfree(tu)`。

## Task 7 - smp.c：AP idle task 分配改造

**文件**: `arch/x86/kernel/smp.c`

参考 Linux `alloc_idle_task`：
```c
// 为每个 AP 分配独立 thread_union
union thread_union *tu = (union thread_union *)kmalloc(THREAD_SIZE, GFP_KERNEL);
struct task_struct *idle = &tu->task;
*idle = init_thread_union.task;   // 从 BSP idle 复制基本字段
idle->pid = 0;                    // idle 进程 pid=0
// 设置 AP 的 rq->idle = idle
// 内核栈顶 = (unsigned long)tu + THREAD_SIZE，写入 trampoline
```

## Task 8 - entry.S：更新偏移 + 改造 __switch_to

**文件**: `arch/x86/entry.S`

`current` 宏通过 esp 屏蔽获取，`__switch_to` 不再需要第三个参数 `current_p_addr`：
- 移除 `current_p_addr` 的 push/pop 相关代码
- 移除 `movl %edx, (%eax)` （更新 current_p 的代码）
- `ret_from_fork` 中改为用 esp 屏蔽获取 current，移除对 `current_p` 数组的引用

更新 task_struct 字段偏移注释（thread 偏移需随 task_struct 布局调整）。

## Task 9 - smp.c / kernel.c：cpu_idle 实现

**文件**: `arch/x86/kernel/smp.c`, `kernel/kernel.c`

```c
void cpu_idle(void) {
    for (;;) {
        while (!current->need_resched)
            safe_halt();
        schedule();
    }
}
```

- `start_secondary()` 末尾改为调用 `cpu_idle()`
- `kernel.c` 的 `_kernel_main` idle 循环改为调用 `cpu_idle()`

## Task 10 - entry.S：ret_from_intr 检查 need_resched

**文件**: `arch/x86/entry.S`

```asm
ENTRY(ret_from_intr)
    /* 检查 current->need_resched（偏移 16） */
    call smp_processor_id    /* 已废弃，改用 esp 屏蔽 */
    movl %esp, %eax
    andl $~(THREAD_SIZE-1), %eax   /* eax = &task_struct */
    cmpl $0, 16(%eax)              /* need_resched 偏移 16 */
    jnz  need_resched_
    jmp  RESTORE_ALL
need_resched_:
    call schedule
    jmp  RESTORE_ALL
```

## 关键数据结构对应关系

| Linux 2.6.20 | LulaOS（修改后） |
|---|---|
| `struct thread_info` (页首) + `struct task_struct` (slab单独分配) | `struct task_struct` (thread_union起始，合并) |
| `current = thread_info->task` | `current = esp & ~0x1FFF` |
| `alloc_thread_info = kmalloc(THREAD_SIZE)` | `kmalloc(THREAD_SIZE)` |
| `alloc_task_struct = kmem_cache_alloc(task_cachep)` | 直接内嵌在 thread_union，无需单独分配 |
| `init_thread_union` (.data.init_task, 8K对齐) | 同 |
| `fork_idle(cpu)` | `smp.c` 中手动分配 |
