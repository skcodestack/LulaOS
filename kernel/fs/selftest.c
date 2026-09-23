/*
 * selftest.c - VFS 三套缓存引用计数闭环自测
 *
 * 参考 LulaOS 驱动层自测惯例（ata_read_mbr 风格）：
 * 启动期一次性执行，printk 各阶段结果，失败立即中止。
 *
 * 覆盖路径：
 *   inode：iget_locked 创建（I_NEW）→ unlock → 二次 iget 哈希命中（count=2）
 *         → iput×2 → unused 驻留（nr_free_inodes 递增）
 *   inode：nlink=0 销毁路径（delete_inode 回调 + nr_inodes 回落）
 *   dentry：d_alloc 建树（根+子）→ d_instantiate（inode 引用转移）
 *         → d_lookup 命中 → d_drop + dput 销毁（级联 iput/dput(parent)）
 *   file：get_empty_filp → fput（release/dput 均为空走纯回收路径）
 *
 * 自测使用独立的 super_block 实例（不挂 super_blocks 链），
 * 结束后 ino=42 驻留 unused（验证复用语义），其余资源全部回落。
 */

#include <fs.h>
#include <libs/memcpy.h>
#include <printk.h>

/* ======================== 自测用 super_block ======================== */

/*
 * test_sb - 自测专用 super_block
 *
 * 不挂 super_blocks 链（无挂载语义），s_op=NULL（销毁路径走
 * 默认回收，不回调 delete_inode/destroy_inode）。
 * s_inodes 必须初始化（iget_locked 会挂入）。
 */
static struct super_block test_sb;

/* ======================== inode 自测 ======================== */

/*
 * selftest_inode - inode 缓存闭环
 *
 * 1) iget_locked(ino=42) 新建（I_NEW）→ 填元数据 → unlock_new_inode
 * 2) 二次 iget_locked(ino=42) 应命中同指针且 i_count=2
 * 3) iput 一次 → i_count=1（仍在 in_use）
 * 4) （供 dentry 测试使用，最终 iput 后进 unused 驻留）
 *
 * 5) iget_locked(ino=43) → i_nlink=0 → iput → 直接销毁（nr_inodes 回落）
 *
 * 返回：0 成功，-1 失败
 */
static int selftest_inode(struct inode **inode42_out)
{
    struct inode *inode, *hit;
    unsigned int before;

    /* --- 1. 创建路径 --- */
    inode = iget_locked(&test_sb, 42);
    if (!inode) {
        printk("VFS: selftest: iget_locked(42) alloc FAILED\n");
        return -1;
    }
    if (!(inode->i_state & I_NEW)) {
        printk("VFS: selftest: fresh inode missing I_NEW\n");
        return -1;
    }
    /* 调用方填充期：元数据 */
    inode->i_mode = S_IFREG | 0644;
    inode->i_size = 123;
    unlock_new_inode(inode);

    /* --- 2. 哈希命中路径 --- */
    hit = iget_locked(&test_sb, 42);
    if (hit != inode) {
        printk("VFS: selftest: iget(42) re-lookup missed (hit=%p want=%p)\n",
               hit, inode);
        return -1;
    }
    if (atomic_read(&inode->i_count) != 2) {
        printk("VFS: selftest: i_count=%d after re-iget (want 2)\n",
               atomic_read(&inode->i_count));
        return -1;
    }
    printk("VFS: selftest: iget cycle OK (ino=42, hit with i_count=2)\n");

    /* --- 3. 释放一次（count=1，交给 dentry 测试持有） --- */
    iput(inode);
    if (atomic_read(&inode->i_count) != 1) {
        printk("VFS: selftest: i_count=%d after iput (want 1)\n",
               atomic_read(&inode->i_count));
        return -1;
    }

    /* --- 4. nlink=0 销毁路径 --- */
    before = inode_cache_count();

    inode = iget_locked(&test_sb, 43);
    if (!inode) {
        printk("VFS: selftest: iget_locked(43) alloc FAILED\n");
        return -1;
    }
    inode->i_nlink = 0;    /* unlink 语义：无名可销毁 */
    unlock_new_inode(inode);
    iput(inode);           /* 归零 + nlink=0 → 直接销毁 */

    if (inode_cache_count() != before) {
        printk("VFS: selftest: nlink=0 destroy FAILED "
               "(nr_inodes=%u want %u)\n",
               inode_cache_count(), before);
        return -1;
    }
    printk("VFS: selftest: inode destroy path OK (nlink=0, nr_inodes back)\n");

    *inode42_out = hit;
    return 0;
}

/* ======================== dentry 自测 ======================== */

/*
 * selftest_dentry - dentry 缓存闭环
 *
 * 引用流（数字为 d_count/i_count 变化）：
 *   root  = d_alloc(NULL, "selftest")   root:1
 *   child = d_alloc(root, "child")      child:1, root:2（父引用）
 *   d_instantiate(child, inode42)       inode42: 1→2（dentry 层持有）
 *   d_lookup(root, "child") 命中        child:2
 *   dput(child)（释放查找引用）          child:1
 *   d_drop(child)                        unhash
 *   dput(child)（销毁路径）              → iput(inode42)=1
 *                                        → dput(root)=1
 *   iput(inode42)                        → 0，nlink=1 → unused 驻留
 *   d_drop(root); dput(root)             根销毁（parent 自指跳过）
 *
 * 返回：0 成功，-1 失败
 */
static int selftest_dentry(struct inode *inode42)
{
    struct dentry *root, *child, *found;
    struct qstr root_name, child_name;
    unsigned int before;

    /* 名字构造（full_name_hash 预计算，d_alloc/d_lookup 均需） */
    root_name.len  = 8;
    root_name.name = "selftest";
    root_name.hash = full_name_hash(root_name.name, root_name.len);

    child_name.len  = 5;
    child_name.name = "child";
    child_name.hash = full_name_hash(child_name.name, child_name.len);

    before = dentry_cache_count();

    /* --- 1. 建树：根 + 子 --- */
    root = d_alloc(NULL, &root_name);
    if (!root) {
        printk("VFS: selftest: d_alloc(root) FAILED\n");
        return -1;
    }
    if (root->d_parent != root) {
        printk("VFS: selftest: root d_parent not self\n");
        return -1;
    }

    child = d_alloc(root, &child_name);
    if (!child) {
        printk("VFS: selftest: d_alloc(child) FAILED\n");
        return -1;
    }
    if (atomic_read(&root->d_count) != 2) {
        printk("VFS: selftest: root d_count=%d after child (want 2)\n",
               atomic_read(&root->d_count));
        return -1;
    }

    /* --- 2. 实例化：关联 inode（引用转移入 dentry 层） --- */
    d_instantiate(child, inode42);
    if (child->d_inode != inode42) {
        printk("VFS: selftest: d_instantiate did not bind inode\n");
        return -1;
    }
    if (atomic_read(&inode42->i_count) != 2) {
        printk("VFS: selftest: inode42 i_count=%d after instantiate (want 2)\n",
               atomic_read(&inode42->i_count));
        return -1;
    }

    /* --- 3. 查找命中 --- */
    found = d_lookup(root, &child_name);
    if (found != child) {
        printk("VFS: selftest: d_lookup('child') missed\n");
        return -1;
    }
    if (atomic_read(&child->d_count) != 2) {
        printk("VFS: selftest: child d_count=%d after lookup (want 2)\n",
               atomic_read(&child->d_count));
        return -1;
    }
    printk("VFS: selftest: dentry cycle OK ('selftest/child' lookup hit)\n");

    /* --- 4. 释放查找引用 --- */
    dput(found);
    if (atomic_read(&child->d_count) != 1) {
        printk("VFS: selftest: child d_count=%d after dput (want 1)\n",
               atomic_read(&child->d_count));
        return -1;
    }

    /* --- 5. unlink 语义：d_drop + dput → 销毁（级联归还引用） --- */
    d_drop(child);
    dput(child);

    if (dentry_cache_count() != before + 1) {
        printk("VFS: selftest: child destroy FAILED (nr_dentry=%u want %u)\n",
               dentry_cache_count(), before + 1);
        return -1;
    }
    if (atomic_read(&inode42->i_count) != 1) {
        printk("VFS: selftest: inode42 i_count=%d after child kill (want 1)\n",
               atomic_read(&inode42->i_count));
        return -1;
    }

    /* --- 6. inode42 最终 iput → unused 驻留（复用语义验证点） --- */
    iput(inode42);
    if (inode_cache_unused() != 1) {
        printk("VFS: selftest: inode42 not parked in unused "
               "(nr_free=%u want 1)\n",
               inode_cache_unused());
        return -1;
    }

    /* --- 7. 根 dentry 销毁（parent 自指，不级联 dput） --- */
    d_drop(root);
    dput(root);

    if (dentry_cache_count() != before) {
        printk("VFS: selftest: root destroy FAILED (nr_dentry=%u want %u)\n",
               dentry_cache_count(), before);
        return -1;
    }
    printk("VFS: selftest: dentry destroy path OK "
           "(drop+put cascade, nr_dentry back)\n");

    return 0;
}

/* ======================== file 自测 ======================== */

/*
 * selftest_file - file 表闭环
 *
 * get_empty_filp（f_op/f_dentry 为 NULL）→ fput 走纯回收路径
 * （release/dput 均判空跳过），验证 nr_files 回落。
 *
 * 返回：0 成功，-1 失败
 */
static int selftest_file(void)
{
    struct file *file;

    file = get_empty_filp();
    if (!file) {
        printk("VFS: selftest: get_empty_filp FAILED\n");
        return -1;
    }
    if (atomic_read(&file->f_count) != 1 || files_stat.nr_files != 1) {
        printk("VFS: selftest: filp count/stat wrong (%d, %d)\n",
               atomic_read(&file->f_count), files_stat.nr_files);
        return -1;
    }

    fput(file);

    if (files_stat.nr_files != 0) {
        printk("VFS: selftest: fput FAILED (nr_files=%d want 0)\n",
               files_stat.nr_files);
        return -1;
    }
    printk("VFS: selftest: file cycle OK (alloc/fput, nr_files back)\n");

    return 0;
}

/* ======================== 入口 ======================== */

/*
 * vfs_selftest - 启动期三缓存闭环自测
 *
 * kernel.c thread_init_func 中 file_table_init() 之后调用一次。
 * 任一阶段失败打印 FAIL 并中止（不影响后续内核启动）。
 */
void vfs_selftest(void)
{
    struct inode *inode42 = NULL;

    /* 自测 sb 初始化（s_inodes 是 iget_locked 的挂入点） */
    memset(&test_sb, 0, sizeof(test_sb));
    INIT_LIST_HEAD(&test_sb.s_inodes);

    if (selftest_inode(&inode42) != 0)
        goto fail;
    if (selftest_dentry(inode42) != 0)
        goto fail;
    if (selftest_file() != 0)
        goto fail;

    printk("VFS: selftest passed (inode/dentry/file refcount cycles OK)\n");
    return;

fail:
    printk("VFS: selftest FAILED, VFS caches left in partial state\n");
}
