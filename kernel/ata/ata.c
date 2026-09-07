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
#include <arch/x86/page.h>
#include <arch/x86/io.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <libs/string.h>
#include <stddef.h>
#include <mm/slab.h>
#include <interrupts/interrupts.h>

/* ======================== 全局状态 ======================== */

/* 两个通道：0=Primary, 1=Secondary */
static struct ata_host  ata_hosts[2];

/* 每个通道最多 2 个设备：[ch][drive] */
static struct ata_device ata_devices[2][2];

/* 防重入标志：只允许第一个 IDE 控制器绑定，忽略后续发现的控制器 */
static int ata_initialized = 0;

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

/* ======================== Bus Master DMA I/O 辅助 ======================== */

/*
 * BM 寄存器布局（相对 BM 基址）：
 *   Primary:   CMD=base+0, STS=base+2, PRDT=base+4
 *   Secondary: CMD=base+8, STS=base+10, PRDT=base+12
 * 这里 channel = (irq == 14) ? 0 : 1
 */
static inline unsigned char bm_channel(struct ata_host *host)
{
    return (host->irq == ATA_PRIMARY_IRQ) ? 0 : 1;
}

static inline void bm_outb(struct ata_host *host, unsigned char off, unsigned char val)
{
    outb(val, (unsigned short)(host->bm_base + off));
}

static inline unsigned char bm_inb(struct ata_host *host, unsigned char off)
{
    return inb((unsigned short)(host->bm_base + off));
}

static inline void bm_outl(struct ata_host *host, unsigned char off, unsigned int val)
{
    outl(val, (unsigned short)(host->bm_base + off));
}

/*
 * bm_cmd_off / bm_sts_off / bm_prdt_off - 通道级寄存器偏移
 *
 * Primary 通道偏移为 0，Secondary 通道偏移为 8。
 */
static inline unsigned char bm_cmd_off(struct ata_host *host)
{
    return bm_channel(host) ? BM_SEC_CMD : BM_PRIM_CMD;
}

static inline unsigned char bm_sts_off(struct ata_host *host)
{
    return bm_channel(host) ? BM_SEC_STS : BM_PRIM_STS;
}

static inline unsigned char bm_prdt_off(struct ata_host *host)
{
    return bm_channel(host) ? BM_SEC_PRDT : BM_PRIM_PRDT;
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
    int ch_idx = (host->irq == ATA_PRIMARY_IRQ) ? 0 : 1;

    dev->host = host;
    dev->drive = drive;
    dev->present = 0;

    printk("ATA: ch%d drive%d: probing...\n", ch_idx, drive);

    /* 选择驱动器：清除 DEV 位设置 master/slave */
    ata_outb(host, ATA_REG_DEVICE, drive ? ATA_DEV_SLAVE : 0);

    /* 400ns 延时让驱动器切换 */
    ata_400ns_delay(host);

    /* 等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: ch%d drive%d: BSY never cleared (timeout)\n", ch_idx, drive);
        return;
    }
    printk("ATA: ch%d drive%d: initial status=%02x\n", ch_idx, drive, status);

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
    printk("ATA: ch%d drive%d: post-IDENTIFY status=%02x\n", ch_idx, drive, status);

    if (status == 0)
        return;     /* 无设备 */

    /* 轮询等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: ch%d drive%d: BSY stuck after IDENTIFY\n", ch_idx, drive);
        return;
    }
    printk("ATA: ch%d drive%d: post-BSY status=%02x err=%02x\n",
           ch_idx, drive, status, ata_inb(host, ATA_REG_ERROR));

    /* 检查 ERR 位：ABRT (0x04) = 设备不支持 IDENTIFY 或无设备 */
    if (status & ATA_STATUS_ERR) {
        unsigned char err = ata_inb(host, ATA_REG_ERROR);
        printk("ATA: ch%d drive%d: IDENTIFY error (status=%02x err=%02x)\n",
               ch_idx, drive, status, err);
        return;
    }

    /* 检查 LBA1/LBA2 签名（ATAPI 设备标识） */
    {
        unsigned char lba1 = ata_inb(host, ATA_REG_LBA1);
        unsigned char lba2 = ata_inb(host, ATA_REG_LBA2);

        if (lba1 == 0x14 && lba2 == 0xEB) {
            /* ATAPI 设备 — 本驱动暂不处理 */
            printk("ATA: ch%d drive%d: ATAPI device detected (not supported)\n",
                   ch_idx, drive);
            return;
        }
    }

    /* 等待 DRQ 或 ERR */
    status = ata_wait_drq(host);
    if (status == 0xFF) {
        printk("ATA: ch%d drive%d: DRQ never asserted\n", ch_idx, drive);
        return;
    }

    /* 读取 256 words (512 bytes) IDENTIFY 数据 */
    for (i = 0; i < 256; i++)
        dev->identify[i] = ata_inw_data(host);

    /*
     * 清除中断：ATA PIO 协议要求每次数据传输完成后，
     * 主机必须读一次主 Status 寄存器（0x1F7）来 acknowledge 中断。
     * 不读的话驱动器停留在 INTRQ 挂起状态，下一条命令会被拒绝。
     */
    status = ata_inb(host, ATA_REG_STATUS);
    printk("ATA: ch%d drive%d: post-read status=%02x\n", ch_idx, drive, status);

    /* 再次检查 ERR：某些设备在数据传输后才报错 */
    if (status & ATA_STATUS_ERR) {
        printk("ATA: ch%d drive%d: error after IDENTIFY data transfer\n", ch_idx, drive);
        return;
    }

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

    /*
     * 检测 DMA 能力：
     *   IDENTIFY word 49 bit8 = 1 表示驱动器支持 DMA。
     *   还需 host->dma_ok（BAR4 有效 + Bus Master 已使能）才能实际使用。
     */
    if ((dev->identify[ATA_ID_CAPS] & ATA_CAPS_DMA) && host->dma_ok)
        dev->dma_ok = 1;

    printk("ATA: ch%d drive%d: model='%s' serial='%s' sectors=%u (%u MB) dma=%d\n",
           host->irq == ATA_PRIMARY_IRQ ? 0 : 1,
           drive, dev->model, dev->serial,
           dev->sectors, dev->sectors / 2048, dev->dma_ok);
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

    /* 等待 BSY 清零（不强制要求 DRDY=1，部分模拟器 IDENTIFY 后不立即置 DRDY） */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        unsigned char alt = ata_ctrl_inb(host);
        printk("ATA: read - BSY stuck (status=%02x alt=%02x)\n", status, alt);
        return -1;
    }
    if (status & ATA_STATUS_ERR) {
        printk("ATA: read - pre-command error (status=%02x err=%02x)\n",
               status, ata_inb(host, ATA_REG_ERROR));
        return -1;
    }
    printk("ATA: read - ready (status=%02x)\n", status);

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

        /* 清除本扇区传输完成中断，让驱动器可以继续下一扇区 */
        ata_inb(host, ATA_REG_STATUS);
    }

    /* 最终状态检查 */
    status = ata_wait_bsy_clear(host);
    if (status & ATA_STATUS_ERR) {
        printk("ATA: read completed with error (status=%02x err=%02x)\n",
               status, ata_inb(host, ATA_REG_ERROR));
        return -1;
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

    /* 等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: write - BSY stuck\n");
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

        /* 清除本扇区写入完成中断 */
        ata_inb(host, ATA_REG_STATUS);
    }

    /* 刷新写缓存 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    status = ata_wait_bsy_clear(host);
    if (status & ATA_STATUS_ERR) {
        printk("ATA: cache flush failed (status=%02x)\n", status);
        return -1;
    }

    return 0;
}

/* ======================== Bus Master DMA 初始化 ======================== */

/*
 * ata_dma_init_channel - 为通道分配 PRD 表并注册到 BM 寄存器
 *
 * 每个通道独立一份 PRD 表。LulaOS 高半区内核下必须用 __pa() 将
 * kmalloc 返回的虚拟地址转为物理地址，再写入 BM PRDT 寄存器。
 *
 * 必须在 BAR4 读取和 Bus Master 使能之后调用。
 */
static void ata_dma_init_channel(struct ata_host *host)
{
    if (!host->dma_ok)
        return;

    /* 分配 PRD 表（单条目，8 字节，最多 64KB 传输） */
    host->prd_table = (struct ata_prd *)kmalloc(sizeof(struct ata_prd), GFP_KERNEL);
    if (!host->prd_table) {
        printk("ATA: DMA: PRD table allocation failed for ch%d\n",
               bm_channel(host));
        host->dma_ok = 0;
        return;
    }

    /* 高半区内核：虚拟地址需减去 PAGE_OFFSET 得到物理地址 */
    host->prd_phys = (unsigned int)__pa(host->prd_table);

    /* 写入该通道的 PRDT 基址寄存器 */
    bm_outl(host, bm_prdt_off(host), host->prd_phys);

    printk("ATA: DMA: ch%d PRD table at phys=0x%08x (reg off=%d)\n",
           bm_channel(host), host->prd_phys, bm_prdt_off(host));

    /* 初始化 DMA 完成等待队列 */
    init_waitqueue_head(&host->wait_queue);

    /* 注册 IRQ 处理函数（IRQ 转中断向量：vector = FIRST_DEVICE_VECTOR + irq） */
    {
        unsigned int vector = FIRST_DEVICE_VECTOR + host->irq;
        int ret = request_irq(vector, ata_irq_handler, "ata_dma", host);
        if (ret == 0)
            printk("ATA: DMA: IRQ%d registered (vector=%#x)\n", host->irq, vector);
        else {
            printk("ATA: DMA: IRQ%d registration FAILED (ret=%d)\n", host->irq, ret);
            host->dma_ok = 0;  /* 中断注册失败，回退为 PIO */
        }
    }
}

/* ======================== ATA 中断处理函数 ======================== */

/*
 * ata_irq_handler - ATA DMA 完成中断处理
 *
 * 当 DMA 传输完成时，IDE 控制器触发 IRQ14（主通道）或 IRQ15（次通道）。
 * 本函数：
 *   1. 检查 BM Status 的 INTR 位，确认是本通道的 DMA 完成中断
 *   2. 唤醒等待在该通道 wait_queue 上的进程
 *   3. 中断处理函数不直接清理 BM 状态，由被唤醒的进程完成
 *
 * 参数：
 *   irq: ISA IRQ 号（14 或 15）
 *   dev_id: 指向 ata_host 结构（注册时传入）
 *   regs: 中断时的寄存器快照（本函数不使用）
 */
static void ata_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    struct ata_host *host = (struct ata_host *)dev_id;
    unsigned char bm_sts;

    /* 读取 BM Status，检查 INTR 位 */
    bm_sts = bm_inb(host, bm_sts_off(host));

    /*
     * BM_STS_INTR (bit 2) = 1 表示 DMA 传输完成并产生了中断。
     * 如果不是我们的中断（可能是共享 IRQ 或其他设备），直接返回。
     */
    if (!(bm_sts & BM_STS_INTR))
        return;

    /* 唤醒等待 DMA 完成的进程 */
    wake_up(&host->wait_queue);
}

/* ======================== SET FEATURES 传输模式 ======================== */

/*
 * ata_set_dma_mode - 向驱动器发送 SET FEATURES 启用 MWDMA2 传输模式
 *
 * 流程（ATA/ATAPI 标准）：
 *   1. 选择驱动器（Device 寄存器）
 *   2. 等待 BSY 清零
 *   3. Features = 0x03 (Set Transfer Mode)
 *   4. Sector Count = transfer mode value (MWDMA2 = 0x22)
 *   5. 发送 SET FEATURES (0xEF) 命令
 *   6. 等待完成，检查 ERR 位
 *
 * QEMU 的 PIIX3 接受任意 DMA 模式设置；部分真实硬件会拒绝不支持的模式。
 * 失败时 dev->dma_ok 保持 0，驱动自动回退到 PIO。
 */
static int ata_set_dma_mode(struct ata_device *dev, unsigned char mode)
{
    struct ata_host *host = dev->host;
    unsigned char status;
    int ch_idx = bm_channel(host);

    /* 选择驱动器 */
    ata_outb(host, ATA_REG_DEVICE, dev->drive ? ATA_DEV_SLAVE : 0);
    ata_400ns_delay(host);

    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: DMA mode: BSY stuck on ch%d drive%d\n",
               ch_idx, dev->drive);
        return -1;
    }

    /* Features = SET_TRANSFER_MODE */
    ata_outb(host, ATA_REG_FEATURES, ATA_FEAT_SET_XFER);

    /* Sector Count = 目标传输模式 */
    ata_outb(host, ATA_REG_NSECTORS, mode);

    /* LBA 寄存器清零 */
    ata_outb(host, ATA_REG_LBA0, 0);
    ata_outb(host, ATA_REG_LBA1, 0);
    ata_outb(host, ATA_REG_LBA2, 0);

    /* 发送 SET FEATURES */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_SET_FEATURES);
    ata_400ns_delay(host);

    /* 等待命令完成（BSY 清零） */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: DMA mode: timeout on ch%d drive%d\n",
               ch_idx, dev->drive);
        return -1;
    }

    if (status & ATA_STATUS_ERR) {
        unsigned char err = ata_inb(host, ATA_REG_ERROR);
        printk("ATA: DMA mode: rejected by drive ch%d drive%d "
               "(mode=0x%02x status=%02x err=%02x)\n",
               ch_idx, dev->drive, mode, status, err);
        return -1;
    }

    printk("ATA: DMA mode: ch%d drive%d set to MWDMA2 (0x%02x) OK\n",
           ch_idx, dev->drive, mode);
    return 0;
}

/* ======================== Bus Master DMA 数据传输 ======================== */

/*
 * ata_dma_read_sectors - Bus Master DMA 方式读取扇区
 *
 * 与 PIO 相比，CPU 不参与数据搬运，DMA 控制器直接将磁盘数据写入内存。
 *
 * 流程：
 *   1. 停止 DMA（CMD=0，清除 Status）
 *   2. 构建 PRD 表：单条目覆盖整个传输
 *   3. 将 PRD 表物理地址写入 BM_PRDT 寄存器
 *   4. 设置 LBA 地址 + 扇区数
 *   5. 发送 READ DMA (0xC8) 命令
 *   6. 设置 BM Command = START
 *   7. 轮询 BM Status，等待传输完成
 *   8. 读主 Status 寄存器清除 INTRQ
 *
 * 缓冲区 buf 在 LulaOS 平坦映射下虚拟地址 = 物理地址，可直接写入 PRD。
 */
int ata_dma_read_sectors(struct ata_host *host, unsigned char drive,
                         unsigned int lba, unsigned char count, void *buf)
{
    unsigned char status;
    unsigned int total_bytes;
    unsigned char sts;

    if (!host->dma_ok || !count)
        return -1;

    total_bytes = (unsigned int)count * 512;

    /*
     * 单条目 PRD 最大 65536 字节（count=0 表示 64K）。
     * 超过 128 个扇区（64KB）需要多条 PRD，这里只支持 ≤128 扇区。
     */
    if (total_bytes > 65536) {
        printk("ATA: DMA: transfer too large (%u bytes, max 65536)\n",
               total_bytes);
        return -1;
    }

    /* 1. 停止任何残留 DMA 传输 */
    bm_outb(host, bm_cmd_off(host), 0);

    /* 2. 清除 Status 寄存器（写 1 清零对应位） */
    bm_outb(host, bm_sts_off(host),
            BM_STS_ERROR | BM_STS_INTR | BM_STS_ACTIVE);

    /* 3. 构建 PRD 表：单条目，DMA 控制器需要物理地址 */
    host->prd_table[0].base  = (unsigned int)__pa(buf);
    host->prd_table[0].count = (total_bytes == 65536)
                               ? 0 : (unsigned short)total_bytes;
    host->prd_table[0].flags = ATA_PRD_EOT;

    /* 4. 写入 PRD 表物理地址 */
    bm_outl(host, bm_prdt_off(host), host->prd_phys);

    printk("ATA: DMA read: lba=%u count=%u buf=0x%08x bytes=%u prd=0x%08x\n",
           lba, count, (unsigned int)(unsigned long)buf,
           total_bytes, host->prd_phys);

    /* 5. 选择驱动器 */
    ata_outb(host, ATA_REG_DEVICE, drive ? ATA_DEV_SLAVE : 0);
    ata_400ns_delay(host);

    /* 6. 等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: DMA read: BSY stuck\n");
        return -1;
    }

    /* 7. 设置扇区数和 LBA 地址 */
    ata_outb(host, ATA_REG_NSECTORS, count);
    ata_outb(host, ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    ata_outb(host, ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    ata_outb(host, ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    /* Device 寄存器：LBA 位 + drive 选择 + LBA bits 24-27 */
    ata_outb(host, ATA_REG_DEVICE,
             ATA_DEV_LBA | (drive ? ATA_DEV_SLAVE : 0) |
             ((lba >> 24) & 0x0F));

    /* 8. 发送 READ DMA 命令（不中断模式由 BM 控制，无需 nIEN） */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_READ_DMA);
    ata_400ns_delay(host);

    /* 9. 启动 DMA：BM_CMD = START（bit0=1），方向为读（bit8=0） */
    bm_outb(host, bm_cmd_off(host), BM_CMD_START);

    /* 10. 睡眠等待 DMA 完成（IRQ 处理函数会唤醒我们） */
    sleep_on(&host->wait_queue);

    /* 被唤醒后检查状态 */
    sts = bm_inb(host, bm_sts_off(host));

    if (sts & BM_STS_ERROR) {
        printk("ATA: DMA read: error (bm_sts=%02x ata_sts=%02x err=%02x)\n",
               sts,
               ata_inb(host, ATA_REG_STATUS),
               ata_inb(host, ATA_REG_ERROR));
        bm_outb(host, bm_cmd_off(host), 0);
        return -1;
    }

    /* 11. 读主 Status 寄存器清除 INTRQ（ATA 协议要求） */
    status = ata_inb(host, ATA_REG_STATUS);
    if (status & ATA_STATUS_ERR) {
        printk("ATA: DMA read: ATA error (status=%02x err=%02x)\n",
               status, ata_inb(host, ATA_REG_ERROR));
        bm_outb(host, bm_cmd_off(host), 0);
        return -1;
    }

    /* 12. 清除 BM Status INTR 位（写 1 清零） */
    bm_outb(host, bm_sts_off(host), BM_STS_INTR);

    /* 13. 停止 DMA */
    bm_outb(host, bm_cmd_off(host), 0);

    printk("ATA: DMA read: OK (final ata_sts=%02x bm_sts=%02x)\n",
           status, bm_inb(host, bm_sts_off(host)));
    return 0;
}

/* ======================== Bus Master DMA 写 ======================== */

/*
 * ata_dma_write_sectors - Bus Master DMA 方式写入扇区
 *
 * 与 DMA 读相比，写方向的差异：
 *   - BM Command bit3（BM_CMD_WRITE）= 1：方向 Memory → Disk
 *   - ATA 命令使用 WRITE DMA（0xCA）
 *   - DMA 传输完成后必须追加 CACHE FLUSH（0xE7），确保数据写入盘片
 *
 * 流程：
 *   1. 停止 DMA（CMD=0，清除 Status）
 *   2. 构建 PRD 表：指向 buf（数据来源）
 *   3. 写 PRDT 基址
 *   4. 设置 LBA 地址 + 扇区数
 *   5. 发送 WRITE DMA (0xCA) 命令
 *   6. 启动 DMA：CMD = START | WRITE
 *   7. 轮询 BM Status 等待完成
 *   8. CACHE FLUSH
 */
int ata_dma_write_sectors(struct ata_host *host, unsigned char drive,
                          unsigned int lba, unsigned char count,
                          const void *buf)
{
    unsigned char status;
    unsigned int total_bytes;
    unsigned char sts;

    if (!host->dma_ok || !count)
        return -1;

    total_bytes = (unsigned int)count * 512;

    if (total_bytes > 65536) {
        printk("ATA: DMA write: transfer too large (%u bytes, max 65536)\n",
               total_bytes);
        return -1;
    }

    /* 1. 停止任何残留 DMA */
    bm_outb(host, bm_cmd_off(host), 0);

    /* 2. 清除 Status 寄存器 */
    bm_outb(host, bm_sts_off(host),
            BM_STS_ERROR | BM_STS_INTR | BM_STS_ACTIVE);

    /*
     * 3. 构建 PRD 表：buf 是数据来源，DMA 控制器从内存读 buf 写入磁盘。
     *    必须转为物理地址，高半区内核下虚拟 != 物理。
     */
    host->prd_table[0].base  = (unsigned int)__pa(buf);
    host->prd_table[0].count = (total_bytes == 65536)
                               ? 0 : (unsigned short)total_bytes;
    host->prd_table[0].flags = ATA_PRD_EOT;

    bm_outl(host, bm_prdt_off(host), host->prd_phys);

    printk("ATA: DMA write: lba=%u count=%u buf=0x%08x bytes=%u\n",
           lba, count, (unsigned int)(unsigned long)buf, total_bytes);

    /* 4. 选择驱动器 */
    ata_outb(host, ATA_REG_DEVICE, drive ? ATA_DEV_SLAVE : 0);
    ata_400ns_delay(host);

    /* 5. 等待 BSY 清零 */
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF) {
        printk("ATA: DMA write: BSY stuck\n");
        return -1;
    }

    /* 6. 设置扇区数和 LBA 地址 */
    ata_outb(host, ATA_REG_NSECTORS, count);
    ata_outb(host, ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    ata_outb(host, ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    ata_outb(host, ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    ata_outb(host, ATA_REG_DEVICE,
             ATA_DEV_LBA | (drive ? ATA_DEV_SLAVE : 0) |
             ((lba >> 24) & 0x0F));

    /* 7. 发送 WRITE DMA 命令 */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_WRITE_DMA);
    ata_400ns_delay(host);

    /*
     * 8. 启动 DMA：START(bit0=1) + WRITE(bit3=1)
     *
     * BM_CMD_WRITE = 1 表示 Memory → Disk（写方向）。
     * 不设置此位则方向相反，DMA 控制器会把磁盘数据读到 buf。
     */
    bm_outb(host, bm_cmd_off(host), BM_CMD_START | BM_CMD_WRITE);

    /* 9. 睡眠等待 DMA 完成（IRQ 处理函数会唤醒我们） */
    sleep_on(&host->wait_queue);

    /* 被唤醒后检查状态 */
    sts = bm_inb(host, bm_sts_off(host));

    if (sts & BM_STS_ERROR) {
        printk("ATA: DMA write: error (bm_sts=%02x ata_sts=%02x err=%02x)\n",
               sts,
               ata_inb(host, ATA_REG_STATUS),
               ata_inb(host, ATA_REG_ERROR));
        bm_outb(host, bm_cmd_off(host), 0);
        return -1;
    }

    /* 10. 读主 Status 寄存器清除 INTRQ */
    status = ata_inb(host, ATA_REG_STATUS);
    if (status & ATA_STATUS_ERR) {
        printk("ATA: DMA write: ata error (status=%02x err=%02x)\n",
               status, ata_inb(host, ATA_REG_ERROR));
        bm_outb(host, bm_cmd_off(host), 0);
        return -1;
    }

    /* 11. 清除 BM Status INTR 位，停止 DMA */
    bm_outb(host, bm_sts_off(host), BM_STS_INTR);
    bm_outb(host, bm_cmd_off(host), 0);

    /*
     * 12. CACHE FLUSH (0xE7)
     *
     * DMA 写完成后，ATA 标准要求主机发送 CACHE FLUSH，
     * 等待驱动器将写缓存中的数据实际写入介质。
     * 不刷新则断电/复位可能丢失数据。
     */
    ata_outb(host, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    status = ata_wait_bsy_clear(host);
    if (status == 0xFF || (status & ATA_STATUS_ERR)) {
        printk("ATA: DMA write: cache flush failed (status=%02x)\n", status);
        return -1;
    }

    printk("ATA: DMA write: OK (final ata_sts=%02x bm_sts=%02x)\n",
           status, bm_inb(host, bm_sts_off(host)));
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
    if (ata_devices[0][0].present) {
        unsigned char mbr[512];
        unsigned short sig;
        int ret;

        printk("ATA: attempting MBR read (LBA 0, mode=%s)...\n",
               ata_devices[0][0].dma_ok ? "DMA" : "PIO");

        if (ata_devices[0][0].dma_ok) {
            ret = ata_dma_read_sectors(&ata_hosts[0], 0, 0, 1, mbr);
        } else {
            ret = ata_pio_read_sectors(&ata_hosts[0], 0, 0, 1, mbr);
        }

        if (ret == 0) {
            sig = (unsigned short)(mbr[511] << 8 | mbr[510]);
            printk("ATA: MBR read OK [%s], signature=0x%04X\n",
                   ata_devices[0][0].dma_ok ? "DMA" : "PIO", sig);
            printk("ATA: MBR first 16 bytes:");
            {
                int i;
                for (i = 0; i < 16; i++)
                    printk(" %02x", mbr[i]);
            }
            printk("\n");

            /* DMA 模式下追加一次 PIO 对比，验证两者一致性 */
            if (ata_devices[0][0].dma_ok) {
                unsigned char mbr_pio[512];
                if (ata_pio_read_sectors(&ata_hosts[0], 0, 0, 1, mbr_pio) == 0) {
                    unsigned int diff = 0, i;
                    for (i = 0; i < 512; i++)
                        if (mbr[i] != mbr_pio[i]) diff++;
                    printk("ATA: DMA vs PIO consistency check: %s (%u bytes differ)\n",
                           diff == 0 ? "MATCH" : "MISMATCH", diff);
                }
            }
        } else {
            printk("ATA: MBR read FAILED [%s]\n",
                   ata_devices[0][0].dma_ok ? "DMA" : "PIO");
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
