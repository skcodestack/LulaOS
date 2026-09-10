/*
 * blk-core.c - LulaOS 块设备核心层
 *
 * 参考 Linux 2.6.20：
 *   block/genhd.c          — register_blkdev, add_disk, gendisk 管理
 *   fs/block_dev.c         — bdget, bdput, block_device 生命周期
 *   block/ll_rw_blk.c     — submit_bh（简化版，直调 strategy）
 *
 * 三层设计：
 *   1) register_blkdev()    — 主设备号注册（blkdevs[] 数组索引）
 *   2) add_disk()           — gendisk 注册 + 自动创建整盘 block_device
 *   3) submit_bh()          — 同步 I/O：直接调用 bdev->bd_disk->fops->strategy()
 *
 * 简化点（对照 Linux 2.6.20）：
 *   - 无 request_queue / elevator（电梯调度）
 *   - 无 bio 层（块 I/O 由 submit_bh 直接完成，Task 4 buffer_head 接入后扩展）
 *   - 无引用计数 kobject（单线程，无并发释放问题）
 */

#include <block/blkdev.h>
#include <mm/slab.h>        /* kmalloc / kfree / GFP_KERNEL */
#include <libs/string.h>    /* _memset / _memcpy / strncpy / strcmp */
#include <libs/memcpy.h>    /* memset */
#include <printk.h>

/* ======================== 全局注册表 ======================== */

/*
 * blkdevs[] - 主设备号注册表
 *
 * blkdevs[major] 存放该主设备号的名称与操作表。
 * major = 0 表示空槽位；register_blkdev(0, ...) 动态分配时扫描首个空槽。
 *
 * 参考 Linux 2.6.20 fs/block_dev.c 的 major_index[]（idr 红黑树），
 * 此处用静态数组，O(1) 查找，实现简洁。
 */
struct blkdev_entry {
    const char                      *name;
    struct block_device_operations  *fops;
};

static struct blkdev_entry blkdevs[MAX_BLKDEV];

/*
 * disk_list - 全局 gendisk 链表
 * 所有 add_disk() 注册的磁盘链入此表，del_gendisk() 移除。
 */
static LIST_HEAD(disk_list);

/*
 * bdev_list - 全局 block_device 链表
 * bdget() 分配的设备链入此表，bdput() 计数归零时移除并释放。
 */
static LIST_HEAD(bdev_list);

/* 子系统初始化标志（防止重复初始化） */
static int blk_initialized = 0;

/* ======================== block_dev_init ======================== */

/*
 * block_dev_init - 块设备子系统初始化
 *
 * 清空 blkdevs[] 注册表，初始化 disk_list / bdev_list 链表头。
 * 必须在 kmem_cache_init() 之后（kmalloc 可用）、ata_init() 之前调用。
 */
void block_dev_init(void)
{
    int i;
    if (blk_initialized)
        return;

    for (i = 0; i < MAX_BLKDEV; i++) {
        blkdevs[i].name = NULL;
        blkdevs[i].fops = NULL;
    }

    /* INIT_LIST_HEAD 已在静态初始化时完成（LIST_HEAD 宏），此处重置以防重入 */
    INIT_LIST_HEAD(&disk_list);
    INIT_LIST_HEAD(&bdev_list);

    blk_initialized = 1;
    printk("block: block device subsystem initialized (max %d majors)\n",
           MAX_BLKDEV);
}

/* ======================== register_blkdev ======================== */

/*
 * register_blkdev - 注册主设备号
 *
 * 流程（参考 Linux 2.6.20 fs/block_dev.c __register_blkdev）：
 *   1. major = 0 → 动态分配：从 1 扫描到 MAX_BLKDEV-1，取首个空槽
 *   2. major != 0 → 静态注册：检查该槽位是否已被占用
 *   3. 填写 blkdevs[major].name 和 .fops
 *   4. 返回实际分配的主设备号；失败返回 0
 */
int register_blkdev(unsigned int major, const char *name,
                    struct block_device_operations *fops)
{
    if (!name || !fops)
        return 0;

    if (!blk_initialized) {
        printk("block: ERROR - block_dev_init() not called\n");
        return 0;
    }

    /* 动态分配：扫描首个可用槽位（跳过 0，0 保留为无效值） */
    if (major == 0) {
        for (major = 1; major < MAX_BLKDEV; major++) {
            if (blkdevs[major].name == NULL)
                break;
        }
        if (major >= MAX_BLKDEV) {
            printk("block: register_blkdev: no free major for '%s'\n", name);
            return 0;
        }
    } else {
        /* 静态注册：检查是否越界或已被占用 */
        if (major >= MAX_BLKDEV) {
            printk("block: register_blkdev: major %u out of range\n", major);
            return 0;
        }
        if (blkdevs[major].name != NULL) {
            printk("block: register_blkdev: major %u already taken ('%s')\n",
                   major, blkdevs[major].name);
            return 0;
        }
    }

    blkdevs[major].name = name;
    blkdevs[major].fops = fops;

    printk("block: registered '%s' as major %u\n", name, major);
    return (int)major;
}

/*
 * unregister_blkdev - 注销主设备号
 *
 * 校验名称匹配后清零槽位，防止误注销其他驱动注册的主设备号。
 */
int unregister_blkdev(unsigned int major, const char *name)
{
    if (major == 0 || major >= MAX_BLKDEV)
        return -1;
    if (blkdevs[major].name == NULL)
        return -1;
    if (name && strcmp(blkdevs[major].name, name) != 0) {
        printk("block: unregister_blkdev: name mismatch for major %u "
               "('%s' vs '%s')\n",
               major, blkdevs[major].name, name);
        return -1;
    }

    blkdevs[major].name = NULL;
    blkdevs[major].fops = NULL;
    printk("block: unregistered major %u\n", major);
    return 0;
}

/* ======================== add_disk / del_gendisk ======================== */

/*
 * add_disk - 将 gendisk 注册到全局链表
 *
 * 流程（参考 Linux 2.6.20 block/genhd.c add_disk）：
 *   1. 校验 disk->major 已注册（blkdevs[] 中有对应条目）
 *   2. 链入 disk_list
 *   3. 调用 bdget() 为整盘（first_minor）创建 block_device
 *      → 建立 bdev->bd_disk = disk 反向引用
 *   4. 打印磁盘容量信息
 *
 * 注意：分区 block_device 不在此处创建，
 *       由 Task 5 partition.c 解析 MBR 后调用 add_partition() 创建。
 */
void add_disk(struct gendisk *disk)
{
    if (!disk)
        return;

    if (disk->major <= 0 || disk->major >= MAX_BLKDEV) {
        printk("block: add_disk: invalid major %d for '%s'\n",
               disk->major, disk->disk_name);
        return;
    }

    if (blkdevs[disk->major].name == NULL) {
        printk("block: add_disk: major %d not registered (call register_blkdev first)\n",
               disk->major);
        return;
    }

    /* 链入全局 gendisk 表 */
    list_add_tail(&disk->disk_list, &disk_list);

    /*
     * 为整盘创建 block_device
     * dev = MKDEV(major, first_minor)，MINOR=0 代表整盘
     */
    struct block_device *bdev = bdget(MKDEV(disk->major, disk->first_minor));
    if (bdev) {
        bdev->bd_disk = disk;
        printk("block: disk '%s' added  major=%d  capacity=%lu sectors (%lu MB)\n",
               disk->disk_name, disk->major,
               disk->capacity, disk->capacity / 2048);
    } else {
        printk("block: add_disk: failed to alloc bdev for '%s'\n",
               disk->disk_name);
    }
}

/*
 * del_gendisk - 注销 gendisk
 *
 * 从 disk_list 移除，并释放对应的整盘 block_device。
 * 分区 block_device 需在此之前由调用方（partition.c）清理。
 */
void del_gendisk(struct gendisk *disk)
{
    if (!disk)
        return;

    /* 从全局链表移除 */
    list_del(&disk->disk_list);

    /* 释放整盘 block_device（按 major:first_minor 查找） */
    struct block_device *bdev, *tmp;
    dev_t whole_dev = MKDEV(disk->major, disk->first_minor);
    list_for_each_entry_safe(bdev, tmp, &bdev_list, bd_list) {
        if (bdev->bd_dev == whole_dev) {
            list_del(&bdev->bd_list);
            kfree(bdev);
            break;
        }
    }

    printk("block: disk '%s' removed\n", disk->disk_name);
}

/* ======================== bdget / bdput ======================== */

/*
 * bdget - 按设备号查找或分配 block_device
 *
 * 查找流程（参考 Linux 2.6.20 fs/block_dev.c bdget）：
 *   1. 遍历 bdev_list，匹配 bd_dev
 *   2. 命中：增加 bd_openers 计数并返回
 *   3. 未命中：kmalloc 分配，初始化，链入 bdev_list
 *      → bd_disk 暂设为 NULL，由 add_disk() 或后续代码填充
 *      → 若该 MAJOR 未注册（blkdevs[] 为空），分配失败返回 NULL
 *
 * 返回：block_device *，NULL 表示分配失败或 MAJOR 未注册
 */
struct block_device *bdget(dev_t dev)
{
    unsigned int major = MAJOR(dev);
    struct block_device *bdev;

    /* MAJOR 合法性检查 */
    if (major == 0 || major >= MAX_BLKDEV || blkdevs[major].name == NULL) {
        printk("block: bdget: major %u not registered\n", major);
        return NULL;
    }

    /* 在已有 block_device 中查找 */
    list_for_each_entry(bdev, &bdev_list, bd_list) {
        if (bdev->bd_dev == dev) {
            bdev->bd_openers++;
            return bdev;
        }
    }

    /* 未找到：分配新的 block_device */
    bdev = (struct block_device *)kmalloc(sizeof(struct block_device),
                                          GFP_KERNEL);
    if (!bdev) {
        printk("block: bdget: kmalloc failed for dev %u:%u\n",
               MAJOR(dev), MINOR(dev));
        return NULL;
    }

    memset(bdev, 0, sizeof(struct block_device));
    bdev->bd_dev     = dev;
    bdev->bd_disk    = NULL;  /* 由调用方（add_disk/add_partition）填充 */
    bdev->bd_openers = 1;
    INIT_LIST_HEAD(&bdev->bd_list);

    /* 链入全局 bdev_list */
    list_add_tail(&bdev->bd_list, &bdev_list);

    return bdev;
}

/*
 * bdput - 释放 block_device
 *
 * 减少 bd_openers，计数归零时从链表移除并 kfree。
 * 参考 Linux 2.6.20 fs/block_dev.c bdput（kobject_put 路径）。
 */
void bdput(struct block_device *bdev)
{
    if (!bdev)
        return;

    bdev->bd_openers--;
    if (bdev->bd_openers <= 0) {
        list_del(&bdev->bd_list);
        kfree(bdev);
    }
}

/* ======================== get_gendisk ======================== */

/*
 * get_gendisk - 按设备号查找对应 gendisk
 *
 * 遍历 disk_list，匹配 major == MAJOR(dev) 且
 * minor 在 [first_minor, first_minor + minors) 范围内。
 *
 * 参考 Linux 2.6.20 block/genhd.c get_gendisk（idr 查找），
 * 此处线性扫描，磁盘数 < 10，性能不是问题。
 */
struct gendisk *get_gendisk(dev_t dev)
{
    unsigned int major = MAJOR(dev);
    unsigned int minor = MINOR(dev);
    struct gendisk *disk;

    list_for_each_entry(disk, &disk_list, disk_list) {
        if ((unsigned int)disk->major == major &&
            minor >= (unsigned int)disk->first_minor &&
            minor <  (unsigned int)(disk->first_minor + disk->minors)) {
            return disk;
        }
    }

    return NULL;
}

/* ======================== submit_bh ======================== */

/*
 * submit_bh - 同步块 I/O 提交
 *
 * 流程（简化版，参考 Linux 2.6.20 block/ll_rw_blk.c submit_bh）：
 *   1. 校验 bdev 与 fops->strategy 是否存在
 *   2. 校验 sector + count 不超出磁盘容量（capacity）
 *   3. 直接调用 fops->strategy(bdev, sector, count, buf, write)
 *   4. 返回 strategy 的结果（0 成功，负数错误码）
 *
 * Linux 原始路径（完整路径，本版本暂未实现）：
 *   submit_bh()
 *     → generic_make_request(bio)
 *       → q->make_request_fn(q, bio)     // request_queue 入口
 *         → elevator dispatch → driver request_fn
 *
 * LulaOS 当前路径：
 *   submit_bh() → strategy()  同步阻塞，无队列，无 bio 拆分。
 *
 * Task 4（buffer cache）接入后，bread() 调用 submit_bh()，
 * bh->b_data 作为 buf，bh->b_blocknr * (blocksize/512) 作为 sector。
 */
int submit_bh(struct block_device *bdev,
              unsigned long sector, unsigned int count,
              void *buf, int write)
{
    if (!bdev) {
        printk("block: submit_bh: NULL bdev\n");
        return -6;  /* -ENXIO */
    }

    struct gendisk *disk = bdev->bd_disk;
    if (!disk || !disk->fops || !disk->fops->strategy) {
        printk("block: submit_bh: no strategy for dev %u:%u\n",
               MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
        return -19; /* -ENODEV */
    }

    /* 容量边界检查：sector + count 不能超出 capacity */
    if (sector + count > disk->capacity) {
        printk("block: submit_bh: I/O beyond capacity "
               "(sector=%lu + count=%u > capacity=%lu)\n",
               sector, count, disk->capacity);
        return -5;  /* -EIO */
    }

    if (!buf || count == 0)
        return -22; /* -EINVAL */

    /* 同步调用驱动的 strategy 回调 */
    return disk->fops->strategy(bdev, sector, count, buf, write);
}
