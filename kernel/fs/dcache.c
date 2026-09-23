/*
 * dcache.c - VFS dentry 缓存（目录树 + 哈希 + LRU）
 *
 * 参考：
 *   fs/dcache.c — dentry_hashtable/d_alloc/d_lookup/d_instantiate/
 *                 d_drop/dput/full_name_hash
 *
 * 数据组织：
 *   [dentry_hashtable] 按 (parent, name->hash) 哈希索引 — d_lookup 命中
 *   [dentry_unused]    d_count==0 且仍在哈希的驻留 dentry（LRU 头插）
 *   [目录树]           d_parent/d_subdirs/d_child 构成，
 *                      根 dentry 的 d_parent 自指
 *
 * 生命周期（引用配对，与 Linux 的一处刻意差异）：
 *   d_alloc          d_count = 1（调用方持有）
 *   d_instantiate    对 inode 取一个引用（d_alias 挂入 inode->i_dentry）；
 *                    由本层取引用而非调用方预持，
 *                    dput 销毁时 iput 配对释放，配对纪律内聚在缓存层
 *   d_lookup 命中    d_count++（调用方 dput 配对）
 *   dput 归零：
 *     仍在哈希       挂 dentry_unused LRU 驻留（父链/名字/inode 全保留）
 *     已 d_drop      立即销毁（unlink 语义）：摘 d_alias → iput(inode)、
 *                    摘 d_child → dput(parent)、释放名字与结构
 *
 * 锁：
 *   dcache_lock 一把全局自旋锁，保护哈希/LRU/树链/d_alias 挂摘/d_count 判定。
 *   锁分工：inode 的 i_count 判定与 i_hash/i_list 归 inode_lock；
 *   d_alias 链（dentry 侧挂入 inode->i_dentry）归 dcache_lock。
 *   iput/dput(parent)/kmem_cache_free 在 dcache_lock 外执行（内部有锁）。
 */

#include <fs.h>
#include <arch/x86/spinlock.h>   /* dcache_lock 自旋锁 */
#include <mm/slab.h>
#include <libs/memcpy.h>
#include <printk.h>

/* ======================== 全局状态 ======================== */

#define D_HASH_BITS     10
#define D_HASH_SIZE     (1UL << D_HASH_BITS)     /* 1024 桶 */
#define D_HASH_MASK     (D_HASH_SIZE - 1)

static struct list_head dentry_hashtable[D_HASH_SIZE];
static struct list_head dentry_unused;    /* LRU：头部 = 最近释放 */
static spinlock_t dcache_lock = SPIN_LOCK_UNLOCKED;

static kmem_cache_t *dentry_cache;

static unsigned int nr_dentry;            /* 存在的 dentry 总数（含 LRU 驻留） */
static unsigned int nr_dentry_unused;     /* LRU 驻留数 */

/* ======================== 哈希与名字比较 ======================== */

/*
 * d_hashfn - dentry 哈希函数
 *
 * 键为 (parent, name->hash)：同名字在不同父目录下分散。
 */
static inline unsigned int d_hashfn(struct dentry *parent,
                                    unsigned long hash)
{
    unsigned long val = hash ^ (unsigned long)parent;
    return (unsigned int)((val ^ (val >> 12)) & D_HASH_MASK);
}

/*
 * full_name_hash - 名字哈希（hash * 33 + c 累加）
 *
 * 照搬 Linux dcache.c：左移 5 减自身等价乘 33。
 */
unsigned int full_name_hash(const char *name, unsigned int len)
{
    unsigned long hash = 0;

    while (len--)
        hash = (hash << 5) - hash + (unsigned char)*name++;
    return (unsigned int)hash;
}

/*
 * dentry_name_compare - 定长逐字节比较（项目暂无 memcmp）
 *
 * 返回：0 相等，非 0 不等。
 */
static int dentry_name_compare(const char *a, const char *b,
                               unsigned int len)
{
    while (len--) {
        if (*a++ != *b++)
            return 1;
    }
    return 0;
}

/* ======================== 内部：销毁 ======================== */

/*
 * dentry_kill - 销毁 dentry（调用方已无引用，且已 unhash）
 *
 * 摘 d_alias（放弃对 inode 的持有）→ iput(inode)；
 * 摘 d_child（离开父目录）→ dput(parent)（父可能随之级联销毁）。
 * iput/dput/kmem_cache_free 均在 dcache_lock 外执行（它们内部有锁，
 * 持 dcache_lock 调用会构成嵌套自旋）。
 */
static void dentry_kill(struct dentry *dentry)
{
    struct inode *inode;
    struct dentry *parent;
    unsigned long flags;

    spin_lock_irqsave(&dcache_lock, flags);

    /* 摘 d_alias：对 inode 的引用在随后的 iput 中归还 */
    list_del_init(&dentry->d_alias);

    /* 摘 d_child：离开父目录的子链 */
    list_del_init(&dentry->d_child);

    inode  = dentry->d_inode;
    parent = (dentry->d_parent != dentry) ? dentry->d_parent : NULL;
    dentry->d_inode = NULL;

    /* 防御：从哈希与 LRU 确保摘净（调用路径保证已 unhash） */
    list_del_init(&dentry->d_hash);
    list_del_init(&dentry->d_lru);
    nr_dentry--;

    spin_unlock_irqrestore(&dcache_lock, flags);

    /* 释放名字与结构（锁外） */
    kfree(dentry->d_name.name);
    kmem_cache_free(dentry_cache, dentry);

    /* 归还引用（可能级联销毁：父 dentry、inode 资源） */
    if (inode)
        iput(inode);
    if (parent)
        dput(parent);
}

/* ======================== 公共 API ======================== */

/*
 * dcache_init - 初始化 dentry 缓存
 *
 * 须在 kmem_cache_init 之后调用。
 */
__init void dcache_init(void)
{
    unsigned int i;

    for (i = 0; i < D_HASH_SIZE; i++)
        INIT_LIST_HEAD(&dentry_hashtable[i]);
    INIT_LIST_HEAD(&dentry_unused);

    dentry_cache = kmem_cache_create("dentry_cache", sizeof(struct dentry));
    if (!dentry_cache) {
        printk("VFS: failed to create dentry_cache\n");
        return;
    }

    printk("VFS: dcache initialized (buckets=%u)\n", D_HASH_SIZE);
}

/*
 * d_lookup - 在 parent 下按名字查找 dentry
 *
 * 比较 parent 指针 + hash + len + 逐字节名字。
 * 命中（含 LRU 驻留）：d_count++（迁出 LRU）并返回；
 * 未命中：返回 NULL。
 */
struct dentry *d_lookup(struct dentry *parent, struct qstr *name)
{
    unsigned int len = name->len;
    unsigned long hashval = name->hash;
    struct list_head *head =
        &dentry_hashtable[d_hashfn(parent, hashval)];
    struct dentry *dentry;
    unsigned long flags;

    spin_lock_irqsave(&dcache_lock, flags);
    list_for_each_entry(dentry, head, d_hash) {
        if (dentry->d_name.hash != hashval)
            continue;
        if (dentry->d_parent != parent)
            continue;
        if (dentry->d_name.len != len)
            continue;
        if (dentry_name_compare(dentry->d_name.name, name->name, len))
            continue;

        /* 命中：取引用；若在 LRU 驻留则摘出（变回活跃） */
        atomic_inc(&dentry->d_count);
        if (dentry->d_lru.next != &dentry->d_lru) {
            list_del_init(&dentry->d_lru);
            nr_dentry_unused--;
        }
        spin_unlock_irqrestore(&dcache_lock, flags);
        return dentry;
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    return NULL;
}

/*
 * d_alloc - 创建新 dentry（negative，d_inode=NULL）
 *
 * @parent: 父 dentry；NULL 表示根 dentry（d_parent 自指，d_sb=NULL，
 *          根 dentry 的 d_sb 由 get_sb/挂载方填入）。
 *
 * 名字独立 kmalloc(len+1)（含 '\0'，strcpy 安全），
 * 挂哈希桶与 parent->d_subdirs，对 parent 取引用。
 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name)
{
    struct dentry *dentry;
    char *dname;
    unsigned long flags;

    dentry = (struct dentry *)kmem_cache_alloc(dentry_cache);
    if (!dentry)
        return NULL;

    dname = (char *)kmalloc(name->len + 1, GFP_KERNEL);
    if (!dname) {
        kmem_cache_free(dentry_cache, dentry);
        return NULL;
    }
    memcpy(dname, name->name, name->len);
    dname[name->len] = '\0';

    memset(dentry, 0, sizeof(*dentry));
    atomic_set(&dentry->d_count, 1);
    dentry->d_name.name = dname;
    dentry->d_name.len  = name->len;
    dentry->d_name.hash = name->hash;
    dentry->d_inode     = NULL;    /* negative：待 d_instantiate */

    INIT_LIST_HEAD(&dentry->d_subdirs);
    INIT_LIST_HEAD(&dentry->d_child);
    INIT_LIST_HEAD(&dentry->d_alias);
    INIT_LIST_HEAD(&dentry->d_hash);
    INIT_LIST_HEAD(&dentry->d_lru);

    spin_lock_irqsave(&dcache_lock, flags);

    if (parent) {
        dentry->d_parent = parent;
        dentry->d_sb = parent->d_sb;
        atomic_inc(&parent->d_count);        /* 父引用（d_child 配对） */
        list_add_tail(&dentry->d_child, &parent->d_subdirs);
    } else {
        dentry->d_parent = dentry;           /* 根：自指 */
        dentry->d_sb = NULL;
    }

    /* 挂哈希桶（键含 d_parent，根以自身参与） */
    list_add(&dentry->d_hash,
             &dentry_hashtable[d_hashfn(dentry->d_parent, name->hash)]);
    nr_dentry++;

    spin_unlock_irqrestore(&dcache_lock, flags);

    return dentry;
}

/*
 * d_instantiate - 将 inode 关联到 dentry（negative → 正项）
 *
 * 对 inode 取一个引用（本层持有，dentry 销毁时 iput 归还），
 * d_alias 挂入 inode->i_dentry（该链归 dcache_lock 保护）。
 */
void d_instantiate(struct dentry *dentry, struct inode *inode)
{
    unsigned long flags;

    if (!dentry || !inode)
        return;

    spin_lock_irqsave(&dcache_lock, flags);
    if (dentry->d_inode) {
        /* 防御：已实例化（调用方 bug），保持幂等 */
        spin_unlock_irqrestore(&dcache_lock, flags);
        printk("VFS: d_instantiate: dentry '%s' already instantiated\n",
               dentry->d_name.name);
        return;
    }
    dentry->d_inode = inode;
    atomic_inc(&inode->i_count);
    list_add(&dentry->d_alias, &inode->i_dentry);
    spin_unlock_irqrestore(&dcache_lock, flags);
}

/*
 * d_drop - 从哈希摘除（unlink 路径第一步）
 *
 * 摘除后 d_lookup 不再命中；调用方随后 dput 释放最后引用时
 * 走立即销毁路径（dentry_kill），完成 inode/parent 引用归还。
 * 若此刻 d_count==0（仅 LRU 驻留无人再 dput），直接销毁防泄漏。
 */
void d_drop(struct dentry *dentry)
{
    unsigned long flags;

    if (!dentry)
        return;

    spin_lock_irqsave(&dcache_lock, flags);

    if (!d_unhashed(dentry)) {
        list_del_init(&dentry->d_hash);

        /* 摘出 LRU（若驻留） */
        if (dentry->d_lru.next != &dentry->d_lru) {
            list_del_init(&dentry->d_lru);
            nr_dentry_unused--;
        }

        /* 无引用的驻留项：无人会再 dput，直接销毁 */
        if (atomic_read(&dentry->d_count) == 0) {
            spin_unlock_irqrestore(&dcache_lock, flags);
            dentry_kill(dentry);
            return;
        }
    }

    spin_unlock_irqrestore(&dcache_lock, flags);
}

/*
 * dput - 释放 dentry 引用
 *
 * 归零且仍在哈希：挂 dentry_unused LRU 头（最近释放，全字段保留）。
 * 归零且已 unhash（d_drop 过）：dentry_kill 立即销毁。
 */
void dput(struct dentry *dentry)
{
    unsigned long flags;

    if (!dentry)
        return;

    spin_lock_irqsave(&dcache_lock, flags);

    if (atomic_dec_and_test(&dentry->d_count)) {
        if (d_unhashed(dentry)) {
            /* 已 d_drop：销毁路径（iput/dput 需锁外） */
            spin_unlock_irqrestore(&dcache_lock, flags);
            dentry_kill(dentry);
            return;
        }

        /* 正常释放：LRU 头插（最近使用位置，复用时最先命中） */
        list_add(&dentry->d_lru, &dentry_unused);
        nr_dentry_unused++;
    }

    spin_unlock_irqrestore(&dcache_lock, flags);
}

/* ======================== 统计 ======================== */

unsigned int dentry_cache_count(void)
{
    return nr_dentry;
}

unsigned int dentry_cache_unused(void)
{
    return nr_dentry_unused;
}
