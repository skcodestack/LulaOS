/*
 * LulaOS ATA 传输层(磁盘驱动)（PIO / Bus Master DMA / IDENTIFY / SET FEATURES）
 *
 * 参考：
 *   drivers/ata/ata_piix.c   — PCI probe（见 kernel/ata/ata-pci.c）
 *   drivers/ata/libata-sff.c — ata_sff_data_xfer(), PIO 传输
 *   drivers/ata/libata-core.c — ata_dev_read_id() IDENTIFY 流程
 *
 * 本文件只含参数驱动的传输函数（无全局状态），
 * 全局 ata_hosts/ata_devices、PCI probe 与块设备接入
 * 位于 kernel/ata/ata-pci.c。
 *
 * 支持的 PCI 设备（由 ata-pci.c 匹配）：
 *   8086:7010  PIIX3 IDE (Bochs/QEMU)
 *   8086:7111  PIIX4 IDE
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
#include <wait.h>           /* prepare_to_wait / finish_wait */

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
void ata_identify(struct ata_host *host, unsigned char drive,
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

/* ======================== Bus Master DMA 初始化 ======================== */

/*
 * ata_dma_init_channel - 为通道分配 PRD 表并注册到 BM 寄存器
 *
 * 每个通道独立一份 PRD 表。LulaOS 高半区内核下必须用 __pa() 将
 * kmalloc 返回的虚拟地址转为物理地址，再写入 BM PRDT 寄存器。
 *
 * 必须在 BAR4 读取和 Bus Master 使能之后调用。
 */
void ata_dma_init_channel(struct ata_host *host)
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
int ata_set_dma_mode(struct ata_device *dev, unsigned char mode)
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

    /*
     * 9. 三段式等待 DMA 完成（Linux wait_event 构建块）：
     *
     *   prepare_to_wait  —— 持 q->lock 原子完成「入队 + 设睡眠态」
     *   BM_CMD_START    —— 启动事件源（此刻起 IRQ 才可能触发）
     *   schedule        —— 若 IRQ 已触发，wake_up 已将 state 改回 RUNNING，
     *                      schedule 不会真正睡眠；否则让出 CPU 等 IRQ
     *   finish_wait     —— 醒来后自己出队 + 置回 RUNNING
     *
     * 顺序不可颠倒：必须先入队再启动 DMA，
     * 否则 IRQ 抢在入队前触发 → wake_up 遍历空队列 → 唤醒丢失 → 永久挂死。
     */
    {
        wait_queue_t wait;

        init_waitqueue_entry(&wait, current);

        /* 原子地入队 + 设 TASK_UNINTERRUPTIBLE */
        prepare_to_wait(&host->wait_queue, &wait);

        /* 10. 启动 DMA：BM_CMD = START（bit0=1），方向为读（bit8=0） */
        bm_outb(host, bm_cmd_off(host), BM_CMD_START);

        /* 11. 让出 CPU，等 IRQ 唤醒 */
        schedule();

        /* 醒来后清理：置回 RUNNING + 出队 */
        finish_wait(&host->wait_queue, &wait);
    }

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
     * 8. 三段式等待 DMA 完成，同 DMA 读路径：
     *   prepare_to_wait（原子入队+设态）→ 启动 DMA → schedule → finish_wait
     */
    {
        wait_queue_t wait;

        init_waitqueue_entry(&wait, current);

        prepare_to_wait(&host->wait_queue, &wait);

        /*
         * 9. 启动 DMA：START(bit0=1) + WRITE(bit3=1)
         *
         * BM_CMD_WRITE = 1 表示 Memory → Disk（写方向）。
         * 不设置此位则方向相反，DMA 控制器会把磁盘数据读到 buf。
         */
        bm_outb(host, bm_cmd_off(host), BM_CMD_START | BM_CMD_WRITE);

        /* 10. 让出 CPU，等 IRQ 唤醒 */
        schedule();

        /* 醒来后清理：置回 RUNNING + 出队 */
        finish_wait(&host->wait_queue, &wait);
    }

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
