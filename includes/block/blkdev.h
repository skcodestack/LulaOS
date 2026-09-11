/*
 * blkdev.h - LulaOS 块设备核心抽象
 *
 * 参考 Linux 2.6.20：
 *   include/linux/genhd.h     — gendisk, hd_struct, dev_t, MAJOR/MINOR
 *   include/linux/fs.h        — block_device, block_device_operations
 *   include/linux/blkdev.h    — submit_bh, 请求队列（本版本暂未实现）
 *
 * 简化设计（配合 ext2 文件系统任务链）：
 *   - dev_t 使用 Linux 标准 12:20 位编码（MAJOR 12 位 / MINOR 20 位）
 *   - gendisk 通过静态数组注册，由 MAJOR 索引查找
 *   - submit_bh 同步直调 drive->strategy()，不做请求队列与电梯调度
 *     （后续 Task 可迭代引入 request_queue + CFQ）
 *   - 分区支持：hd_struct 描述分区，bd_part 挂接分区视图，
 *     submit_bh 完成相对扇区 → 整盘 LBA 的重映射（驱动无感知）
 */

#ifndef __BLKDEV_H__
#define __BLKDEV_H__

#include <libs/list.h>

/* ======================== dev_t：设备号编码 ======================== */

/*
 * Linux   设备号布局（新编码，2.6 起使用）：
 *   bits [31:20]  = MAJOR（12 位，最多 4096 个主设备号）
 *   bits [19:0]   = MINOR（20 位，最多 1048576 个次设备号）
 *
 * 主设备号标识驱动类型（如 3=hd, 8=sd, 259=blkext），
 * 次设备号标识具体分区（0=整盘，1~N=分区）。
 */
typedef unsigned long dev_t;

#define MINORBITS       20
#define MINORMASK       ((1U << MINORBITS) - 1)

#define MAJOR(dev)      ((unsigned int)((dev) >> MINORBITS))
#define MINOR(dev)      ((unsigned int)((dev) & MINORMASK))
#define MKDEV(ma, mi)   (((dev_t)(ma) << MINORBITS) | (mi))

/* ======================== 常量 ======================== */

#define MAX_BLKDEV      64      /* 最多注册 64 个主设备号 */
#define DISK_NAME_LEN   32      /* 磁盘名最大长度（如 "hd0", "hd0p1"） */

/* ======================== 前向声明 ======================== */

struct block_device;
struct gendisk;

/* ======================== hd_struct（分区描述） ======================== */

/*
 * hd_struct - 磁盘分区描述符
 *
 * 参考 Linux include/linux/genhd.h struct hd_struct（简化）：
 *   - start_sect：分区第一个扇区的整盘 LBA（512B 单位）
 *   - nr_sects：  分区扇区数（512B 单位）
 *
 * 每个"分区" block_device 通过 bd_part 指向一个 hd_struct；
 * 整盘 block_device 的 bd_part 恒为 NULL。
 *
 * 分区重映射在 submit_bh 层完成（相对扇区 + start_sect → 整盘 LBA），
 * 驱动 strategy 只认整盘 LBA，无需感知分区存在。
 *
 * 生命周期：add_partition() 中 kmalloc，delete_partition() 中 kfree；
 * 分区 bdev 长期持有此结构（生命周期 = 磁盘生命周期）。
 */
struct hd_struct {
    unsigned long start_sect;   /* 分区起始扇区（整盘 LBA，512B 单位） */
    unsigned long nr_sects;     /* 分区扇区数（512B 单位） */
};

/* ======================== block_device_operations ======================== */

/*
 * block_device_operations - 块设备驱动操作表
 *
 * 由驱动（如 ATA/SATA）填充并注册，VFS/buffer cache 通过此表调用驱动。
 *
 * strategy(bdev, sector, count, buf, write)：
 *   核心 I/O 入口。同步执行，返回 0 表示成功，负数表示错误。
 *   @bdev:   目标块设备（携带 dev_t 与 gendisk 反向引用）
 *   @sector: 起始逻辑扇区号（LBA，512 字节单位；分区已由块层重映射为整盘 LBA）
 *   @count:  扇区数（1~255，PIO 单次传输上限）
 *   @buf:    数据缓冲区（至少 count * 512 字节）
 *   @write:  0=读，1=写
 *
 * ioctl/getgeo：后续扩展（mount/ioctl 系统调用接入时使用）
 */
struct block_device_operations {
    int  (*strategy)(struct block_device *bdev,
                     unsigned long sector, unsigned int count,
                     void *buf, int write);
    int  (*ioctl)(struct block_device *bdev,
                  unsigned int cmd, unsigned long arg);
    int  (*getgeo)(struct block_device *bdev, void *geo);
    /* struct module *owner; */  /* 暂不实现模块引用计数 */
};

/* ======================== block_device ======================== */

/*
 * block_device - 已打开块设备实例
 *
 * 每个 (MAJOR, MINOR) 对应一个 block_device。
 * bdget() 负责查找或分配，brelse_dev() 释放。
 *
 * 与 Linux  的区别：
 *   - 不含 inode/bdev_inode 反向引用（VFS 接入  ）
 *   - 不含 bd_mutex（单线程内核，无需锁）
 *   - bd_openers 简化为打开计数
 *
 * 分区语义：
 *   整盘 bdev（MINOR=first_minor）  ：bd_part == NULL，I/O 直达整盘 LBA
 *   分区 bdev（MINOR=first_minor+N）：bd_part 指向 hd_struct，
 *                                     submit_bh 自动重映射 + 分区边界校验
 */
struct block_device {
    dev_t               bd_dev;     /* 设备号（MAJOR+MINOR） */
    struct gendisk     *bd_disk;    /* 指向所属 gendisk */
    struct hd_struct   *bd_part;    /* 分区描述（整盘为 NULL，分区指向 hd_struct） */
    struct list_head    bd_list;    /* 链入全局 bdev_list */
    int                 bd_openers; /* 打开计数 */
};

/* ======================== gendisk ======================== */

/*
 * gendisk - 通用磁盘描述符
 *
 * 每个物理磁盘（如 hd0, sd0）注册一个 gendisk，
 * 分区（hd0p1 等）通过 part[] 数组或直接 MINOR 偏移描述。
 *
 * capacity：以 512 字节扇区为单位的总容量（整盘）。
 *            分区容量通过分区表解析后记录在 hd_struct 中。
 *
 * 参考 Linux 2.6.20 struct gendisk（drivers/block/genhd.c）：
 *   - major/minor：设备号，minor 为整盘的起始 MINOR（分区按步长递增）
 *   - first_minor：同 minor，保持兼容
 *   - minors：该驱动分配的次设备号个数（1=整盘无分区，>1 含分区）
 *   - disk_name：设备名（如 "hd0"），分区名为 "hd0p1"
 */
struct gendisk {
    int                  major;         /* 主设备号 */
    int                  first_minor;   /* 整盘起始次设备号 */
    int                  minors;        /* 分配的次设备号个数 */
    char                 disk_name[DISK_NAME_LEN];
    unsigned long        capacity;      /* 扇区总数（512B/sector） */
    struct block_device_operations *fops;
    struct list_head     disk_list;     /* 链入全局 disk_list */
    void                *private_data;  /* 驱动私有数据（如 ata_device *） */
};

/* ======================== 公共 API ======================== */

/*
 * block_dev_init - 块设备子系统初始化
 * 清空注册表与 gendisk/block_device 链表。
 * 由内核启动时（mm_init 之后、ata_init 之前）调用。
 */
void block_dev_init(void);

/*
 * register_blkdev - 注册主设备号
 * @major:  期望的主设备号（0 = 动态分配，返回实际分配值）
 * @name:   设备名（如 "hd", "sd"），用于 /proc/devices 展示
 * @fops:   该主设备号下所有磁盘共享的操作表
 * 返回：> 0 = 分配到的主设备号（动态分配时），0 = 失败
 *
 * 若 major != 0 且已被占用，返回 0（注册失败）。
 */
int register_blkdev(unsigned int major, const char *name,
                    struct block_device_operations *fops);

/*
 * unregister_blkdev - 注销主设备号
 * @major: 主设备号
 * @name:  设备名（用于校验，防止误注销）
 * 返回：0 成功，-1 未找到
 */
int unregister_blkdev(unsigned int major, const char *name);

/*
 * add_disk - 将 gendisk 注册到全局链表并创建对应 block_device
 * @disk: 填充完毕的 gendisk（major/fops/capacity 必须已设置）
 *
 * 内部调用 bdget(MKDEV(disk->major, disk->first_minor)) 分配整盘 block_device，
 * 并将 disk->bd_disk 反向引用建立起来，使后续 bread/submit_bh 可以通过
 * bdev->bd_disk->fops->strategy 找到驱动入口。
 *
 * 调用后应执行 partition_scan(disk) 解析分区表（创建分区 block_device）。
 */
void add_disk(struct gendisk *disk);

/*
 * del_gendisk - 注销 gendisk（释放 block_device 并从链表移除）
 * @disk: 要注销的 gendisk
 */
void del_gendisk(struct gendisk *disk);

/*
 * add_partition - 创建分区 block_device
 * @disk:       所属 gendisk（须已 add_disk）
 * @partno:     分区号（1 ~ disk->minors-1，0 保留给整盘）
 * @start_sect: 分区起始扇区（整盘 LBA，512B 单位）
 * @nr_sects:   分区扇区数
 * 返回：0 成功，-1 失败（参数非法 / 内存不足）
 *
 * 流程（参考 Linux block/genhd.c add_partition）：
 *   kmalloc hd_struct → bdget(MKDEV(major, first_minor+partno))
 *   → 填 bdev->bd_disk / bd_part → 分区 bdev 进入全局 bdev_list
 *
 * 分区 bdev 的引用由本函数长期持有（openers=1），
 * 由 delete_partition() 释放，不随 bread/bdput 释放。
 */
int add_partition(struct gendisk *disk, int partno,
                  unsigned long start_sect, unsigned long nr_sects);

/*
 * delete_partition - 删除分区 block_device 并释放 hd_struct
 * @disk:   所属 gendisk
 * @partno: 分区号（1 ~ disk->minors-1）
 */
void delete_partition(struct gendisk *disk, int partno);

/*
 * partition_scan - 读取并解析磁盘 MBR 分区表
 * @disk: 已 add_disk 的 gendisk
 *
 * 读取整盘 LBA0（经 buffer cache），校验 0x55AA 签名后遍历
 * 4 个主分区条目，对每个有效条目调用 add_partition()。
 * 实现在 kernel/block/partition.c。
 */
void partition_scan(struct gendisk *disk);

/*
 * bdget - 按设备号查找或分配 block_device
 * @dev: 设备号（MAJOR+MINOR）
 *
 * 查找顺序：遍历 bdev_list 匹配 bd_dev；
 * 未找到则从静态池分配，bd_disk 指向 MAJOR 对应的 gendisk。
 * 返回：block_device 指针，NULL 表示分配失败或 MAJOR 未注册。
 */
struct block_device *bdget(dev_t dev);

/*
 * bdput - 释放 block_device（减少打开计数，计数归零时释放）
 * @bdev: 要释放的 block_device
 */
void bdput(struct block_device *bdev);

/*
 * get_gendisk - 按设备号查找对应 gendisk
 * @dev: 设备号（仅用 MAJOR 部分索引）
 * 返回：gendisk 指针，NULL 表示该主设备号未注册
 */
struct gendisk *get_gendisk(dev_t dev);

/*
 * submit_bh - 同步块 I/O 提交（简化路径）
 * @bdev:   目标块设备
 * @sector: 起始扇区号（512B 单位，LBA；分区 bdev 传分区相对扇区）
 * @count:  扇区数
 * @buf:    数据缓冲区（至少 count * 512 字节）
 * @write:  0=读，1=写
 *
 * 分区重映射（关键语义）：
 *   bdev->bd_part 非空时：先按 nr_sects 校验，再 sector += start_sect，
 *   驱动收到的是整盘 LBA；整盘 bdev 走原有 capacity 校验。
 *
 * 直接调用 bdev->bd_disk->fops->strategy()，同步阻塞直到完成。
 * 返回：0 成功，负数错误码（-EIO / -ENODEV 等）。
 *
 * 与 Linux   的区别：
 *   Linux 原始路径：submit_bh → generic_make_request → request_queue → elevator → driver
 *   LulaOS 简化路径：submit_bh → strategy()（同步，无队列）
 */
int submit_bh(struct block_device *bdev,
              unsigned long sector, unsigned int count,
              void *buf, int write);

/*
 * blkdev_get_capacity - 获取块设备容量（扇区数，分区感知）
 * @bdev: 块设备
 * 返回：容量（扇区数），0 表示设备无效
 *
 * 分区 bdev：返回 hd_struct.nr_sects（分区容量）
 * 整盘 bdev：返回 gendisk.capacity（整盘容量）
 */
static inline unsigned long blkdev_get_capacity(struct block_device *bdev)
{
    if (!bdev)
        return 0;
    if (bdev->bd_part)
        return bdev->bd_part->nr_sects;
    if (!bdev->bd_disk)
        return 0;
    return bdev->bd_disk->capacity;
}

#endif /* __BLKDEV_H__ */
