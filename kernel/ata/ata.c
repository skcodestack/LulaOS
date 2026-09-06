/*
 * LulaOS ATA PIO 磁盘驱动
 *
 * 参考：
 *   drivers/ata/ata_piix.c   — PCI ID table + probe
 *   drivers/ata/libata-sff.c — ata_sff_data_xfer(), PIO 传输
 *   drivers/ata/libata-core.c — ata_dev_read_id() IDENTIFY 流程
 *
 * 架构简化为单文件三层：
 *   1) PCI 驱动注册（ata_pci_probe）— 参考 ata_piix.c
 *   2) 设备探测（ata_identify）      — 参考 libata-core.c
 *   3) PIO 数据传输                  — 参考 libata-sff.c
 *
 * 支持的 PCI 设备：
 *   8086:7010  PIIX3 IDE (Bochs/QEMU)
 *   8086:7111  PIIX4 IDE
 *
 * Legacy 模式端口分配：
 *   Primary:   cmd=0x1F0 ctrl=0x3F6  IRQ14
 *   Secondary: cmd=0x170 ctrl=0x376  IRQ15
 */

#include <ata/ata.h>
#include <pci/pci.h>
#include <arch/x86/io.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <libs/string.h>
#include <stddef.h>
#include <mm/slab.h>

/* ======================== 全局状态 ======================== */

/* 两个通道：0=Primary, 1=Secondary */
static struct ata_host  ata_hosts[2];

/* 每个通道最多 2 个设备：[ch][drive] */
static struct ata_device ata_devices[2][2];

/* ======================== I/O 端口访问辅助 ======================== */

/*
 * ata_inb - 从 Command Block 寄存器读取 8 位
 */
static inline unsigned char ata_inb(struct ata_host *host, unsigned char reg)
{
    return inb((unsigned short)(host->io_base + reg));
}

/*
 * ata_outb - 向 Command Block 寄存器写入 8 位
 */
static inline void ata_outb(struct ata_host *host, unsigned char reg,
                            unsigned char val)
{
    outb(val, (unsigned short)(host->io_base + reg));
}

/*
 * ata_inw_data - 从 Data 寄存器读取 16 位
 */
static inline unsigned short ata_inw_data(struct ata_host *host)
{
    return inw((unsigned short)(host->io_base + ATA_REG_DATA));
}

/*
 * ata_outw_data - 向 Data 寄存器写入 16 位
 */
static inline void ata_outw_data(struct ata_host *host, unsigned short val)
{
    outw(val, (unsigned short)(host->io_base + ATA_REG_DATA));
}

/*
 * ata_ctrl_inb - 从 Control Block 读取 8 位（Alternative Status）
 */
static inline unsigned char ata_ctrl_inb(struct ata_host *host)
{
    return inb((unsigned short)(host->ctrl_base + ATA_REG_ALT_STATUS));
}

/*
 * ata_400ns_delay - 400ns 延时
 *
 * 读取 Alt Status 4 次 ≈ 400ns（每次 I/O 约 100ns）。
 * 发送命令后必须等待，让驱动器有时间开始处理。
 */
static inline void ata_400ns_delay(struct ata_host *host)
{
    ata_ctrl_inb(host);
    ata_ctrl_inb(host);
    ata_ctrl_inb(host);
    ata_ctrl_inb(host);
}

/* ======================== 轮询等待辅助 ======================== */

/*
 * ata_wait_bsy_clear - 等待 BSY 位清零
 *
 * 轮询 Alt Status 直到 BSY=0，最多等待 ~1 秒（100 万次循环）。
 * 返回最终状态字节，若超时返回 0xFF。
 */
static unsigned char ata_wait_bsy_clear(struct ata_host *host)
{
    unsigned int timeout = 1000000;
    unsigned char status;

    while (timeout--) {
        status = ata_ctrl_inb(host);
        if (!(status & ATA_STATUS_BSY))
            return status;
    }

    printk("ATA: timeout waiting for BSY clear (status=%02x)\n", status);
    return 0xFF;
}

/*
 * ata_wait_drq - 等待 DRQ 位置位
 *
 * 返回最终状态字节，超时或出错返回 0xFF。
 */
static unsigned char ata_wait_drq(struct ata_host *host)
{
    unsigned int timeout = 1000000;
    unsigned char status;

    while (timeout--) {
        status = ata_ctrl_inb(host);

        if (status & ATA_STATUS_ERR) {
            printk("ATA: device error during wait (status=%02x err=%02x)\n",
                   status, ata_inb(host, ATA_REG_ERROR));
            return 0xFF;
        }

        if (status & ATA_STATUS_DRQ)
            return status;

        if (!(status & ATA_STATUS_BSY)) {
            /* BSY=0 但 DRQ 也没置位，异常 */
            if (status == 0)
                return 0xFF;    /* 无设备 */
        }
    }

    printk("ATA: timeout waiting for DRQ (status=%02x)\n", status);
    return 0xFF;
}

/*
 * ata_wait_ready - 等待设备就绪（BSY=0 且 DRDY=1）
 */
static unsigned char ata_wait_ready(struct ata_host *host)
{
    unsigned int timeout = 1000000;
    unsigned char status;

    while (timeout--) {
        status = ata_ctrl_inb(host);
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRDY))
            return status;
    }

    return 0xFF;
}

/* ======================== IDENTIFY DEVICE ======================== */

/*
 * ata_identify - 发送 IDENTIFY DEVICE 命令
 *
 * 流程（参考 libata-core.c ata_dev_read_id()）：
 *   1. 选择驱动器（写 Device 寄存器）
 *   2. 等待 BSY 清零
 *   3. 清零 Sector Count 和 LBA 寄存器
 *   4. 发送 IDENTIFY (0xEC) 命令
 *   5. 400ns 延时
 *   6. 检查状态：
 *      - status=0 → 无设备
 *      - BSY=1   → 轮询直到 BSY=0
 *      - ERR=1   → 可能是 ATAPI（检查签名），本驱动忽略
 *      - DRQ=1   → 读取 256 words
 *   7. 解析 IDENTIFY 数据，填充 ata_device
 */
static void ata_identify(struct ata_host *host, unsigned char drive,
                         struct ata_device *dev)
{
    unsigned char status;
    unsigned int i;

    dev->host = host;
    dev->drive = drive;
    dev->present = 0;

    /* 选择驱动器：清除 DEV 位设置 master/slave */
    ata_outb(host, ATA_REG_DEVICE, drive ? ATA_DEV_SLAVE : 0);

    /* 400ns 延时让驱动器切换 */
    ata_400ns_delay(host);

    /* 等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF)
        return;

    /* 清零 Sector Count 和 LBA 寄存器（IDENTIFY 要求） */
    ata_outb(host, ATA_REG_NSECTORS, 0);
    ata_outb(host, ATA_REG_LBA0, 0);
    ata_outb(host, ATA_REG_LBA1, 0);
    ata_outb(host, ATA_REG_LBA2, 0);

    /* 发送 IDENTIFY 命令 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    /* 400ns 延时 */
    ata_400ns_delay(host);

    /* 检查状态 */
    status = ata_ctrl_inb(host);
    if (status == 0)
        return;     /* 无设备 */

    /* 轮询等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF)
        return;

    /* 检查 LBA1/LBA2 签名（ATAPI 设备标识） */
    {
        unsigned char lba1 = ata_inb(host, ATA_REG_LBA1);
        unsigned char lba2 = ata_inb(host, ATA_REG_LBA2);

        if (lba1 == 0x14 && lba2 == 0xEB) {
            /* ATAPI 设备 — 本驱动暂不处理 */
            printk("ATA: ch%d drive%d: ATAPI device detected (not supported)\n",
                   host->irq == ATA_PRIMARY_IRQ ? 0 : 1, drive);
            return;
        }
    }

    /* 等待 DRQ 或 ERR */
    status = ata_wait_drq(host);
    if (status == 0xFF) {
        /* 可能设备不支持 IDENTIFY，尝试 IDENTIFY PACKET */
        printk("ATA: ch%d drive%d: IDENTIFY failed\n",
               host->irq == ATA_PRIMARY_IRQ ? 0 : 1, drive);
        return;
    }

    /* 读取 256 words (512 bytes) IDENTIFY 数据 */
    for (i = 0; i < 256; i++)
        dev->identify[i] = ata_inw_data(host);

    /* 检查 LBA 支持 */
    if (!(dev->identify[ATA_ID_CAPS] & ATA_CAPS_LBA)) {
        printk("ATA: ch%d drive%d: LBA not supported (CHS only)\n",
               host->irq == ATA_PRIMARY_IRQ ? 0 : 1, drive);
        return;
    }

    /* 提取总扇区数（LBA28） */
    dev->sectors = ((unsigned int)dev->identify[ATA_ID_LBA_HI] << 16)
                 | (unsigned int)dev->identify[ATA_ID_LBA_LO];

    /* 提取序列号（word 10-19，20 字符） */
    {
        unsigned char *raw;
        for (i = 0; i < 10; i++) {
            raw = (unsigned char *)&dev->identify[ATA_ID_SERIAL + i];
            dev->serial[i * 2]     = raw[1];   /* 高字节在前 */
            dev->serial[i * 2 + 1] = raw[0];
        }
        dev->serial[20] = '\0';
        /* 去除尾部空格 */
        for (i = 19; i > 0 && dev->serial[i] == ' '; i--)
            dev->serial[i] = '\0';
    }

    /* 提取型号（word 27-46，40 字符） */
    {
        unsigned char *raw;
        for (i = 0; i < 20; i++) {
            raw = (unsigned char *)&dev->identify[ATA_ID_MODEL + i];
            dev->model[i * 2]     = raw[1];
            dev->model[i * 2 + 1] = raw[0];
        }
        dev->model[40] = '\0';
        /* 去除尾部空格 */
        for (i = 39; i > 0 && dev->model[i] == ' '; i--)
            dev->model[i] = '\0';
    }

    dev->present = 1;
    host->present = 1;

    printk("ATA: ch%d drive%d: model='%s' serial='%s' sectors=%u (%u MB)\n",
           host->irq == ATA_PRIMARY_IRQ ? 0 : 1,
           drive, dev->model, dev->serial,
           dev->sectors, dev->sectors / 2048);
}

/* ======================== PIO 数据传输 ======================== */

/*
 * ata_pio_read_sectors - PIO 读取扇区
 *
 * 参考 libata-sff.c ata_sff_data_xfer()
 *
 * 流程：
 *   1. 等待设备就绪（BSY=0, DRDY=1）
 *   2. 设置扇区数和 LBA 地址
 *   3. 发送 READ SECTORS (0x20) 命令
 *   4. 逐扇区等待 DRQ，读取 256 words (512 bytes)
 */
int ata_pio_read_sectors(struct ata_host *host, unsigned char drive,
                         unsigned int lba, unsigned char count, void *buf)
{
    unsigned char status;
    unsigned short *p = (unsigned short *)buf;
    unsigned int i, s;

    if (!count)
        return -1;

    /* 等待设备就绪 */
    status = ata_wait_ready(host);
    if (status == 0xFF) {
        printk("ATA: read - device not ready\n");
        return -1;
    }

    /* 设置扇区数 */
    ata_outb(host, ATA_REG_NSECTORS, count);

    /* 设置 LBA 地址 */
    ata_outb(host, ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    ata_outb(host, ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    ata_outb(host, ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    /* Device 寄存器：LBA 位 + drive 选择 + LBA bits 24-27 */
    ata_outb(host, ATA_REG_DEVICE,
             ATA_DEV_LBA | (drive ? ATA_DEV_SLAVE : 0) |
             ((lba >> 24) & 0x0F));

    /* 发送 READ SECTORS 命令 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    /* 400ns 延时 */
    ata_400ns_delay(host);

    /* 逐扇区读取 */
    for (s = 0; s < count; s++) {
        status = ata_wait_drq(host);
        if (status == 0xFF) {
            printk("ATA: read error at sector %u (status=%02x err=%02x)\n",
                   lba + s, status, ata_inb(host, ATA_REG_ERROR));
            return -1;
        }

        /* 读取 256 words = 512 bytes */
        for (i = 0; i < 256; i++)
            p[s * 256 + i] = ata_inw_data(host);
    }

    return 0;
}

/*
 * ata_pio_write_sectors - PIO 写入扇区
 *
 * 流程：
 *   1. 等待设备就绪
 *   2. 设置扇区数和 LBA 地址
 *   3. 发送 WRITE SECTORS (0x30) 命令
 *   4. 逐扇区等待 DRQ，写入 256 words
 *   5. 发送 CACHE FLUSH
 */
int ata_pio_write_sectors(struct ata_host *host, unsigned char drive,
                          unsigned int lba, unsigned char count,
                          const void *buf)
{
    unsigned char status;
    const unsigned short *p = (const unsigned short *)buf;
    unsigned int i, s;

    if (!count)
        return -1;

    /* 等待设备就绪 */
    status = ata_wait_ready(host);
    if (status == 0xFF) {
        printk("ATA: write - device not ready\n");
        return -1;
    }

    /* 设置扇区数 */
    ata_outb(host, ATA_REG_NSECTORS, count);

    /* 设置 LBA 地址 */
    ata_outb(host, ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    ata_outb(host, ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    ata_outb(host, ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    /* Device 寄存器 */
    ata_outb(host, ATA_REG_DEVICE,
             ATA_DEV_LBA | (drive ? ATA_DEV_SLAVE : 0) |
             ((lba >> 24) & 0x0F));

    /* 发送 WRITE SECTORS 命令 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    /* 400ns 延时 */
    ata_400ns_delay(host);

    /* 逐扇区写入 */
    for (s = 0; s < count; s++) {
        status = ata_wait_drq(host);
        if (status == 0xFF) {
            printk("ATA: write error at sector %u\n", lba + s);
            return -1;
        }

        /* 写入 256 words = 512 bytes */
        for (i = 0; i < 256; i++)
            ata_outw_data(host, p[s * 256 + i]);
    }

    /* 刷新写缓存 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy_clear(host);

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
 */
static int ata_pci_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    int ch, drive;

    (void)id;

    printk("ATA: found IDE controller at %02x:%02x.%x [%04x:%04x]\n",
           pdev->bus, pdev->devfn >> 3, pdev->devfn & 7,
           pdev->vendor, pdev->device);

    /* 使能 I/O 空间响应 */
    pci_enable_device(pdev);

    /* 初始化 Primary 通道 */
    ata_hosts[0].pdev     = pdev;
    ata_hosts[0].io_base  = ATA_PRIMARY_IO;
    ata_hosts[0].ctrl_base = ATA_PRIMARY_CTRL;
    ata_hosts[0].irq      = ATA_PRIMARY_IRQ;
    ata_hosts[0].present  = 0;

    /* 初始化 Secondary 通道 */
    ata_hosts[1].pdev     = pdev;
    ata_hosts[1].io_base  = ATA_SECONDARY_IO;
    ata_hosts[1].ctrl_base = ATA_SECONDARY_CTRL;
    ata_hosts[1].irq      = ATA_SECONDARY_IRQ;
    ata_hosts[1].present  = 0;

    /* 清零设备表 */
    {
        unsigned char *p = (unsigned char *)ata_devices;
        unsigned int i;
        for (i = 0; i < sizeof(ata_devices); i++)
            p[i] = 0;
    }

    /* 枚举所有通道的所有驱动器 */
    for (ch = 0; ch < 2; ch++) {
        for (drive = 0; drive < 2; drive++) {
            ata_identify(&ata_hosts[ch], (unsigned char)drive,
                         &ata_devices[ch][drive]);
        }
    }

    /* 验证测试：读取 MBR（LBA 0） */
    if (ata_devices[0][0].present) {
        unsigned char mbr[512];
        unsigned short sig;

        if (ata_pio_read_sectors(&ata_hosts[0], 0, 0, 1, mbr) == 0) {
            sig = (unsigned short)(mbr[511] << 8 | mbr[510]);
            printk("ATA: MBR read OK, signature=0x%04X\n", sig);
            printk("ATA: MBR first 16 bytes:");
            {
                int i;
                for (i = 0; i < 16; i++)
                    printk(" %02x", mbr[i]);
            }
            printk("\n");
        } else {
            printk("ATA: MBR read FAILED\n");
        }
    }

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
