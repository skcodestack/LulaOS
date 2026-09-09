# LulaOS 内核架构——自底向上 6 层设计文档

> 参考 Linux 2.6.20 实现 · 文档涵盖 Task 1 ~ Task 10 已落地的全部基础设施
> 适用内核：LulaOS（x86 裸机，i686-elf-gcc）

---

## 目录

| 层号 | 名称 | Task | 核心职责 |
|------|------|------|---------|
| 1 | 时间基础设施 | 1 | jiffies / udelay / sleep / gettimeofday |
| 2 | 页表 PGALLOC | 2 | 动态分配/释放页表页，支撑 vmalloc / mmap |
| 3 | Buffer Cache | 3 | 块设备统一缓存，bread / bwrite / sync |
| 4 | VFS 核心 | 4-6 | 命名解析 / inode+dentry 缓存 / rootfs+mount |
| 5 | initramfs 驱动 | 7 | 解析 cpio newc 镜像，初始化根文件系统 |
| 6 | 系统调用 | 8 | sys_open/read/write/close/ioctl 全链路 |
| 7 | 设备与 ext2 | 9-10 | devtmpfs + TTY/fb 接入 + ext2 只读挂载 |

设计原则：**自底向上构建，每层仅依赖下层接口，不允许跨层反向依赖**。

---

## Layer 1 · 时间基础设施（Task 1）

> 所有延时、超时、定时功能的基石

### 核心数据

```c
/* arch/x86/kernel/time.c */
volatile unsigned long jiffies;          /* PIT 中断递增全局计数器 */
#define HZ 100                           /* 时钟频率：100 次/秒，jiffies 每 10ms +1 */
```

### 关键实现

| 函数 | 语义 |
|------|------|
| `init_pit()` | 初始化 8254 PIT，Mode 3 方波，频率 HZ |
| `timer_interrupt()` | IRQ0 处理器，`jiffies++` |
| `udelay(usecs)` | 基于 jiffies 的忙等待微秒延时 |
| `msleep(msecs)` | 忙等待毫秒睡眠（轮询 `jiffies - start >= target`） |
| `sys_gettimeofday()` | 返回 `{tv_sec = jiffies/HZ, tv_usec = ...}` |
| `sys_select(timeout)` | 超时判断：`jiffies - start >= timeout` |

### 调用链

```
PIT IRQ0 → timer_interrupt → jiffies++
  ↓
udelay / msleep / sys_gettimeofday / sys_select
```

### 文件索引

- `includes/time.h`：`struct timeval`、`HZ`、`jiffies` 声明
- `arch/x86/kernel/time.c`：`init_pit`、`jiffies` 定义

---

## Layer 2 · 页表动态分配 PGALLOC（Task 2）

> vmalloc / ioremap / mmap 的页表底座

### 核心思想

x86 三级页表（PGD → PMD → PTE），内核启动时由 `paging_init()` 静态建立初始映射；运行时动态分配新页表页支撑 vmalloc / mmap / ioremap。

### 关键实现

```c
/* 页表分配：从 slab 或 kmalloc 取一个物理页 */
pgd_t *pgd_alloc(void);
pte_t *pte_alloc_one(void);            /* → kmalloc(PAGE_SIZE) 或 alloc_page() */

/* ioremap：物理地址 → 内核虚拟地址 */
void *ioremap(unsigned long phys, unsigned long size)
{
    /* 1. vmalloc_area 中找连续虚拟区间
     * 2. 对每页：pte = pte_alloc_one(); 设 pde->pde = pte | PRESENT|RW|USER
     * 3. 逐页建立映射：pte[i] = (phys + i*PAGE_SIZE) | PRESENT|RW
     * 4. flush_tlb() */
}

/* mmap：用户空间映射 */
sys_mmap(addr, len, prot, flags, fd, offset)
  → do_mmap(file, addr, len, prot, flags)
      → 分配虚拟区间
      → 逐页 map_page（用户态页标志含 USER 位）
```

### 文件索引

- `arch/x86/mm/pgtable.c`：pgd/pte alloc
- `arch/x86/mm/ioremap.c`：ioremap 实现
- `kernel/mm/vmalloc.c`：vmalloc 区域管理

---

## Layer 3 · Buffer Cache 块缓存（Task 3）

> 文件系统与块设备之间的统一缓存层

### 核心结构

```c
struct buffer_head {
    unsigned long b_blocknr;      /* 逻辑块号 */
    unsigned short b_dev;         /* 设备号 dev_t */
    unsigned short b_size;        /* 块大小（字节）*/
    void *b_data;                 /* 数据缓冲指针 */
    atomic_t b_count;             /* 引用计数 */
    unsigned long b_state;        /* BH_Uptodate / BH_Dirty / BH_Lock */
    struct buffer_head *b_next;   /* 哈希链 */
    struct buffer_head *b_lru_next; /* LRU 链 */
};
```

### 关键算法

```
bread(dev, block, size)
  → __getblk(dev, block, size)
      → __find_buffer(dev, block, size)    /* 全局哈希表查找 */
          命中 → atomic_inc(&bh->b_count) → 返回
          未命中 → grow_buffers()          /* 批量分配 buffer_head + 数据页 */
                   → 加入哈希 + LRU
  → ll_rw_block(READ, 1, &bh)             /* 发 ATA/SATA 命令 */
  → wait_on_buffer(bh)                     /* 同步等待 BH_Uptodate */
  → 返回 bh

bwrite(bh)
  → mark_buffer_dirty(bh)                 /* 置 BH_Dirty */
  → ll_rw_block(WRITE, 1, &bh)            /* 同步写 */
  → wait_on_buffer(bh)                    /* 等待完成 */

brelse(bh)
  → atomic_dec(&bh->b_count)
  → count==0 → 加入 LRU 头部

sync_buffers(dev)
  → 遍历所有 bh：BH_Dirty → bwrite → clear_bit(BH_Dirty)
```

### 块号 → 扇区号换算

```c
sector = block * (size >> 9)      /* size=1024 → sector=block*2
                                     size=4096 → sector=block*8 */
```

### 关键约束

- 块大小仅支持 **1024 / 4096**（grow_buffers 约束）
- bread 同步阻塞（无异步 IO 路径）
- bwrite 同步直写（每次写立即落盘）

### 文件索引

- `includes/buffer_head.h`：`struct buffer_head`、API 声明
- `kernel/fs/buffer.c`：bread / bwrite / brelse / sync_buffers

---

## Layer 4 · VFS 核心（Task 4-6）

> 统一抽象所有文件系统的公共层

### 4.1 核心数据结构

```c
/* --- 文件对象 --- */
struct file {
    struct dentry *f_dentry;
    struct vfsmount *f_vfsmnt;
    struct file_operations *f_op;
    loff_t f_pos;                   /* 当前读写位置 */
    unsigned int f_flags;           /* O_RDONLY / O_WRONLY / O_RDWR ... */
    unsigned int f_mode;            /* FMODE_READ / FMODE_WRITE */
    atomic_t f_count;
};

/* --- 目录项缓存 --- */
struct dentry {
    atomic_t d_count;               /* 引用计数 */
    struct inode *d_inode;          /* NULL = negative dentry */
    struct dentry *d_parent;
    struct list_head d_child;       /* 父目录子项链 */
    struct list_head d_subdirs;     /* 子目录列表 */
    struct qstr d_name;             /* 文件名 */
    struct super_block *d_sb;
    struct list_head d_hash;        /* 哈希桶链 */
};

/* --- inode --- */
struct inode {
    unsigned long i_ino;
    unsigned short i_mode;          /* S_IFREG / S_IFDIR / S_IFCHR ... */
    loff_t i_size;
    struct inode_operations *i_op;
    struct file_operations *i_fop;  /* 默认 file_operations */
    struct super_block *i_sb;
    atomic_t i_count;
    unsigned long i_state;          /* I_NEW / I_DIRTY */
    struct list_head i_hash;
    struct list_head i_sb_list;     /* 挂到 sb->s_inodes */
};

/* --- 文件系统类型 --- */
struct file_system_type {
    const char *name;
    int fs_flags;
    struct super_block *(*get_sb)(...);
    struct list_head fs_supers;     /* 同类型所有 sb 实例 */
};
```

### 4.2 命名解析（Task 4）

```c
path_walk(path, nd)
{
    nd->dentry = root;  nd->mnt = root_mnt;
    while (*name) {
        /* 跳过 '/' */
        /* 提取组件名 this.name / this.len */

        /* 挂载穿越：mnt_mounts 链查找匹配 dentry 的 vfsmount */
        while (nd->mnt->mnt_mounts) {
            mnt = find_mount(nd->mnt, nd->dentry);
            if (!mnt) break;
            nd->mnt = mnt;
            nd->dentry = mnt->mnt_root;
        }

        /* 查 dcache（哈希表） */
        dentry = d_lookup(parent, &this);
        if (dentry) { nd->dentry = dentry; continue; }

        /* dcache miss → 构造 negative dentry → i_op->lookup() */
        dentry = d_alloc(parent, &this);
        dentry->d_sb = parent->d_sb;
        /* 加入哈希（dentry->d_name.hash） */
        if (inode->i_op && inode->i_op->lookup)
            inode->i_op->lookup(dir_inode, dentry);
        nd->dentry = dentry;
    }
}
```

### 4.3 inode / dentry 缓存（Task 5）

```c
/* inode 缓存：全局哈希表 inode_hashtable[32] */
inode = iget(sb, ino);
  → find_inode_fast(sb, ino)       /* 哈希命中 → i_count++ */
      命中 → 返回
      未命中 → alloc_inode(sb)     /* kmalloc + INIT_LIST_HEAD + i_state=I_NEW */
                → 加入哈希表
                → sb->s_op->read_inode(inode)  /* 填充字段（可睡眠）*/
                → clear I_NEW

/* dentry 缓存：哈希表 dentry_hashtable[32]，按 name.hash 索引 */
d_lookup(parent, name)
  → hash(name) → 遍历桶
      d_name.len 匹配 && memcmp 匹配 → d_count++ → 返回

d_instantiate(dentry, inode)
  → dentry->d_inode = inode
  → atomic_inc(&inode->i_count)    /* dentry 持有 inode 一引用 */

dput(dentry)
  → d_count 归零 → 从父 d_subdirs 摘除 → kfree
      → d_inode 非 NULL → iput(d_inode)
```

### 4.4 rootfs 与 mount 机制（Task 6）

```c
/* rootfs：内存文件系统，提供 / 根目录 */
init_rootfs()
  → register_filesystem(&rootfs_fs_type)
  → do_kern_mount("rootfs", 0)
      → ramfs_get_sb()
          → 分配 super_block
          → fill_super：创建 root inode (S_IFDIR) + root dentry
      → mnt->mnt_root = mnt->mnt_sb->s_root
  → set_fs_root(current->fs, mnt->mnt_root, mnt)

/* mount 机制 */
struct super_block {
    unsigned long s_blocksize;
    void *s_fs_info;                /* 文件系统私有数据（ext2_sb_info 等）*/
    struct super_operations *s_op;
    struct list_head s_inodes;      /* 该 sb 的所有 inode */
    struct list_head s_instances;   /* 同 fs_type 的所有 sb */
    struct dentry *s_root;
};

do_kern_mount(dev_name, flags, fs_type)
  → fs_type->get_sb(dev_name, flags)
      → path_walk(dev_name, &nd)   /* 块设备路径解析 */
      → alloc_super()
      → fill_super(sb, data, flags)
          → 读 superblock → 分配 root inode → 创建 root dentry
  → 加入 fs_type->fs_supers
  → 返回 vfsmount

/* 公共底座 */
vfs_mknod(dir, name, mode, rdev)    /* sys_mknod 与 devtmpfs 共用 */
vfs_mkdir(dir, name, mode)          /* sys_mkdir 与挂载点准备共用 */
  → 查 dcache 存在性（含 negative → -EEXIST）
  → d_alloc 构造 negative dentry
  → dir->i_op->mknod / mkdir()
  → 失败：d_drop + dput 回滚
```

### 文件索引

| 模块 | 文件 |
|------|------|
| 命名解析 | `kernel/fs/namei.c` |
| inode/dentry 缓存 | `kernel/fs/inode.c`、`kernel/fs/dcache.c` |
| rootfs + ramfs | `kernel/fs/rootfs.c`、`kernel/fs/ramfs.c` |
| mount 机制 | `kernel/fs/namespace.c` |
| 文件系统注册 | `kernel/fs/filesystems.c` |
| 设备节点创建 | `kernel/fs/devices.c` |

---

## Layer 5 · initramfs 驱动（Task 7）

> 系统早期根文件系统初始化

### 镜像格式：cpio newc（`070701`）

```
每个文件项：
  struct cpio_newc_header {   /* 110 字节 ASCII hex */
      char c_magic[6];        /* "070701" */
      char c_ino[8];
      char c_mode[8];
      char c_namesize[8];
      char c_filesize[8];
      ...
  };
  文件名（c_namesize 字节，含 NUL）
  4 字节对齐填充
  文件内容（c_filesize 字节）
  4 字节对齐填充
  结束标志：文件名 "TRAILER!!!"
```

### 解析流程

```c
populate_rootfs(initrd_start, initrd_end)
{
    验证 header->c_magic == "070701"
    循环：
        读取 c_mode、c_namesize、c_filesize
        提取文件名（NUL 终止）
        if (name == "TRAILER!!!") break

        if (S_ISDIR(mode))
            vfs_mkdir(nd.dentry, name, mode)
        else if (S_ISCHR(mode) || S_ISBLK(mode))
            vfs_mknod(nd.dentry, name, mode, MKDEV(major, minor))
        else if (S_ISREG(mode)) {
            vfs_mknod(nd.dentry, name, S_IFREG | mode, 0)
            /* 将内容写入 page cache（当前简化：直接拷贝到 inode 缓冲）*/
        }

        推进指针（header + name + padding + data + padding）
    释放 initrd 镜像内存
}
```

### 文件索引

- `kernel/fs/initramfs.c`：`populate_rootfs()`
- `kernel/kernel.c`：`initrd_start / initrd_end` 由 boot 模块设置

---

## Layer 6 · 系统调用（Task 8）

> 用户态访问 VFS 的唯一入口

### 核心系统调用

#### sys_open

```c
sys_open(path, flags, mode)
{
    /* 1. 从用户空间拷贝路径 */
    name = getname(path);           /* copy_from_user + NUL 终止 */

    /* 2. 路径解析 */
    err = path_walk(name, &nd);     /* Layer 4 命名解析 */

    /* 3. negative dentry → 文件不存在 */
    if (!nd.dentry->d_inode) {
        if (!(flags & O_CREAT)) return -ENOENT;
        /* TODO: O_CREAT 路径（Task 11） */
    }

    /* 4. 权限校验 */
    may_open(nd, flags)             /* FMODE_READ/WRITE 映射 */

    /* 5. 组装 file 对象 */
    f = dentry_open(nd.dentry, nd.mnt, flags)
        → f->f_op = inode->i_fop    /* 取 inode 默认 f_op */
        → f->f_mode = (flags+1) & O_ACCMODE
        → f->f_pos = 0

    /* 6. 分配 fd 并安装 */
    fd = get_unused_fd()            /* 从 0 扫描 current->files->fd[] */
    current->files->fd[fd] = f
    return fd
}
```

#### sys_read / sys_write

```c
sys_read(fd, buf, count)
{
    f = current->files->fd[fd]
    if (!(f->f_mode & FMODE_READ)) return -EBADF
    if (!f->f_op->read) return -EINVAL
    ret = f->f_op->read(f, buf, count, &pos)   /* pos = &f->f_pos */
    return ret
}

sys_write(fd, buf, count)
{
    f = current->files->fd[fd]
    if (!(f->f_mode & FMODE_WRITE)) return -EBADF
    if (f->f_flags & O_APPEND) f->f_pos = inode->i_size
    ret = f->f_op->write(f, buf, count, &pos)
    return ret
}
```

#### 其他系统调用

| 调用 | 实现要点 |
|------|---------|
| `sys_close(fd)` | `fd[fd]=NULL` → `fput(f)` → `f_count==0 → iput` |
| `sys_lseek(fd, off, whence)` | `SEEK_SET/CUR/END`，pos<0 → `-EINVAL` |
| `sys_ioctl(fd, cmd, arg)` | 转发 `f->f_op->ioctl(f, cmd, arg)` |
| `sys_fstat(fd, statbuf)` | `inode = f->f_dentry->d_inode` → 拷各字段 |
| `sys_getdents(fd, dirent, count)` | `f->f_op->readdir(f, buf, filldir)` |
| `sys_mknod(path, mode, dev)` | `path_walk` 父目录 → `vfs_mknod` |
| `sys_mount(dev, dir, type, flags)` | 查 `file_systems` 表 → `do_kern_mount` |

### 关键设计

- `f_mode = (flags + 1) & O_ACCMODE`：O_RDONLY(0)→FMODE_READ，O_WRONLY(1)→FMODE_WRITE，O_RDWR(2)→FMODE_BOTH
- fd 分配：从 0 扫描 `current->files->fd[]` 找第一个 NULL 槽
- sys_close 先置 NULL 再 fput（防 fput→iput 链中重入 current->files）
- `getname(path)` 从用户空间拷路径串（KERNEL_DS 下放行内核缓冲）

### 文件索引

- `kernel/syscall.c`：所有系统调用实现、`sys_call` 分发入口
- `kernel/fs/open.c`：`may_open`、`dentry_open`

---

## Layer 7 · 设备管理与 ext2 只读挂载（Task 9-10）

> 真实设备接入 + 磁盘文件系统读取

### 7.1 devtmpfs（Task 9）

基于 ramfs，在 `/dev` 下自动管理设备节点。

```c
/* 公共注册接口 */
int devtmpfs_register(dev_t dev, const char *name,
                      mode_t mode, struct file_operations *fops);
int devtmpfs_unregister(dev_t dev, const char *name);
int devtmpfs_create_node(const char *name, mode_t mode, dev_t dev);

/* 初始化 */
devtmpfs_mount()
{
    vfs_mkdir(current->fs->root, "dev", 0755);   /* 创建 /dev 目录 */
    register_filesystem(&devtmpfs_fs_type);
    do_kern_mount("devtmpfs", 0, &devtmpfs_fs_type);
    /* 挂载到 /dev 路径 */
}
```

### 7.2 /dev/console 与键盘输入（Task 9）

**环形输入缓冲（1024B）：**

```c
#define CONSOLE_IN_BUF_SIZE 1024
static char console_in_buf[CONSOLE_IN_BUF_SIZE];
static unsigned int console_in_head, console_in_tail;
static spinlock_t console_in_lock;
static wait_queue_head_t console_in_wait;

/* 中断注入：键盘 handler 调用 */
console_input_putchar(c)
{
    spin_lock_irqsave(&console_in_lock, flags);
    next = (head + 1) % SIZE;
    if (next == tail) tail = (tail+1) % SIZE;   /* 满时挤掉最旧 */
    buf[head] = c;  head = next;
    wakeup = waitqueue_active(&console_in_wait);
    spin_unlock_irqrestore(...);
    if (wakeup) wake_up(&console_in_wait);       /* 锁外唤醒，防死锁 */
}

/* 用户读取：阻塞等待 + 防丢失唤醒 double-check */
console_read(file, buf, count, ppos)
{
    for (read = 0; read < count; ) {
        c = console_in_getchar();
        if (c < 0) {
            if (file->f_flags & O_NONBLOCK) return read ?: -EAGAIN;
            if (read) return read;
            /* 三段式等待 + double-check */
            prepare_to_wait(&console_in_wait, &wait);
            c = console_in_getchar();            /* 锁前再查一次 */
            if (c < 0) schedule();
            finish_wait(&console_in_wait, &wait);
            c = console_in_getchar();
            if (c < 0) return read;
        }
        buf[read++] = c;
    }
}
```

**键盘输入注入**（`kernel/keyboard.c`）：

```c
/* 中断 handler：扫描码 → ASCII → 双路输出 */
if (ch != '\0') {
    console_input_putchar(ch);      /* 1. 注入 /dev/console 输入缓冲 */
    printk("%c", ch);               /* 2. 本地回显到屏幕 */
}
```

### 7.3 /dev/fb0（Task 9）

```c
/* 直接访问线性帧缓冲显存 */
fb_read/fb_write:
    offset = file->f_pos
    fb_virt = fb.virt_addr + offset    /* 物理 LFB 的 ioremap 虚拟地址 */
    copy_to_user / copy_from_user
    f_pos += copied

fb_open:
    if (!fb.active || !fb.virt_addr) return -ENODEV;

fb_dev_init:
    devtmpfs_register(MKDEV(FB_MAJOR, 0), "fb0", S_IFCHR|0600, &fb_fops);
```

### 7.4 ext2 只读挂载（Task 10）

**磁盘布局（64MB LulaOS.img）：**

```
[0 - 2048 sectors]    1MB boot area（MBR + GRUB core.img）
[2048 - 131072]       63MB ext2 分区（hd0p1 = MKDEV(3, 1)）
```

**挂载链路：**

```c
init_ext2_fs()
  → register_filesystem(&ext2_fs_type)

do_mount("/dev/hd0p1", "/mnt", "ext2", MS_RDONLY, NULL)
  → path_walk("/mnt", &nd)
  → ext2_get_sb(fs_type, flags, dev_name)
      → ext2_lookup_bdev("/dev/hd0p1")
          → path_walk → 取 inode->i_rdev（块设备号）
      → kmalloc(sizeof(ext2_sb_info))
      → ext2_fill_super(sb)
          → bread(dev, 1, 1024)                /* 1K 块读 superblock */
          → 验证 magic == 0xEF53
          → 若 block_size != 1024 → 重读块 0
          → 计算 groups_count
          → 读取组描述符表（常驻内存）
          → iget(sb, EXT2_ROOT_INO=2)
              → ext2_read_inode
          → d_alloc(NULL, "/") + d_instantiate
      → 加入 fs_type->fs_supers
  → graft_tree(mnt, nd)                        /* 挂到 /mnt 挂载点 */
```

**ext2 核心数据结构：**

```c
/* 内存 superblock 信息 */
struct ext2_sb_info {
    unsigned long s_block_size;
    unsigned long s_inodes_per_group;
    unsigned long s_blocks_per_group;
    unsigned long s_groups_count;
    struct buffer_head **s_group_desc;    /* 组描述符块缓冲数组 */
    struct buffer_head *s_sbh;            /* superblock 常驻引用 */
    struct ext2_super_block s_es;         /* superblock 内存副本 */
};

/* 内存 inode 信息 */
struct ext2_inode_info {
    __u32 i_data[15];                     /* 块指针：12 直接 + IND + DIND + TIND */
    __u32 i_flags;
};
```

**ext2_read_inode 关键步骤：**

```
1. ino 校验：保留 inode（<11 且 !=2）→ 拒绝
2. 定位组：group = (ino - 1) / inodes_per_group
3. 读组描述符：ext2_get_group_desc(sb, group) → bg_inode_table
4. 计算块号：block = table + (index % per_block) / inode_size
5. bread → 读取 raw inode → 填 i_mode / i_size / i_nlink / i_data
6. 按类型装配 fops：
     S_IFREG → ext2_file_operations
     S_IFDIR → ext2_dir_operations + ext2_dir_inode_operations
     S_IFCHR/BLK → i_rdev = i_data[0]
7. 失败：i_mode = 0（调用方检测视为不存在）
```

**ext2 块映射 ext2_bmap（三级间接）：**

```c
ext2_bmap(inode, block)
{
    if (block < 12)
        return ei->i_data[block];              /* 直接块 */

    if (block < 12 + apb) {                    /* 一级间接 */
        phys = ei->i_data[EXT2_IND_BLOCK];
        if (!phys) return 0;                   /* 空洞 */
        bh = bread(dev, phys, block_size);
        return ((__u32 *)bh->b_data)[block - 12];
    }

    if (block < 12 + apb + apb*apb) {          /* 二级间接 */
        /* 两级 bread */
    }

    /* 三级间接：三级嵌套 bread */
}
```

**ext2_readdir 对齐规则：**

```c
ext2_readdir(file, dirent, filldir)
{
    pos = file->f_pos;                          /* 目录内字节偏移 */
    while (pos < inode->i_size) {
        phys = ext2_bmap(inode, pos / block_size);
        bh = bread(dev, phys, block_size);
        de = (ext2_dir_entry_2 *)(bh->b_data + pos % block_size);

        while (offset < block_size && pos < i_size) {
            if (de->rec_len < 8) break;         /* 坏目录项 */
            if (de->inode) {
                over = filldirent(de->name, de->name_len, de->inode);
                if (over) break;                /* 缓冲满：不对齐，直接退出 */
                ret++;
            }
            pos += de->rec_len;
            de = (ext2_dir_entry_2 *)((char *)de + de->rec_len);
        }

        if (over) break;                        /* 不对齐，下次从当前位置继续 */
        pos += block_size - (pos % block_size); /* 坏目录项：对齐到块边界 */
    }
    file->f_pos = pos;
    return ret;
}
```

### 文件索引

| 模块 | 文件 |
|------|------|
| devtmpfs | `kernel/fs/devtmpfs.c` |
| /dev/console | `kernel/tty/console.c` |
| /dev/fb0 | `kernel/video/fb_dev.c` |
| ext2 头文件 | `includes/ext2_fs.h` |
| ext2 挂载 | `kernel/ext2/super.c` |
| ext2 inode | `kernel/ext2/inode.c` |
| ext2 namei | `kernel/ext2/namei.c` |

---

## 跨层调用全景图

```
用户程序
  │
  ▼ sys_open / sys_read / sys_write ...          ← Layer 6
  │
  ▼ path_walk → d_lookup → i_op->lookup          ← Layer 4 (VFS)
  │
  ▼ ext2_lookup / ext2_readdir
  │    └→ ext2_bmap → bread(dev, block, 1024)    ← Layer 3 (Buffer Cache)
  │              └→ ll_rw_block(READ/WRITE, ...)
  │                    └→ sata_read/write         ← 块设备驱动
  │
  ▼ ext2_fill_super → iget(ROOT_INO)
  │
  └→ do_mount → path_walk("/dev/hd0p1")
        └→ devtmpfs 自动创建节点                   ← Layer 7

内核线程
  │
  ▼ populate_rootfs(initrd)                       ← Layer 5
  │    └→ vfs_mkdir / vfs_mknod（ramfs）
  │
  ▼ jiffies → udelay / msleep                     ← Layer 1
  │
  ▼ ioremap → pte_alloc_one                       ← Layer 2
```

---

## 启动初始化顺序（kernel.c 接线）

```c
_kernel_main()
  → init_cpu() / setup_arch() / _init_idt() / _init_interrupts()
  → sched_init() / mm_init() / kmem_cache_init() / vmalloc_area_init()
  → bsp_start_idle()
      └→ start_kernel()        /* Task 7-10 接线代码在此函数 */
          → platform_bus_init() / acpi_device_init()
          → init_drivers()（PCI/ATA/USB/IDE/SATA）
          → init_rootfs() + populate_rootfs()         /* Layer 5 */
          → ksys_open 自测（/motd）                    /* Layer 6 验证 */
          → devtmpfs_mount() + console_init()          /* Layer 7 TTY */
              + fb_dev_init()                          /* Layer 7 fb */
          → init_ext2_fs()                             /* Layer 7 ext2 */
          → vfs_mkdir("/mnt")
          → devtmpfs_create_node("hd0p1", S_IFBLK)
          → do_mount("/dev/hd0p1", "/mnt", "ext2", MS_RDONLY)
          → 读 /mnt/boot/grub/grub.cfg 验证
          → sti() → cpu_idle()
```
