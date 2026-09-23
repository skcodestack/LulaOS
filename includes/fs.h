/*
 * fs.h - LulaOS VFS 核心对象与操作表
 *
 * 参考：
 *   include/linux/fs.h        — super_block / inode / file / 三张操作表
 *                               file_system_type / files_struct / fs_struct
 *   include/linux/dcache.h    — dentry / qstr
 *   include/linux/stat.h      — 文件类型与权限位常量
 *
 * 四大对象职责（字段较 Linux 大幅简化）：
 *   super_block — 已挂载文件系统实例（设备/块大小/魔数 + s_op 表）
 *   inode       — 文件元数据节点（权限/大小/时间戳 + i_op/i_fop 表）
 *   dentry      — 路径分量缓存（父子链 + dentry 哈希，构成目录树）
 *   file        — 已打开文件实例（f_pos 读写游标 + f_op 表）
 *
 * 三张操作表（函数指针，未实现的成员置 NULL，调用方判空）：
 *   super_operations  — sb 生命周期与 inode 分配/回写/回收
 *   inode_operations  — 目录项操作（lookup/create/mkdir/unlink...）
 *   file_operations   — 文件 I/O（read/write/llseek/readdir/ioctl）
 *
 * 缓存与实现文件：
 *   kernel/fs/inode.c      — inode 哈希缓存（iget_locked/iput）
 *   kernel/fs/dcache.c     — dentry 哈希 + LRU（d_alloc/d_lookup/dput）
 *   kernel/fs/file_table.c — file 结构池（get_empty_filp/fput）
 *                             + files/fs_struct 引用计数
 *                             + 文件系统类型注册（register_filesystem）
 *   kernel/fs/selftest.c   — 启动期引用计数闭环自测
 */

#ifndef __LULA_FS_H__
#define __LULA_FS_H__

#include <arch/linkage.h>       /* __init */
#include <libs/list.h>
#include <arch/x86/atomic.h>
#include <time.h>               /* timespec, get_seconds */
#include <block/blkdev.h>       /* dev_t, struct block_device */

/*
 * __user — 用户态指针标注（Linux 地址空间隔离注解）。
 * LulaOS 无独立地址空间审计，空定义占位；Task 8+ 的 read/write
 * 路径经 uaccess.h 校验，此注解仅保留 API 形状。
 */
#ifndef __user
#define __user
#endif

/* ======================== 文件类型与权限位 ======================== */
/* 值同 Linux include/linux/stat.h（八进制） */

#define S_IFMT   00170000   /* 文件类型掩码 */
#define S_IFSOCK 01400000   /* socket */
#define S_IFLNK  01200000   /* 符号链接 */
#define S_IFREG  01000000   /* 普通文件 */
#define S_IFBLK  00600000   /* 块设备 */
#define S_IFDIR  00400000   /* 目录 */
#define S_IFCHR  00200000   /* 字符设备 */
#define S_IFIFO  00100000   /* 管道 */

#define S_ISUID  0004000    /* set-uid */
#define S_ISGID  0002000    /* set-gid */
#define S_ISVTX  0001000    /* sticky */

#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* 属主/属组/其他读写执行位 */
#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

/* inode 权限检查位（permission() 用，Task 8 路径解析接入） */
#define MAY_READ   1
#define MAY_WRITE  2
#define MAY_EXEC   4

/* ======================== 前向声明 ======================== */

struct super_block;
struct inode;
struct dentry;
struct file;
struct vfsmount;        /* 挂载描述（Task 9 namespace.c 完整定义） */
struct nameidata;       /* 路径解析上下文（Task 8 namei.c 完整定义） */

/* ======================== qstr（dentry 名字） ======================== */

/*
 * qstr - 带哈希的字符串（dentry 名字）
 *
 * name 由 d_alloc 单独 kmalloc(len+1)（含 '\0'），
 * dentry 销毁时 kfree。hash 由 full_name_hash() 预计算。
 */
struct qstr {
    unsigned int    len;
    unsigned int    hash;
    const char     *name;
};

/* ======================== kstatfs（statfs 结果） ======================== */

/*
 * kstatfs - 文件系统统计信息
 *
 * 参考 Linux include/linux/statfs.h（仅保留 ext2 statfs 所需字段）。
 */
struct kstatfs {
    long            f_type;     /* 文件系统魔数 */
    unsigned long   f_bsize;    /* 块大小（字节） */
    unsigned long   f_blocks;   /* 总块数 */
    unsigned long   f_bfree;    /* 空闲块数 */
    unsigned long   f_bavail;   /* 非保留空闲块数 */
    unsigned long   f_files;    /* 总 inode 数 */
    unsigned long   f_ffree;    /* 空闲 inode 数 */
};

/* ======================== filldir_t（readdir 回调） ======================== */

/*
 * filldir_t - getdents/readdir 的目录项填充回调
 *
 * readdir 实现对每个目录项调用一次，把 (名字, inode 号) 填入调用方缓冲。
 */
typedef int (*filldir_t)(void *buf, const char *name, int namlen,
                         unsigned long offset, unsigned long ino,
                         unsigned int d_type);

/* ======================== 三张操作表 ======================== */

/*
 * super_operations - 超级块操作表
 *
 * 生命周期：get_sb 填充 → ... → put_super 释放
 * inode 侧：alloc_inode/destroy_inode 可覆盖默认缓存行为
 *           （Task 6 统一走 inode_cache，ext2 用 i_private 挂私有数据）；
 *           write_inode 为脏 inode 回写（Task 11 ext2）；
 *           delete_inode 在 nlink=0 且引用归零时调用（释放磁盘资源）。
 */
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *inode);
    void (*write_inode)(struct inode *inode, int sync);
    void (*delete_inode)(struct inode *inode);
    int  (*statfs)(struct super_block *sb, struct kstatfs *buf);
    void (*put_super)(struct super_block *sb);
};

/*
 * inode_operations - inode 操作表（目录项类操作）
 *
 * lookup 为目录 inode 的核心回调：按 dentry 名字查找子项，
 * 未命中返回 NULL（调用方将 dentry 留作 negative 缓存）。
 * create/mkdir/mknod/symlink 供 ramfs/ext2 创建路径（Task 7/11）。
 */
struct inode_operations {
    int (*create)(struct inode *dir, struct dentry *dentry,
                  int mode, struct nameidata *nd);
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);
    int (*mkdir)(struct inode *dir, struct dentry *dentry, int mode);
    int (*rmdir)(struct inode *dir, struct dentry *dentry);
    int (*mknod)(struct inode *dir, struct dentry *dentry,
                 int mode, dev_t rdev);
    int (*symlink)(struct inode *dir, struct dentry *dentry,
                   const char *symname);
    int (*unlink)(struct inode *dir, struct dentry *dentry);
    int (*rename)(struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry);
    int (*truncate)(struct inode *inode);
};

/*
 * file_operations - 文件操作表
 *
 * read/write 返回传输字节数或负错误码；offset 参数由调用方
 * 传 &file->f_pos（实现内部更新游标）。
 * ioctl 携带 inode（字符设备按 i_rdev 分发，Task 9）。
 * release 在最后一个引用 fput 归零时调用（close 语义）。
 */
struct file_operations {
    long (*llseek)(struct file *file, long offset, int origin);
    long (*read)(struct file *file, char __user *buf,
                 unsigned long count, long *offset);
    long (*write)(struct file *file, const char __user *buf,
                  unsigned long count, long *offset);
    int  (*readdir)(struct file *file, void *dirent, filldir_t filldir);
    int  (*ioctl)(struct inode *inode, struct file *file,
                  unsigned int cmd, unsigned long arg);
    int  (*open)(struct inode *inode, struct file *file);
    int  (*release)(struct inode *inode, struct file *file);
};

/* ======================== super_block ======================== */

/*
 * super_block - 已挂载文件系统实例
 *
 * s_inodes：本文件系统全部 inode（含 unused 驻留）链表头，
 *           iget_locked 挂入，销毁时摘除。
 * s_root：  根目录 dentry（get_sb 填充，指向挂载点）。
 * s_fs_info：文件系统私有数据（ext2_sb_info 等）。
 */
struct super_block {
    struct list_head        s_list;       /* 链入全局 super_blocks */
    struct list_head        s_inodes;     /* 本 sb 的 inode 链头 */
    dev_t                   s_dev;        /* 设备号（匿名/内存 fs 为 0） */
    struct block_device    *s_bdev;       /* 底层块设备（ext2 用） */
    unsigned long           s_blocksize;  /* 逻辑块大小（字节） */
    unsigned long           s_magic;      /* 文件系统魔数 */
    unsigned long           s_flags;      /* 挂载标志 */
    atomic_t                s_count;      /* 引用计数 */
    struct dentry          *s_root;       /* 根 dentry */
    struct super_operations *s_op;        /* 超级块操作表 */
    struct file_system_type *s_type;      /* 所属文件系统类型 */
    void                   *s_fs_info;    /* 文件系统私有数据 */
};

/* ======================== inode ======================== */

/* inode 状态位：I_NEW = 刚由 iget_locked 创建，调用方填充中，不可复用 */
#define I_NEW           0x01

/*
 * inode - 文件元数据节点
 *
 * 四条链：
 *   i_hash    — inode_hashtable 桶链（sb+ino 定位，Task 6 inode.c）
 *   i_sb_list — 链入 i_sb->s_inodes
 *   i_list    — inode_in_use（i_count>0）或 inode_unused（=0 驻留）
 *   i_dentry  — 引用本 inode 的 dentry 集合链头（d_alias 挂入）
 *
 * 生命周期：iget_locked 分配/命中（i_count++）→ 调用方填元数据 →
 * unlock_new_inode 清 I_NEW → ... → iput 归零：
 *   nlink>0  → 挂 inode_unused 缓存驻留（复用走哈希命中）
 *   nlink==0 → 销毁（先回调 s_op->delete_inode 释放磁盘资源）
 */
struct inode {
    struct list_head        i_hash;
    struct list_head        i_sb_list;
    struct list_head        i_list;
    struct list_head        i_dentry;

    atomic_t                i_count;      /* 引用计数 */
    struct super_block     *i_sb;
    unsigned long           i_ino;        /* inode 号（文件系统内唯一） */
    unsigned int            i_nlink;      /* 硬链接数（0 = 无名可销毁） */
    unsigned int            i_mode;       /* 文件类型 + 权限位 */
    unsigned int            i_uid;
    unsigned int            i_gid;
    unsigned long           i_size;       /* 文件长度（字节） */
    unsigned long           i_blocks;     /* 占用块数（512B 单位） */
    unsigned int            i_blkbits;    /* 块大小位数（默认 10 = 1KB） */
    dev_t                   i_rdev;       /* 设备文件的设备号（mknod） */

    struct timespec         i_atime;      /* 访问时间 */
    struct timespec         i_mtime;      /* 修改时间 */
    struct timespec         i_ctime;      /* inode 变更时间 */

    struct inode_operations *i_op;
    struct file_operations  *i_fop;
    void                   *i_private;    /* 文件系统私有（ext2_inode_info） */
    unsigned int            i_state;      /* I_NEW 等状态位 */
};

/* ======================== dentry ======================== */

/*
 * dentry - 路径分量缓存（目录树节点）
 *
 * 目录树：d_parent 指向父节点，d_subdirs 收集子节点（d_child 挂入），
 * 根 dentry 的 d_parent 指向自身。
 * 查找键：(d_parent, d_name) 二元组 → dentry_hashtable（dcache.c）。
 * d_inode 为 NULL 时是 negative dentry（查找未命中的缓存）。
 *
 * 生命周期：d_alloc（count=1）→ d_instantiate 挂 inode（对 inode 取引用）→
 * ... → dput 归零：仍在哈希 → 挂 dentry_unused LRU 驻留；
 *                  已 d_drop（unlink）→ 立即销毁（对 inode/parent 释放引用）。
 */
struct dentry {
    atomic_t                d_count;      /* 引用计数 */
    struct inode           *d_inode;      /* NULL = negative dentry */
    struct dentry          *d_parent;     /* 父节点（根 = 自身） */
    struct qstr             d_name;       /* 本分量名字（name 为独立分配） */

    struct list_head        d_subdirs;    /* 子 dentry 链头（d_child 挂入） */
    struct list_head        d_child;      /* 链入 d_parent->d_subdirs */
    struct list_head        d_alias;      /* 链入 d_inode->i_dentry */
    struct list_head        d_hash;       /* dentry_hashtable 桶链 */
    struct list_head        d_lru;        /* dentry_unused LRU */

    struct super_block     *d_sb;
    void                   *d_fsdata;     /* 文件系统私有 */
};

/* dentry 是否已从哈希摘除（d_drop 后为真；自指 = 不在任何桶中） */
#define d_unhashed(dentry)  ((dentry)->d_hash.next == &(dentry)->d_hash)

/* ======================== file ======================== */

/*
 * file - 已打开文件实例（fd 表项指向的对象）
 *
 * 同一文件可被多次打开，各自持有独立 f_pos 游标；
 * f_count 由 fd 表与dup共享计数，归零时触发 release + dput。
 */
struct file {
    atomic_t                f_count;      /* 引用计数 */
    unsigned int            f_flags;      /* 打开标志（O_RDONLY...） */
    unsigned int            f_mode;       /* 读写模式（FMODE_READ/WRITE） */
    unsigned long           f_pos;        /* 读写游标 */
    struct file_operations *f_op;
    struct dentry          *f_dentry;     /* 指向文件 dentry */
    struct vfsmount        *f_vfsmnt;     /* 所属挂载（Task 9） */
    void                   *private_data; /* 驱动私有（TTY ioctl 等） */
};

/* f_mode 位（open 时由 f_flags 转换） */
#define FMODE_READ      0x1
#define FMODE_WRITE     0x2

/* ======================== file_system_type ======================== */

#define FS_REQUIRES_DEV 0x0001    /* 需要真实块设备（ext2）；ramfs 无此位 */

/*
 * file_system_type - 文件系统驱动注册描述
 *
 * register_filesystem 挂入全局 fs_types 单链；
 * get_sb 由 mount 路径调用（Task 9/10），负责读盘/填充并挂 vfsmount。
 */
struct file_system_type {
    const char             *name;
    struct file_system_type *next;
    int                     fs_flags;
    int (*get_sb)(struct file_system_type *fs_type, int flags,
                  const char *dev_name, void *data, struct vfsmount *mnt);
    void (*kill_sb)(struct super_block *sb);
};

/* ======================== files_struct / fs_struct ======================== */

/*
 * files_struct - 进程 fd 表
 *
 * fork 共享（引用计数，等价 CLONE_FILES）；fd 表逻辑 Task 8 接管
 * （alloc_fd/打开计数/关闭清空）。Task 6 仅建结构与计数。
 */
#define NR_OPEN_DEFAULT 32    /* 初始 fd 数组容量 */

struct files_struct {
    atomic_t        count;                        /* 共享计数 */
    int             max_fds;                      /* 当前表容量 */
    struct file    *fd_array[NR_OPEN_DEFAULT];    /* fd → file 映射 */
};

/*
 * fs_struct - 进程根目录与工作目录
 *
 * fork 共享（等价 CLONE_FS）。root/pwd 由 Task 7 rootfs 挂载时填充，
 * chdir（Task 12）改写 pwd。put 归零时 dput 两个 dentry。
 */
struct fs_struct {
    atomic_t        count;
    struct dentry  *root;      /* 进程根目录（'/'） */
    struct dentry  *pwd;       /* 工作目录（相对路径解析基点） */
    int             umask;     /* 创建文件时的权限掩码 */
};

/* ======================== file 表统计 ======================== */

#define NR_FILE 256    /* 全局 file 结构上限 */

struct files_stat_struct {
    int nr_files;        /* 已分配 file 数 */
    int nr_free_files;   /* 预留统计位（暂未用） */
    int max_files;       /* 上限（NR_FILE） */
};

extern struct files_stat_struct files_stat;

/* ======================== 全局链（super.c 对应物暂驻 inode.c） ======================== */

/* 全部已注册 super_block 链表头（定义在 kernel/fs/inode.c） */
extern struct list_head super_blocks;

/* ======================== inode 缓存 API（kernel/fs/inode.c） ======================== */

__init void inode_init(void);

/*
 * iget_locked - 按 (sb, ino) 查找或创建 inode
 *
 * 命中（含 unused 驻留复用）：i_count++ 并返回；
 * 未命中：从 inode_cache 分配，置 I_NEW，挂哈希与 sb->s_inodes。
 * I_NEW 状态下调用方填充 i_mode/i_op/i_fop/时间戳等元数据，
 * 完成后必须调 unlock_new_inode（期间禁止 iput 以外的访问）。
 *
 * 返回：inode 指针，NULL 表示内存不足。
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino);

/* 清除 I_NEW，inode 对外可见（可被并发查找命中） */
void unlock_new_inode(struct inode *inode);

/*
 * iput - 释放 inode 引用
 *
 * 归零且 nlink>0：挂 inode_unused 缓存驻留（后续 iget 哈希命中复用）；
 * 归零且 nlink=0：摘链 → 回调 s_op->delete_inode（释放磁盘资源）
 *                 → 回调 s_op->destroy_inode（可选）→ kmem_cache_free。
 */
void iput(struct inode *inode);

/* 缓存统计（调试/自测观察） */
unsigned int inode_cache_count(void);
unsigned int inode_cache_unused(void);

/* ======================== dentry 缓存 API（kernel/fs/dcache.c） ======================== */

__init void dcache_init(void);

/* 名字哈希（hash * 33 + c 累加，同 Linux dcache.c） */
unsigned int full_name_hash(const char *name, unsigned int len);

/*
 * d_lookup - 在 parent 下按名字查找 dentry
 *
 * 命中：d_count++ 返回（调用方用毕 dput）；
 * 未命中：返回 NULL（negative 结果不入缓存，Task 8 namei 决定是否驻留）。
 */
struct dentry *d_lookup(struct dentry *parent, struct qstr *name);

/*
 * d_alloc - 创建新 dentry（negative，未关联 inode）
 *
 * @parent: 父 dentry；NULL 表示根 dentry（d_parent 自指）。
 * 名字独立 kmalloc 拷贝，挂 dentry_hashtable 与 parent->d_subdirs。
 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);

/*
 * d_instantiate - 将 inode 关联到 dentry（negative → 正项）
 *
 * 对 inode 取一个引用（dput 销毁时 iput 配对释放），
 * d_alias 挂入 inode->i_dentry。
 */
void d_instantiate(struct dentry *dentry, struct inode *inode);

/* d_drop - 从哈希摘除（unlink 后随 dput 触发真实销毁） */
void d_drop(struct dentry *dentry);

/* dput - 释放引用：归零时挂 LRU 驻留，或（已 unhash）立即销毁 */
void dput(struct dentry *dentry);

/* dget - 增加引用（inline 快路径） */
static inline struct dentry *dget(struct dentry *dentry)
{
    if (dentry)
        atomic_inc(&dentry->d_count);
    return dentry;
}

/* 缓存统计（调试/自测观察） */
unsigned int dentry_cache_count(void);
unsigned int dentry_cache_unused(void);

/* ======================== file 表 API（kernel/fs/file_table.c） ======================== */

__init void file_table_init(void);

/*
 * get_empty_filp - 从 file 表分配空 file（f_count=1）
 *
 * 超过 NR_FILE 上限返回 NULL。
 * 调用方填 f_op/f_dentry 后交给 fd 表（Task 8）。
 */
struct file *get_empty_filp(void);

/*
 * fput - 释放 file 引用
 *
 * 归零时回调 f_op->release（若非空）→ dput(f_dentry) → 回收。
 */
void fput(struct file *file);

/* files_struct / fs_struct 引用计数（fork 共享 / do_exit 释放） */
struct files_struct *get_files_struct(struct files_struct *fs);
void put_files_struct(struct files_struct *fs);
struct fs_struct *get_fs_struct(struct fs_struct *fs);
void put_fs_struct(struct fs_struct *fs);

/* ======================== 文件系统类型注册（kernel/fs/file_table.c） ======================== */

/*
 * register_filesystem - 注册文件系统类型（ramfs/ext2 初始化时调用）
 * 返回：0 成功，-1 失败（重名或内存不足）
 */
int register_filesystem(struct file_system_type *fs);

/*
 * unregister_filesystem - 注销文件系统类型
 * 返回：0 成功，-1 未找到
 */
int unregister_filesystem(struct file_system_type *fs);

/* 按名字查找已注册类型（mount 路径用），未找到返回 NULL */
struct file_system_type *get_fs_type(const char *name);

/* ======================== 自测（kernel/fs/selftest.c） ======================== */

/* 启动期引用计数闭环自测（kernel.c thread_init_func 调用一次） */
void vfs_selftest(void);

#endif /* __LULA_FS_H__ */
