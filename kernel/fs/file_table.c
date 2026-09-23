/*
 * file_table.c - VFS file 结构池 + files/fs_struct 引用计数
 *               + 文件系统类型注册表
 *
 * 参考：
 *   fs/file_table.c   — filp_cache / get_empty_filp / fput
 *   fs/filesystems.c  — register_filesystem / get_fs_type（fs_types 链）
 *   kernel/fork.c     — get/put_files_struct, get/put_fs_struct
 *
 * file 生命周期：
 *   get_empty_filp 分配（f_count=1）→ open 路径填 f_op/f_dentry →
 *   fd 表持有 → ... → fput 归零：f_op->release（若非空）→ dput(f_dentry)
 *   → kmem_cache_free。
 *
 * 锁：file 表本身无锁（启动期单线程创建；运行期 fd 表由持有者
 * 串行访问，Task 8 接入时如需再补 files->file_lock）。
 * f_op->release 回调不持任何 VFS 锁执行（TTY close 等可能睡眠）。
 *
 * files_struct / fs_struct：fork 共享（CLONE_FILES/CLONE_FS 语义），
 * put 归零时整体回收（fd 数组清空、root/pwd dput）。
 * Task 6 阶段进程恒为 NULL，代码防御性就位，Task 7/8 接入即生效。
 */

#include <fs.h>
#include <mm/slab.h>
#include <libs/memcpy.h>
#include <libs/string.h>       /* strcmp（get_fs_type/register_filesystem） */
#include <printk.h>

/* ======================== 全局状态 ======================== */

static kmem_cache_t *filp_cache;

/* file 表统计（fs.h extern） */
struct files_stat_struct files_stat = { 0, 0, 0 };

/* 已注册文件系统类型单链（register_filesystem 头插） */
static struct file_system_type *file_systems;

/* ======================== file 表 ======================== */

/*
 * file_table_init - 初始化 file 结构池
 *
 * 须在 kmem_cache_init 之后调用。
 */
__init void file_table_init(void)
{
    filp_cache = kmem_cache_create("filp_cache", sizeof(struct file));
    if (!filp_cache) {
        printk("VFS: failed to create filp_cache\n");
        return;
    }

    files_stat.max_files = NR_FILE;

    printk("VFS: file table initialized (max_files=%d)\n", NR_FILE);
}

/*
 * get_empty_filp - 从 file 表分配空 file（f_count=1）
 *
 * 超过 NR_FILE 上限返回 NULL（fd 泄漏防御）。
 * 调用方填 f_op/f_dentry/f_flags 后交给 fd 表（Task 8）。
 */
struct file *get_empty_filp(void)
{
    struct file *file;

    if (files_stat.nr_files >= files_stat.max_files) {
        printk("VFS: get_empty_filp: file table full (%d)\n",
               files_stat.nr_files);
        return NULL;
    }

    file = (struct file *)kmem_cache_alloc(filp_cache);
    if (!file)
        return NULL;

    memset(file, 0, sizeof(*file));
    atomic_set(&file->f_count, 1);
    file->f_pos = 0;

    files_stat.nr_files++;
    return file;
}

/*
 * fput - 释放 file 引用
 *
 * 归零时回调 f_op->release（close 语义；签名 (inode, file)，
 * inode 取自 f_dentry，无 dentry 的 file 传 NULL）→
 * dput(f_dentry) → 回收结构。
 */
void fput(struct file *file)
{
    struct dentry *dentry;
    struct inode *inode;
    struct file_operations *fop;

    if (!file)
        return;

    if (!atomic_dec_and_test(&file->f_count))
        return;

    dentry = file->f_dentry;
    inode  = dentry ? dentry->d_inode : NULL;
    fop    = file->f_op;

    /* close 回调（无锁执行；TTY 等驱动在 release 中做清理） */
    if (fop && fop->release)
        fop->release(inode, file);

    /* 归还 dentry 引用（file 对 f_dentry 持有一个引用，open 时取得） */
    if (dentry)
        dput(dentry);

    file->f_dentry = NULL;
    file->f_op = NULL;
    kmem_cache_free(filp_cache, file);
    files_stat.nr_files--;
}

/* ======================== files_struct / fs_struct ======================== */

/*
 * get_files_struct - 增加共享计数（fork 复制时调用）
 */
struct files_struct *get_files_struct(struct files_struct *fs)
{
    if (fs)
        atomic_inc(&fs->count);
    return fs;
}

/*
 * put_files_struct - 释放共享计数；归零时回收 fd 数组与结构
 *
 * fd 表逻辑（遍历 fput）Task 8 接入后补充；
 * Task 6 阶段 fd_array 恒空，直接回收。
 */
void put_files_struct(struct files_struct *fs)
{
    if (!fs)
        return;

    if (!atomic_dec_and_test(&fs->count))
        return;

    /* Task 8：此处遍历 fd_array 对非空项 fput 后再回收 */
    kfree(fs);
}

/*
 * get_fs_struct - 增加共享计数（fork 复制时调用）
 */
struct fs_struct *get_fs_struct(struct fs_struct *fs)
{
    if (fs)
        atomic_inc(&fs->count);
    return fs;
}

/*
 * put_fs_struct - 释放共享计数；归零时 dput root/pwd 并回收结构
 *
 * root/pwd 由 Task 7 rootfs 挂载时填充；Task 6 阶段恒 NULL。
 */
void put_fs_struct(struct fs_struct *fs)
{
    if (!fs)
        return;

    if (!atomic_dec_and_test(&fs->count))
        return;

    if (fs->root)
        dput(fs->root);
    if (fs->pwd)
        dput(fs->pwd);
    kfree(fs);
}

/* ======================== 文件系统类型注册 ======================== */

/*
 * register_filesystem - 注册文件系统类型
 *
 * 头插 fs_types 链；重名或 name 为空返回 -1。
 * ramfs（Task 7）/ ext2（Task 10）初始化时调用。
 */
int register_filesystem(struct file_system_type *fs)
{
    struct file_system_type *p;

    if (!fs || !fs->name)
        return -1;

    for (p = file_systems; p; p = p->next) {
        if (strcmp(p->name, fs->name) == 0)
            return -1;    /* 重名 */
    }

    fs->next = file_systems;
    file_systems = fs;
    printk("VFS: registered filesystem '%s'\n", fs->name);
    return 0;
}

/*
 * unregister_filesystem - 注销文件系统类型
 */
int unregister_filesystem(struct file_system_type *fs)
{
    struct file_system_type **p;

    if (!fs)
        return -1;

    for (p = &file_systems; *p; p = &(*p)->next) {
        if (*p == fs) {
            *p = fs->next;
            fs->next = NULL;
            return 0;
        }
    }
    return -1;
}

/*
 * get_fs_type - 按名字查找已注册类型
 *
 * mount 路径（Task 9）用：解析 fstype 字符串后定位 get_sb 回调。
 */
struct file_system_type *get_fs_type(const char *name)
{
    struct file_system_type *p;

    for (p = file_systems; p; p = p->next) {
        if (strcmp(p->name, name) == 0)
            return p;
    }
    return NULL;
}
