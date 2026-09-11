/*
 * LulaOS ATA 探测层 + 块设备接入
 *
 * 参考：
 *   drivers/ata/ata_piix.c   — PCI ID table + probe（ata_piix_init_one）
 *   drivers/ide/ide.c        — 块设备注册（blk_register_region + add_disk）
 *   block/genhd.c            — add_disk 后的分区发现（register_disk）
 *
 * 职责（与 kernel/ata/ata.c 传输层分离）：
 *   1. 全局状态：ata_hosts[2]（通道）、ata_devices[2][2]（设备）
 *   2. PCI probe：使能控制器 → 初始化通道 → IDENTIFY 枚举 → SET FEATURES
 *   3. 驱动层自测：ata_read_mbr（DMA/PIO 一致性验证）
 *   4. 块设备接入 
 *      register_blkdev(3, "hd") → add_disk(hd0~hd3) → partition_scan
 *
 * 主设备号 3 = Linux "hd" 传统编号；每盘 minors=16（整盘 + 15 分区）。
 * 分区（hd0p1 等）由 kernel/block/partition.c 解析 MBR 创建，
 * submit_bh 层完成 分区相对扇区 → 整盘 LBA 的重映射。
 */

#include <ata/ata.h>
#include <pci/pci.h>
#include <block/blkdev.h>
#include <printk.h>
#include <libs/string.h>
#include <stddef.h>

/* ======================== 全局状态 ======================== */

/* 两个通道：0=Primary, 1=Secondary */
struct ata_host  ata_hosts[2];

/* 每个通道最多 2 个设备：[ch][drive] */
struct ata_device ata_devices[2][2];

/* 防重入标志：只允许第一个 IDE 控制器绑定，忽略后续发现的控制器 */
static int ata_initialized = 0;

/* ======================== 块设备接入（Task 5） ======================== */

#define ATA_DISK_MAJOR      3       /* Linux 习惯：major 3 = hd */
#define ATA_MINORS_PER_DISK 16      /* 整盘(MINOR 0) + 最多 15 分区 */

/* 2 通道 × 主从 = 最多 4 块盘（hd0 ~ hd3），生命周期与内核相同 */
static struct gendisk ata_disks[4];

/*
 * ata_strategy - 块设备 I/O 策略回调
 *
 * 由 submit_bh() 同步调用，sector 已是整盘 LBA（分区重映射在块层完成）。
 * private_data 指向 ata_device，按 DMA 能力选择传输路径
 * （与 ata_read_mbr 的 DMA 优先 + PIO 回退策略一致）。
 *
 * 返回：0 成功，负数错误码（-ENODEV / -EINVAL）
 */
static int ata_strategy(struct block_device *bdev,
                        unsigned long sector, unsigned int count,
                        void *buf, int write)
{
    struct ata_device *dev;
    struct ata_host *host;

    if (!bdev || !bdev->bd_disk)
        return -19; /* -ENODEV */

    dev = (struct ata_device *)bdev->bd_disk->private_data;
    if (!dev || !dev->present)
        return -19; /* -ENODEV */

    host = dev->host;

    /* ATA NSECTORS 寄存器 8 位：单次传输最多 255 扇区 */
    if (count == 0 || count > 255)
        return -22; /* -EINVAL */

    if (dev->dma_ok) {
        if (write)
            return ata_dma_write_sectors(host, dev->drive,
                                         (unsigned int)sector,
                                         (unsigned char)count, buf);
        return ata_dma_read_sectors(host, dev->drive,
                                    (unsigned int)sector,
                                    (unsigned char)count, buf);
    }

    if (write)
        return ata_pio_write_sectors(host, dev->drive,
                                     (unsigned int)sector,
                                     (unsigned char)count, buf);
    return ata_pio_read_sectors(host, dev->drive,
                                (unsigned int)sector,
                                (unsigned char)count, buf);
}

static struct block_device_operations ata_fops = {
    .strategy = ata_strategy,
    .ioctl    = NULL,
    .getgeo   = NULL,
};

/*
 * ata_blk_register_disks - 将枚举到的 ATA 设备接入块设备层
 *
 * 流程（参考 Linux drivers/ide/ide-probe.c ide_genesis + add_disk）：
 *   1. register_blkdev(3, "hd")：注册主设备号与操作表
 *   2. 遍历 ata_devices，对每个 present 设备：
 *      填充 gendisk（major/first_minor/minors/capacity/fops/private_data）
 *      → add_disk()（创建整盘 block_device）
 *      → partition_scan()（读 MBR 创建分区 block_device）
 *
 * first_minor = disk_no * 16：hd0=0, hd1=16, hd2=32, hd3=48，
 * 每盘保留 [first_minor, first_minor+16) 共 16 个次设备号。
 */
static void ata_blk_register_disks(void)
{
    int ch, drive, disk_no = 0;

    /* 注册主设备号 3（Linux hd 传统编号） */
    if (register_blkdev(ATA_DISK_MAJOR, "hd", &ata_fops) != ATA_DISK_MAJOR) {
        printk("ATA: failed to register major %d, block layer disabled\n",
               ATA_DISK_MAJOR);
        return;
    }

    for (ch = 0; ch < 2; ch++) {
        for (drive = 0; drive < 2; drive++) {
            struct gendisk *disk;

            if (!ata_devices[ch][drive].present)
                continue;

            disk = &ata_disks[disk_no];
            memset(disk, 0, sizeof(*disk));
            disk->major        = ATA_DISK_MAJOR;
            disk->first_minor  = disk_no * ATA_MINORS_PER_DISK;
            disk->minors       = ATA_MINORS_PER_DISK;
            disk->capacity     = ata_devices[ch][drive].sectors;
            disk->fops         = &ata_fops;
            disk->private_data = &ata_devices[ch][drive];

            /* disk_name: "hd0" ~ "hd3" */
            disk->disk_name[0] = 'h';
            disk->disk_name[1] = 'd';
            disk->disk_name[2] = (char)('0' + disk_no);
            disk->disk_name[3] = '\0';

            /* 注册整盘并解析分区表（MBR → hd0p1 等） */
            add_disk(disk);
            partition_scan(disk);

            disk_no++;
        }
    }
}

/* ======================== MBR 读取自测 ======================== */

/*
 * ata_read_mbr - 读取 Master Boot Record（LBA 0）并打印验证信息
 *
 * MBR 位于磁盘第一个扇区（LBA 0），共 512 字节：
 *   偏移 0x000 ~ 0x1BD：引导代码 + 保留区
 *   偏移 0x1BE ~ 0x1FD：4 个 16 字节分区表项
 *   偏移 0x1FE ~ 0x1FF：引导签名 0x55AA
 *
 * 读取策略：设备支持 DMA 时优先 DMA，否则 PIO 回退
 * （与原 probe 内联验证逻辑一致）。
 *
 * verbose=1 时打印引导签名与前 16 字节十六进制转储；
 * DMA 读取成功后追加一次 PIO 读取对比，验证两种传输方式的一致性。
 *
 * @host:    ATA 通道（提供 I/O 基址）
 * @dev:     目标设备（提供 drive 号、present/dma_ok 状态）
 * @verbose: 1=打印读取结果详情，0=静默读取
 * 返回：0 成功，-1 失败（设备不存在或读取失败）
 */
int ata_read_mbr(struct ata_host *host, struct ata_device *dev, int verbose)
{
    unsigned char mbr[512];
    unsigned short sig;
    int ret;

    if (!dev->present)
        return -1;

    if (verbose)
        printk("ATA: attempting MBR read (LBA 0, mode=%s)...\n",
               dev->dma_ok ? "DMA" : "PIO");

    /* DMA 优先，设备不支持 DMA 则 PIO */
    if (dev->dma_ok)
        ret = ata_dma_read_sectors(host, dev->drive, 0, 1, mbr);
    else
        ret = ata_pio_read_sectors(host, dev->drive, 0, 1, mbr);

    if (ret != 0) {
        if (verbose)
            printk("ATA: MBR read FAILED [%s]\n",
                   dev->dma_ok ? "DMA" : "PIO");
        return -1;
    }

    if (!verbose)
        return 0;

    /* 检查引导签名（偏移 510~511 应为 0x55 0xAA） */
    sig = (unsigned short)(mbr[511] << 8 | mbr[510]);
    printk("ATA: MBR read OK [%s], signature=0x%04X\n",
           dev->dma_ok ? "DMA" : "PIO", sig);
    printk("ATA: MBR first 16 bytes:");
    {
        int i;
        for (i = 0; i < 16; i++)
            printk(" %02x", mbr[i]);
    }
    printk("\n");

    /* DMA 模式下追加一次 PIO 对比，验证两者一致性 */
    if (dev->dma_ok) {
        unsigned char mbr_pio[512];
        if (ata_pio_read_sectors(host, dev->drive, 0, 1, mbr_pio) == 0) {
            unsigned int diff = 0, i;
            for (i = 0; i < 512; i++)
                if (mbr[i] != mbr_pio[i]) diff++;
            printk("ATA: DMA vs PIO consistency check: %s (%u bytes differ)\n",
                   diff == 0 ? "MATCH" : "MISMATCH", diff);
        }
    }

    return 0;
}

/* ======================== PCI 驱动注册 ======================== */

/*
 * ata_pci_id_table - 支持的 PCI 设备 ID 表
 *
 * 参考 Linux drivers/ata/ata_piix.c piix_pata_mwdma
 */
static const struct pci_device_id ata_pci_id_table[] = {
    { .vendor = 0x8086, .device = 0x7010, .class = 0 },  /* PIIX3 IDE */
    { .vendor = 0x8086, .device = 0x7111, .class = 0 },  /* PIIX4 IDE */
    { .vendor = 0,      .device = 0,      .class = 0x010100 }, /* 任意 IDE 控制器 */
    { 0 },
};

/*
 * ata_pci_probe - PCI 设备 probe 回调
 *
 * 参考 ata_piix.c ata_piix_init_one()：
 *   1. 使能 PCI 设备（I/O + MEM + Bus Master）
 *   2. 设置 Primary 和 Secondary 通道的 Legacy I/O 端口
 *   3. 对每个通道的 Master/Slave 执行 IDENTIFY
 *   4. 验证测试：读取 MBR (sector 0) 打印签名
 *   5. 接入块设备层：register_blkdev + add_disk + partition_scan
 */
static int ata_pci_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    int ch, drive;
    unsigned int bar4_raw;

    (void)id;

    /* 防重入：全局 ata_hosts/ata_devices 只容纳一个控制器，忽略后续发现的 */
    if (ata_initialized) {
        printk("ATA: ignoring additional IDE controller at %02x:%02x.%x\n",
               pdev->bus, pdev->devfn >> 3, pdev->devfn & 7);
        return -1;
    }
    ata_initialized = 1;

    printk("ATA: found IDE controller at %02x:%02x.%x [%04x:%04x]\n",
           pdev->bus, pdev->devfn >> 3, pdev->devfn & 7,
           pdev->vendor, pdev->device);

    /*
     * 使能 I/O 空间 + Bus Master(总线控制)
     *
     * Bus Master DMA 需要 PCI Command 寄存器 bit2=1（PCI_COMMAND_MASTER）。
     * 不使能则 BM Command 寄存器的 START 位无效。
     */
    pci_enable_device(pdev);
    {
        unsigned int cmd;
        cmd = pci_config_read32(pdev->bus, pdev->devfn, PCI_COMMAND);
        cmd |= PCI_COMMAND_MASTER;
        pci_config_write32(pdev->bus, pdev->devfn, PCI_COMMAND, cmd);
        printk("ATA: PCI command register = 0x%04x (Bus Master enabled)\n",
               cmd & 0xFFFF);
    }

    /*
     * 读取 BAR4：Bus Master IDE I/O 基址
     *
     * PIIX3 IDE 的 BAR4（PCI config offset 0x20）存放 BM I/O 端口基址。
     * bit0=1 表示 I/O 空间，bit1=0（IDE BM 不使用 mem 空间）。
     * 实际基址 = bar4 & 0xFFFC（去掉低 2 位）。
     */
    bar4_raw = pci_config_read32(pdev->bus, pdev->devfn, ATA_PCI_BM_BAR);
    printk("ATA: BAR4 (IDE BM) raw = 0x%08x\n", bar4_raw);

    /* 初始化 Primary 通道 */
    ata_hosts[0].pdev      = pdev;
    ata_hosts[0].io_base   = ATA_PRIMARY_IO;
    ata_hosts[0].ctrl_base = ATA_PRIMARY_CTRL;
    ata_hosts[0].irq       = ATA_PRIMARY_IRQ;
    ata_hosts[0].present   = 0;
    ata_hosts[0].bm_base   = (unsigned short)(bar4_raw & 0xFFFC);
    ata_hosts[0].dma_ok    = (bar4_raw & 0x01) && (ata_hosts[0].bm_base != 0);
    ata_hosts[0].prd_table = NULL;
    ata_hosts[0].prd_phys  = 0;

    /* 初始化 Secondary 通道 */
    ata_hosts[1].pdev      = pdev;
    ata_hosts[1].io_base   = ATA_SECONDARY_IO;
    ata_hosts[1].ctrl_base = ATA_SECONDARY_CTRL;
    ata_hosts[1].irq       = ATA_SECONDARY_IRQ;
    ata_hosts[1].present   = 0;
    ata_hosts[1].bm_base   = (unsigned short)(bar4_raw & 0xFFFC);
    ata_hosts[1].dma_ok    = (bar4_raw & 0x01) && (ata_hosts[1].bm_base != 0);
    ata_hosts[1].prd_table = NULL;
    ata_hosts[1].prd_phys  = 0;

    /* DMA 状态总览 */
    printk("ATA: Bus Master IDE base = 0x%04x, dma_ok=%d\n",
           ata_hosts[0].bm_base, ata_hosts[0].dma_ok);

    /* 清零设备表 */
    {
        unsigned char *p = (unsigned char *)ata_devices;
        unsigned int i;
        for (i = 0; i < sizeof(ata_devices); i++)
            p[i] = 0;
    }

    /* 初始化 DMA：为每个通道分配 PRD 表（BAR4 有效时） */
    ata_dma_init_channel(&ata_hosts[0]);
    ata_dma_init_channel(&ata_hosts[1]);

    /* 枚举所有通道的所有驱动器 */
    for (ch = 0; ch < 2; ch++) {
        for (drive = 0; drive < 2; drive++) {
            ata_identify(&ata_hosts[ch], (unsigned char)drive,
                         &ata_devices[ch][drive]);

            /*
             * 驱动器支持 DMA 时，发送 SET FEATURES 启用 MWDMA2。
             * 失败则 dev->dma_ok 保持 0，MBR 测试自动回退到 PIO。
             */
            if (ata_devices[ch][drive].dma_ok) {
                if (ata_set_dma_mode(&ata_devices[ch][drive],
                                     ATA_XFER_MWDMA2) != 0) {
                    printk("ATA: DMA mode rejected on ch%d drive%d, "
                           "falling back to PIO\n", ch, drive);
                    ata_devices[ch][drive].dma_ok = 0;
                }
            }
        }
    }

    /* 验证测试：读取 MBR（LBA 0） */
    printk("ATA: scan complete. ch0-drive0 present=%d dma=%d\n",
           ata_devices[0][0].present, ata_devices[0][0].dma_ok);
    if (ata_devices[0][0].present)
        ata_read_mbr(&ata_hosts[0], &ata_devices[0][0], 1);

    /* 接入块设备层：hd0 ~ hd3 + MBR 分区解析 */
    ata_blk_register_disks();

    return 0;
}

/* PCI 驱动描述符 */
static struct pci_driver ata_pci_driver = {
    .driver = {
        .name = "ata_piix",
    },
    .id_table = ata_pci_id_table,
    .probe    = ata_pci_probe,
    .remove   = NULL,
};

/* ======================== 初始化入口 ======================== */

/*
 * ata_init - ATA 子系统初始化
 *
 * 注册 PCI 驱动，driver_register() 内部调用 driver_attach()
 * 遍历已枚举的 PCI 设备，自动匹配 PIIX3/PIIX4 并调用 probe。
 */
void ata_init(void)
{
    printk("ATA: initializing subsystem\n");
    pci_register_driver(&ata_pci_driver);
}
