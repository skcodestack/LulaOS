/*
 * LulaOS SATA 总线扫描 + 块设备接入
 *
 * 参考：
 *   Linux drivers/ata/ahci.c        — ahci_init_one() 末尾的设备枚举
 *   Linux drivers/scsi/sd.c         — sd_probe()：add_disk 接入块层
 *   Linux block/genhd.c             — add_disk 后的分区发现（register_disk）
 *
 * 职责（与 kernel/sata/sata.c 传输层分离）：
 *   1. sata_scan_host：遍历 PxSSTS/PxSIG 检测在线端口，
 *      发送 IDENTIFY（sata_identify，实现在 sata.c）填充容量/型号
 *   2. 块设备接入（Task 5）：
 *      register_blkdev(8, "sd") → add_disk(sd0~sd5) → partition_scan
 *
 * 主设备号 8 = Linux "sd" 传统编号；每盘 minors=16（整盘 + 15 分区），
 * first_minor = port_no * 16（磁盘编号与端口号一致，跳过 ATAPI 不留空洞）。
 * 分区（sd0p1 等）由 kernel/block/partition.c 解析 MBR 创建，
 * submit_bh 层完成 分区相对扇区 → 整盘 LBA 的重映射。
 */

#include <sata/sata.h>
#include <block/blkdev.h>
#include <printk.h>
#include <libs/string.h>
#include <stddef.h>

/* ======================== 块设备接入（Task 5） ======================== */

#define SATA_DISK_MAJOR      8       /* Linux 习惯：major 8 = sd */
#define SATA_MINORS_PER_DISK 16      /* 整盘(MINOR 0) + 最多 15 分区 */

/* 每端口一块盘（sd0 ~ sd5），生命周期与内核相同 */
static struct gendisk sata_disks[SATA_MAX_PORTS];

/*
 * sata_strategy - 块设备 I/O 策略回调
 *
 * 由 submit_bh() 同步调用，sector 已是整盘 LBA（分区重映射在块层完成）。
 * private_data 指向 sata_port，转调 LBA48 DMA 传输接口。
 *
 * 与 ata_strategy 的差异：
 *   - SATA 无 PIO 路径（AHCI 全 DMA），无需 DMA/PIO 分支
 *   - LBA48 接口：lba 参数为 unsigned long long，
 *     sector（32 位 unsigned long）隐式零扩展传入
 *   - count 上限：LBA48 计数 16 位（0 表示 65536，此处保守拒绝）；
 *     单 PRDT 4MB（8192 扇区）上限由传输层内部检查
 *
 * 返回：0 成功，负数错误码（-ENODEV / -EINVAL）
 */
static int sata_strategy(struct block_device *bdev,
                         unsigned long sector, unsigned int count,
                         void *buf, int write)
{
    struct sata_port *port;

    if (!bdev || !bdev->bd_disk)
        return -19; /* -ENODEV */

    port = (struct sata_port *)bdev->bd_disk->private_data;
    if (!port || !port->present)
        return -19; /* -ENODEV */

    /* LBA48 Count 16 位：单次传输最多 65535 扇区 */
    if (count == 0 || count > 65535)
        return -22; /* -EINVAL */

    if (write)
        return sata_write_sectors(port, sector, count, buf);
    return sata_read_sectors(port, sector, count, buf);
}

static struct block_device_operations sata_fops = {
    .strategy = sata_strategy,
    .ioctl    = NULL,
    .getgeo   = NULL,
};

/*
 * sata_blk_register_disks - 将扫描到的 SATA 磁盘接入块设备层
 *
 * 流程（参考 Linux drivers/scsi/sd.c sd_probe + add_disk）：
 *   1. register_blkdev(8, "sd")：注册主设备号与操作表
 *   2. 遍历 host->ports，对每个 present 且 type==0（磁盘）端口：
 *      填充 gendisk（major/first_minor/minors/capacity/fops/private_data）
 *      → add_disk()（创建整盘 block_device）
 *      → partition_scan()（读 MBR 创建分区 block_device）
 *
 * first_minor = port_no * 16：sd0=0, sd1=16, ... sd5=80，
 * 每盘保留 [first_minor, first_minor+16) 共 16 个次设备号。
 * ATAPI（type==1）与未知类型端口不注册（present 保持 0，双条件防御）。
 */
static void sata_blk_register_disks(struct sata_host *host)
{
    unsigned int port_no;

    /* 注册主设备号 8（Linux sd 传统编号） */
    if (register_blkdev(SATA_DISK_MAJOR, "sd", &sata_fops) != SATA_DISK_MAJOR) {
        printk("SATA: failed to register major %d, block layer disabled\n",
               SATA_DISK_MAJOR);
        return;
    }

    for (port_no = 0; port_no < SATA_MAX_PORTS; port_no++) {
        struct sata_port *port = &host->ports[port_no];
        struct gendisk *disk;

        if (!port->present || port->type != 0)
            continue;  /* 无设备或 ATAPI 光驱（暂不支持） */

        disk = &sata_disks[port_no];
        memset(disk, 0, sizeof(*disk));
        disk->major        = SATA_DISK_MAJOR;
        disk->first_minor  = port_no * SATA_MINORS_PER_DISK;
        disk->minors       = SATA_MINORS_PER_DISK;
        /* LBA48 容量截断到 unsigned long（32 位系统） */
        disk->capacity     = (unsigned long)port->sectors48;
        disk->fops         = &sata_fops;
        disk->private_data = port;

        /* disk_name: "sd0" ~ "sd5"（编号 = 端口号） */
        disk->disk_name[0] = 's';
        disk->disk_name[1] = 'd';
        disk->disk_name[2] = (char)('0' + port_no);
        disk->disk_name[3] = '\0';

        /* 注册整盘并解析分区表（MBR → sd0p1 等） */
        add_disk(disk);
        partition_scan(disk);
    }
}

/* ======================== 总线扫描 ======================== */

/*
 * sata_scan_host - 扫描 AHCI 控制器所有端口并接入块设备层
 *
 * 遍历 ports_implemented 位图，对每个已实现的端口：
 *   1. 读取 PxSSTS.DET：=3 表示设备在线且 PHY 已建立
 *   2. 读取 PxSIG：判断设备类型（0x00000101=SATA 磁盘）
 *   3. 调用 sata_identify()：发送 IDENTIFY 获取型号/序列号/容量
 * 扫描完成后调用 sata_blk_register_disks() 接入块设备层。
 *
 * PxSSTS.DET 状态含义：
 *   0 = 无设备或 PHY 未检测到
 *   1 = 设备存在但 PHY 通信未建立（需要复位）
 *   3 = 设备存在且 PHY 通信已建立（可发送命令）
 */
void sata_scan_host(struct sata_host *host)
{
    unsigned int port_no;
    unsigned int online_count = 0;

    printk("SATA: scanning %u ports (PI=0x%08x)...\n",
           host->num_ports, host->ports_implemented);

    for (port_no = 0; port_no < host->num_ports && port_no < SATA_MAX_PORTS;
         port_no++) {
        struct sata_port *port = &host->ports[port_no];
        unsigned int ssts, sig, det;

        /* 跳过未实现的端口 */
        if (!(host->ports_implemented & (1U << port_no)))
            continue;

        /* 读取 PxSSTS */
        ssts = sata_readl(port->mmio + (PORT_SSTS / 4));
        det  = ssts & 0x0F;

        if (det != SSTS_DET_PHYUP) {
            printk("SATA: port%u: no device (ssts=0x%08x det=%u)\n",
                   port_no, ssts, det);
            continue;
        }

        /* 读取 PxSIG 判断设备类型 */
        sig = sata_readl(port->mmio + (PORT_SIG / 4));

        printk("SATA: port%u: device detected sig=0x%08x\n", port_no, sig);

        if (sig == SATA_SIG_DISK) {
            port->type = 0;
            printk("SATA: port%u: SATA hard disk\n", port_no);
        } else if (sig == SATA_SIG_ATAPI) {
            port->type = 1;
            printk("SATA: port%u: SATAPI device (not supported yet)\n",
                   port_no);
            /* ATAPI 暂不处理，跳过 IDENTIFY */
            continue;
        } else {
            printk("SATA: port%u: unknown device type (sig=0x%08x), skipping\n",
                   port_no, sig);
            continue;
        }

        port->present = 1;
        online_count++;

        /* 发送 IDENTIFY DEVICE */
        if (sata_identify(port) != 0) {
            port->present = 0;
            printk("SATA: port%u: IDENTIFY failed, marking absent\n",
                   port_no);
        }
    }

    printk("SATA: scan complete. %u device(s) online\n", online_count);

    /* 接入块设备层：sd0 ~ sd5 + MBR 分区解析 */
    sata_blk_register_disks(host);
}
