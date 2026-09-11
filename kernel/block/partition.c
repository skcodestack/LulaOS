/*
 * partition.c - MBR 分区表解析
 *
 * 参考 Linux：
 *   fs/partitions/msdos.c — msdos_partition() MBR 分区表解析
 *   block/genhd.c         — add_partition() 分区注册
 *
 * MBR（Master Boot Record）布局（LBA0，512 字节）：
 *   偏移 0x000~0x1BD：引导代码（446 字节，本实现忽略）
 *   偏移 0x1BE~0x1FD：4 个 16 字节分区表条目
 *   偏移 0x1FE~0x1FF：签名 0x55 0xAA
 *
 * 分区表条目（16 字节）：
 *   byte 0      ：引导标志（0x80=可引导，0x00=不可引导）
 *   byte 1~3    ：起始 CHS（忽略，仅用 LBA 字段）
 *   byte 4      ：分区类型（0x00=空，0x05/0x0F=扩展分区，0x83=Linux）
 *   byte 5~7    ：结束 CHS（忽略）
 *   byte 8~11   ：起始 LBA（小端 u32）
 *   byte 12~15  ：扇区数（小端 u32）
 *
 * 设计要点：
 *   - 读 MBR 走 bread()（buffer cache 统一缓存路径），同时验证
 *     buffer cache → submit_bh → 驱动 strategy 全链路
 *   - bh 以 bd_dev 为键：整盘 block0 与分区 block0 缓存天然隔离
 *   - 仅解析 4 个主分区，扩展分区（0x05/0x0F）跳过并提示
 *   - highmem 页 b_data=NULL 时临时 kmap（LulaOS 64MB 全为
 *     ZONE_NORMAL，此路径为防御性代码）
 */

#include <block/blkdev.h>
#include <block/buffer_head.h>      /* bread / brelse / BLOCK_SIZE */
#include <arch/x86/highmem.h>      /* kmap / kunmap */
#include <printk.h>

/* ======================== MBR 布局常量 ======================== */

#define MBR_SIG_OFFSET      510     /* 签名 0x55AA 偏移 */
#define MBR_PART_TABLE      0x1BE   /* 分区表起始偏移 */
#define MBR_PART_ENTRIES    4       /* 主分区条目数 */
#define MBR_ENTRY_SIZE      16      /* 每条目字节数 */

/* 分区类型 */
#define PART_TYPE_EMPTY     0x00    /* 空条目 */
#define PART_TYPE_EXTENDED  0x05    /* 扩展分区（CHS 寻址） */
#define PART_TYPE_EXT_LBA   0x0F    /* 扩展分区（LBA 寻址） */

/*
 * part_le32 - 读取小端 32 位无符号数（无对齐要求）
 *
 * 分区表条目内的 LBA/扇区数字段位于奇数偏移（0x1BE + i*16 + 8），
 * 不能保证 4 字节对齐，逐字节拼接最稳妥（x86 虽允许非对齐访问，
 * 但拼接写法无平台假设且明确小端语义）。
 */
static unsigned int part_le32(const unsigned char *p)
{
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

/* ======================== partition_scan ======================== */

/*
 * partition_scan - 读取并解析磁盘 MBR 分区表
 * @disk: 已 add_disk() 的 gendisk
 *
 * 流程（参考 Linux fs/partitions/msdos.c msdos_partition）：
 *   1. bdget 获取整盘 block_device（MINOR = first_minor）
 *   2. bread 读 LBA0（block 0，BLOCK_SIZE=1024 覆盖前 2 个扇区，
 *      MBR 位于块内前 512 字节）
 *   3. 校验签名 mbr[510]=0x55, mbr[511]=0xAA；
 *      失败视为无分区盘（GPT/裸盘），仅 printk 提示后返回
 *   4. 遍历 4 个分区条目：
 *      - type=0x00        → 空条目跳过
 *      - type=0x05 / 0x0F → 扩展分区跳过（仅支持 4 主分区）
 *      - 边界校验 start+nr <= disk->capacity，越界条目跳过并告警
 *      - add_partition(disk, i+1, start, nr) 创建分区 block_device
 *   5. brelse + bdput 释放引用
 *
 * 驱动侧应在 add_disk() 之后立即调用本函数。
 */
void partition_scan(struct gendisk *disk)
{
    struct block_device *bdev;
    struct buffer_head *bh;
    unsigned char *mbr;
    void *kaddr = NULL;
    int i;

    if (!disk)
        return;

    /* 获取整盘 block_device（MINOR = first_minor，0=整盘） */
    bdev = bdget(MKDEV(disk->major, disk->first_minor));
    if (!bdev) {
        printk("partition: '%s': no whole-disk bdev\n", disk->disk_name);
        return;
    }

    /*
     * 读 LBA0：block 0（BLOCK_SIZE=1024 → 覆盖扇区 0~1）
     * 走 bread 统一缓存路径，同时验证块层全链路：
     *   bread → __getblk → submit_bh(read) → ata/sata strategy
     */
    bh = bread(bdev, 0, BLOCK_SIZE);
    if (!bh) {
        printk("partition: '%s': failed to read MBR (LBA 0)\n",
               disk->disk_name);
        bdput(bdev);
        return;
    }

    /*
     * 定位 MBR 数据：
     *   normal 页：b_data 直接可用（页内偏移 = blocknr % BLOCKS_PER_PAGE）
     *   highmem 页：b_data=NULL，临时 kmap（用完立即释放）
     */
    if (bh->b_data) {
        mbr = (unsigned char *)bh->b_data;
    } else {
        unsigned long offset;

        kaddr = kmap(bh->b_page);
        if (!kaddr) {
            printk("partition: '%s': kmap failed for MBR page\n",
                   disk->disk_name);
            brelse(bh);
            bdput(bdev);
            return;
        }
        offset = (bh->b_blocknr % BLOCKS_PER_PAGE) * bh->b_size;
        mbr = (unsigned char *)kaddr + offset;
    }

    /* 校验 MBR 签名（偏移 510~511 = 0x55 0xAA） */
    if (mbr[MBR_SIG_OFFSET] != 0x55 || mbr[MBR_SIG_OFFSET + 1] != 0xAA) {
        printk("partition: '%s': no MBR signature (0x55AA), "
               "treating as whole disk\n",
               disk->disk_name);
        goto out;
    }

    /* 遍历 4 个主分区条目（偏移 0x1BE + i*16） */
    for (i = 0; i < MBR_PART_ENTRIES; i++) {
        unsigned char *ent = mbr + MBR_PART_TABLE + i * MBR_ENTRY_SIZE;
        unsigned char boot_flag = ent[0];
        unsigned char type      = ent[4];
        unsigned int  start_lba = part_le32(ent + 8);
        unsigned int  nr_sects  = part_le32(ent + 12);

        /* 空条目 */
        if (type == PART_TYPE_EMPTY)
            continue;

        /* 扩展分区：容器分区，内部还有二级分区表，本版本不支持 */
        if (type == PART_TYPE_EXTENDED || type == PART_TYPE_EXT_LBA) {
            printk("partition: '%s': entry %d is extended partition "
                   "(type 0x%02x), skipped (only 4 primary supported)\n",
                   disk->disk_name, i + 1, type);
            continue;
        }

        /* 边界校验：分区不得超出磁盘容量 */
        if (nr_sects == 0 ||
            (unsigned long)start_lba + nr_sects > disk->capacity) {
            printk("partition: '%s': entry %d beyond disk end "
                   "(start=%u sectors=%u capacity=%lu), skipped\n",
                   disk->disk_name, i + 1, start_lba, nr_sects,
                   disk->capacity);
            continue;
        }

        /* 注册分区（partno = 1~4） */
        if (add_partition(disk, i + 1, start_lba, nr_sects) == 0) {
            printk("%sp%d: start=%u sectors=%u (%u MB) type=%02x%s\n",
                   disk->disk_name, i + 1,
                   start_lba, nr_sects, nr_sects / 2048, type,
                   (boot_flag & 0x80) ? " bootable" : "");
        }
    }

out:
    if (kaddr)
        kunmap(bh->b_page);
    brelse(bh);
    bdput(bdev);
}
