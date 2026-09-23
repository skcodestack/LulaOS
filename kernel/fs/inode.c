/*
 * inode.c - VFS inode 缓存
 *
 * 参考：
 *   fs/inode.c           — inode_hashtable/iget_locked/iput/inode_in_use
 *   fs/super.c           — super_blocks 全局链（本项目暂驻本文件）
 *
 * 数据组织：
 *   [inode_hashtable] 按 (sb, ino) 哈希索引 — iget_locked 快速命中
 *   [inode_in_use]    i_count > 0 的 inode
 *   [inode_unused]    i_count == 0 且 nlink > 0 的驻留 inode（哈希保留，
 *                     再次 iget 命中复用，不回读磁盘）
 *   [sb->s_inodes]    每个文件系统自己的 inode 全集（含驻留）
 *
 * 锁：
 *   inode_lock 一把全局自旋锁，保护哈希桶/两条链/s_inodes/i_count 判定。
 *   delete_inode 回调在锁外执行（可能做磁盘 I/O）。
 *
 * 与 Linux 的差异：
 *   - 无 inode 状态锁/wait_on_inode（单锁同步查找，I_NEW 期间调用方独占）
 *   - unused 驻留不做 shrink（回收留给后续任务迭代）
 *   - 统一从 inode_cache 分配（slab），不实现 alloc_inode 覆盖路径；
 *     ext2 的私有数据通过 i_private 挂接
 */

#include <fs.h>
#include <arch/x86/spinlock.h>   /* inode_lock 自旋锁 */
#include <mm/slab.h>
#include <libs/memcpy.h>
#include <printk.h>

/* ======================== 全局状态 ======================== */

#define I_HASH_BITS     10
#define I_HASH_SIZE     (1UL << I_HASH_BITS)     /* 1024 桶 */
#define I_HASH_MASK     (I_HASH_SIZE - 1)

/* 全部已注册 super_block 链表头（fs.h extern，get_sb 挂入/put_super 摘除） */
struct list_head super_blocks = LIST_HEAD_INIT(super_blocks);

static struct list_head inode_hashtable[I_HASH_SIZE];
static struct list_head inode_in_use;
static struct list_head inode_unused;
static spinlock_t inode_lock = SPIN_LOCK_UNLOCKED;

static kmem_cache_t *inode_cache;

static unsigned int nr_inodes;          /* 存在的 inode 总数（in_use+unused） */
static unsigned int nr_free_inodes;     /* unused 驻留数 */

/* ======================== 哈希 ======================== */

/*
 * i_hashfn - inode 哈希函数
 *
 * 输入 (sb, ino)，输出桶号。ino 与 sb 指针混合，
 * 同一 ino 在不同文件系统（不同 sb）分散到不同桶。
 */
static inline unsigned int i_hashfn(struct super_block *sb,
                                    unsigned long ino)
{
    unsigned long val = ino ^ (ino >> 8) ^ (unsigned long)sb;
    return (unsigned int)((val ^ (val >> 12)) & I_HASH_MASK);
}

/*
 * __iget - 命中路径的链表迁移（须持 inode_lock）
 *
 * 从 unused 命中的 inode（i_count 0→1）迁回 in_use。
 */
static inline void __iget(struct inode *inode)
{
    if (atomic_read(&inode->i_count) == 1) {
        /* 归零期间它挂在 unused 上 */
        list_del_init(&inode->i_list);
        list_add(&inode->i_list, &inode_in_use);
        nr_free_inodes--;
    }
}

/* ======================== 公共 API ======================== */

/*
 * inode_init - 初始化 inode 缓存
 *
 * 须在 kmem_cache_init 之后调用（kmem_cache_create 依赖 slab 就绪）。
 */
__init void inode_init(void)
{
    unsigned int i;

    for (i = 0; i < I_HASH_SIZE; i++)
        INIT_LIST_HEAD(&inode_hashtable[i]);
    INIT_LIST_HEAD(&inode_in_use);
    INIT_LIST_HEAD(&inode_unused);

    inode_cache = kmem_cache_create("inode_cache", sizeof(struct inode));
    if (!inode_cache) {
        printk("VFS: failed to create inode_cache\n");
        return;
    }

    printk("VFS: inode cache initialized (buckets=%u)\n", I_HASH_SIZE);
}

/*
 * iget_locked - 按 (sb, ino) 查找或创建 inode
 *
 * 命中（含 unused 驻留复用）：i_count++，返回。
 * 未命中：从 inode_cache 分配，i_state=I_NEW，调用方独占填充
 * （i_mode/i_nlink/i_op/i_fop/时间戳），完成后调 unlock_new_inode。
 *
 * 注意：新 inode 挂入 sb->s_inodes，调用方须保证 sb->s_inodes
 * 已 INIT_LIST_HEAD（get_sb 填充职责）。
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
    struct list_head *head = &inode_hashtable[i_hashfn(sb, ino)];
    struct inode *inode;
    unsigned long flags;

    /* 1. 查哈希 */
    spin_lock_irqsave(&inode_lock, flags);
    list_for_each_entry(inode, head, i_hash) {
        if (inode->i_ino == ino && inode->i_sb == sb) {
            atomic_inc(&inode->i_count);
            __iget(inode);
            spin_unlock_irqrestore(&inode_lock, flags);
            return inode;
        }
    }
    spin_unlock_irqrestore(&inode_lock, flags);

    /* 2. 未命中：分配新 inode（锁外，kmem_cache_alloc 可能扩充 slab） */
    inode = (struct inode *)kmem_cache_alloc(inode_cache);
    if (!inode) {
        printk("VFS: iget_locked: alloc failed for ino %lu\n", ino);
        return NULL;
    }

    memset(inode, 0, sizeof(*inode));
    inode->i_sb      = sb;
    inode->i_ino     = ino;
    atomic_set(&inode->i_count, 1);
    inode->i_nlink   = 1;    /* 默认 1；ext2 从磁盘覆盖，unlink 减到 0 */
    inode->i_blkbits = 10;   /* 默认 1KB 块；fill_super 按实际覆盖 */
    inode->i_state   = I_NEW;

    INIT_LIST_HEAD(&inode->i_hash);
    INIT_LIST_HEAD(&inode->i_sb_list);
    INIT_LIST_HEAD(&inode->i_list);
    INIT_LIST_HEAD(&inode->i_dentry);

    /* 3. 挂哈希桶、sb 链与 in_use（须持锁） */
    spin_lock_irqsave(&inode_lock, flags);
    list_add(&inode->i_hash, head);
    list_add(&inode->i_sb_list, &sb->s_inodes);
    list_add(&inode->i_list, &inode_in_use);
    nr_inodes++;
    spin_unlock_irqrestore(&inode_lock, flags);

    return inode;
}

/*
 * unlock_new_inode - 结束 I_NEW 填充期，inode 对外可见
 *
 * 之后 iget_locked 可命中该 inode（填充期间独占由调用方纪律保证，
 * 单锁同步查找使其他查找者要么看不到（未挂哈希前），要么看到完整数据）。
 */
void unlock_new_inode(struct inode *inode)
{
    unsigned long flags;

    spin_lock_irqsave(&inode_lock, flags);
    inode->i_state &= ~I_NEW;
    spin_unlock_irqrestore(&inode_lock, flags);
}

/*
 * iput - 释放 inode 引用
 *
 * 归零且 nlink>0：摘出 in_use，挂 unused 头（缓存驻留）。
 * 归零且 nlink=0：摘除哈希/s_inodes/in_use，锁外回调
 *   s_op->delete_inode（释放文件系统磁盘资源，如位图回收）与
 *   s_op->destroy_inode（可选，释放 i_private），然后回收结构。
 */
void iput(struct inode *inode)
{
    unsigned long flags;

    if (!inode)
        return;

    spin_lock_irqsave(&inode_lock, flags);

    if (!atomic_dec_and_test(&inode->i_count)) {
        spin_unlock_irqrestore(&inode_lock, flags);
        return;
    }

    /* 引用归零：此刻必在 in_use（unused 链上的 i_count 恒为 0） */
    list_del_init(&inode->i_list);

    if (inode->i_nlink == 0) {
        /* 销毁路径：从哈希与 sb 链摘除 */
        list_del_init(&inode->i_hash);
        list_del_init(&inode->i_sb_list);
        nr_inodes--;
        spin_unlock_irqrestore(&inode_lock, flags);

        /* 锁外回调（delete_inode 可能做磁盘 I/O 释放位图等） */
        if (inode->i_sb && inode->i_sb->s_op) {
            if (inode->i_sb->s_op->delete_inode)
                inode->i_sb->s_op->delete_inode(inode);
            if (inode->i_sb->s_op->destroy_inode)
                inode->i_sb->s_op->destroy_inode(inode);
        }

        /* 防御：销毁时不应仍有 dentry 引用（d_alias 链应为空） */
        if (!list_empty(&inode->i_dentry))
            printk("VFS: iput: ino %lu destroyed with live dentry(s)\n",
                   inode->i_ino);

        kmem_cache_free(inode_cache, inode);
        return;
    }

    /* nlink>0：缓存驻留（哈希保留，后续 iget 命中复用） */
    list_add(&inode->i_list, &inode_unused);
    nr_free_inodes++;
    spin_unlock_irqrestore(&inode_lock, flags);
}

/* ======================== 统计 ======================== */

unsigned int inode_cache_count(void)
{
    return nr_inodes;
}

unsigned int inode_cache_unused(void)
{
    return nr_free_inodes;
}
