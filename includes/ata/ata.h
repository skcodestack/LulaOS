/*
 * LulaOS ATA 磁盘驱动头文件
 *
 * 参考 Linux include/linux/ata.h + drivers/ata/libata.h
 *
 * 支持 PIIX3/PIIX4 IDE 控制器 (8086:7010 / 8086:7111)
 * Legacy (Compatibility) 模式下的固定 I/O 端口：
 *   Primary Channel:   cmd=0x1F0  ctrl=0x3F6   IRQ14
 *   Secondary Channel: cmd=0x170  ctrl=0x376   IRQ15
 */

#ifndef __ATA_H__
#define __ATA_H__

#include <pci/pci.h>
#include <wait.h>

/* ======================== ATA I/O 寄存器偏移 ======================== */
/* 相对于 Command Block I/O 基址 */

#define ATA_REG_DATA        0x00    /* 16-bit 数据读写 */
#define ATA_REG_ERROR       0x01    /* 读：错误码 */
#define ATA_REG_FEATURES    0x01    /* 写：特性字节 */
#define ATA_REG_NSECTORS    0x02    /* 扇区计数 */
#define ATA_REG_LBA0        0x03    /* Sector / LBA bits 0-7 */
#define ATA_REG_LBA1        0x04    /* Cylinder low / LBA bits 8-15 */
#define ATA_REG_LBA2        0x05    /* Cylinder high / LBA bits 16-23 */
#define ATA_REG_DEVICE      0x06    /* Drive/Head / LBA bits 24-27 */
#define ATA_REG_STATUS      0x07    /* 读：状态 */
#define ATA_REG_COMMAND     0x07    /* 写：命令 */

/* Control Block（相对 Control I/O 基址） */
#define ATA_REG_ALT_STATUS  0x00    /* 读：替代状态（不清除中断） */
#define ATA_REG_DEV_CTRL    0x00    /* 写：设备控制 */

/* ======================== Status 寄存器位 ======================== */

#define ATA_STATUS_BSY      0x80    /* Busy */
#define ATA_STATUS_DRDY     0x40    /* Device Ready */
#define ATA_STATUS_DF       0x20    /* Device Fault */
#define ATA_STATUS_DSC      0x10    /* Seek Complete */
#define ATA_STATUS_DRQ      0x08    /* Data Request */
#define ATA_STATUS_CORR     0x04    /* Corrected Data */
#define ATA_STATUS_IDX      0x02    /* Index */
#define ATA_STATUS_ERR      0x01    /* Error */

/* ======================== Error 寄存器位 ======================== */

#define ATA_ERR_BBK         0x80    /* Bad Block */
#define ATA_ERR_UNC         0x40    /* Uncorrectable Data */
#define ATA_ERR_MC          0x20    /* Media Changed */
#define ATA_ERR_IDNF        0x10    /* ID Not Found */
#define ATA_ERR_MCR         0x08    /* Media Change Requested */
#define ATA_ERR_ABRT        0x04    /* Aborted Command */
#define ATA_ERR_TK0NF       0x02    /* Track 0 Not Found */
#define ATA_ERR_AMNF        0x01    /* Address Mark Not Found */

/* ======================== Device 寄存器位 ======================== */

#define ATA_DEV_LBA         0x40    /* LBA 模式 */
#define ATA_DEV_SLAVE       0x10    /* 选择从盘 */

/* ======================== Device Control 位 ======================== */

#define ATA_DC_NIEN         0x02    /* 禁止中断 */
#define ATA_DC_SRST         0x04    /* 软件复位 */

/* ======================== ATA 命令 ======================== */

#define ATA_CMD_READ_SECTORS    0x20    /* READ SECTORS (PIO, LBA28) */
#define ATA_CMD_WRITE_SECTORS   0x30    /* WRITE SECTORS (PIO, LBA28) */
#define ATA_CMD_READ_DMA        0xC8    /* READ DMA (Bus Master DMA, LBA28) */
#define ATA_CMD_WRITE_DMA       0xCA    /* WRITE DMA (Bus Master DMA, LBA28) */
#define ATA_CMD_IDENTIFY        0xEC    /* IDENTIFY DEVICE */
#define ATA_CMD_IDENTIFY_PACKET 0xA1    /* IDENTIFY PACKET DEVICE (ATAPI) */
#define ATA_CMD_CACHE_FLUSH     0xE7    /* FLUSH CACHE */

/* ======================== IDENTIFY 数据字索引 ======================== */

#define ATA_ID_CAPS             49      /* 能力字 */
#define ATA_ID_MAJOR_VER        80      /* ATA 主版本号 */
#define ATA_ID_LBA_LO           60      /* LBA28 总扇区数（低 16 位） */
#define ATA_ID_LBA_HI           61      /* LBA28 总扇区数（高 16 位） */
#define ATA_ID_SERIAL           10      /* 序列号起始字 (10-19) */
#define ATA_ID_MODEL            27      /* 型号字符串起始字 (27-46) */

/* ATA_CAPS 位 */
#define ATA_CAPS_DMA            0x0100  /* bit8: 支持 DMA */
#define ATA_CAPS_LBA            0x0200  /* bit9: 支持 LBA */

/* ======================== Legacy I/O 端口 ======================== */

#define ATA_PRIMARY_IO          0x1F0
#define ATA_PRIMARY_CTRL        0x3F6
#define ATA_PRIMARY_IRQ         14

#define ATA_SECONDARY_IO        0x170
#define ATA_SECONDARY_CTRL      0x376
#define ATA_SECONDARY_IRQ       15

/* ======================== Bus Master IDE DMA ======================== */
/*
 * PIIX3/PIIX4 IDE 控制器通过 BAR4 (PCI config offset 0x20) 提供
 * Bus Master IDE I/O 基址。该基址为 I/O 端口（bit0=1）。
 *
 * 寄存器布局（相对 BM 基址）：
 *   Primary Channel:
 *     BM_PRIM_CMD   = base + 0x00  (1 byte, 写：0x01=Start, 0x00=Stop)
 *     BM_PRIM_STS   = base + 0x02  (1 byte)
 *     BM_PRIM_PRDT  = base + 0x04  (4 bytes, PRD 表物理地址，必须 4 字节对齐)
 *   Secondary Channel:
 *     BM_SEC_CMD   = base + 0x08
 *     BM_SEC_STS   = base + 0x0A
 *     BM_SEC_PRDT  = base + 0x0C
 *
 * Status 寄存器位：
 *   bit0: Active     (0=DMA 停止, 1=DMA 正在运行)
 *   bit1: Error      (1=发生错误)
 *   bit2: Interrupt  (1=传输完成)
 *   bit5: DMA Capable (Slave  设备支持 DMA)
 *   bit6: DMA Capable (Master 设备支持 DMA)
 *
 * PRD (Physical Region Descriptor) 表：
 *   每个条目 8 字节：
 *     0-3: 缓冲区物理基址（必须 2 字节对齐）
 *     4-5: 字节数（0 = 64KB）
 *     6-7: 保留（必须为 0）
 *   最后一条 EOT (End of Table) 位 = bit31 of dword[4]
 *
 * DMA 传输前必须先设置 SET FEATURES (0xEF) 子命令 0x03 启用 DMA 传输模式，
 * 否则驱动器不接受 DMA 命令。
 */

/* Bus Master IDE 寄存器偏移（相对 BM 基址） */
#define BM_PRIM_CMD         0x00    /* Primary Channel Command */
#define BM_PRIM_STS         0x02    /* Primary Channel Status */
#define BM_PRIM_PRDT        0x04    /* Primary Channel PRD Table Address */
#define BM_SEC_CMD          0x08    /* Secondary Channel Command */
#define BM_SEC_STS          0x0A    /* Secondary Channel Status */
#define BM_SEC_PRDT         0x0C    /* Secondary Channel PRD Table Address */

/* BM Command 位 */
#define BM_CMD_START        0x01    /* 启动 DMA 传输 */
#define BM_CMD_WRITE        0x08    /* 0=Read(Disk→Memory), 1=Write(Memory→Disk) */

/* BM Status 位 */
#define BM_STS_ACTIVE       0x01    /* DMA 正在运行 */
#define BM_STS_ERROR        0x02    /* 发生错误 */
#define BM_STS_INTR         0x04    /* 传输完成，中断已挂起 */
#define BM_STS_DMA0         0x20    /* Drive 0 支持 DMA */
#define BM_STS_DMA1         0x40    /* Drive 1 支持 DMA */

/* PCI BAR4：Bus Master IDE I/O 基址 */
#define ATA_PCI_BM_BAR      0x20    /* PCI config offset (BAR5/IDE Bus Master) */

/* SET FEATURES 子命令 */
#define ATA_CMD_SET_FEATURES    0xEF    /* SET FEATURES */
#define ATA_FEAT_SET_XFER       0x03    /* 子命令：设置传输模式 */
#define ATA_XFER_PIO0           0x08    /* PIO Mode 0 */
#define ATA_XFER_PIO3           0x0B    /* PIO Mode 3 */
#define ATA_XFER_PIO4           0x0C    /* PIO Mode 4 */
#define ATA_XFER_MWDMA0         0x20    /* Multi-word DMA Mode 0 */
#define ATA_XFER_MWDMA1         0x21    /* Multi-word DMA Mode 1 */
#define ATA_XFER_MWDMA2         0x22    /* Multi-word DMA Mode 2 */

/* PRD 表最大条目数 */
#define ATA_MAX_PRD         64

/*
 * ata_prd - Physical Region Descriptor 条目
 *
 * 每个条目描述一段连续的物理内存区域供 DMA 传输。
 * 必须以 8 字节对齐（4 字节边界即可，QEMU 要求 4 字节对齐）。
 */
struct ata_prd {
    unsigned int   base;    /* 物理基址（32位，bit0 保留） */
    unsigned short count;   /* 字节数（0 = 65536） */
    unsigned short flags;   /* bit15 = EOT (最后一条) */
};
#define ATA_PRD_EOT         0x8000  /* flags: End of Table */

/* ======================== ata_host 结构 ======================== */

/*
 * ata_host - ATA 通道/端口描述
 *
 * 参考 Linux ata_host 简化版，每个 PIIX3 IDE 控制器有 2 个通道。
 * Legacy 模式下使用固定 I/O 端口地址。
 *
 * bm_base：Bus Master IDE I/O 基址（从 BAR4 读取），0 = DMA 不可用。
 *           Primary 通道使用 bm_base 基址，Secondary 通道使用 bm_base+8。
 *           整个控制器共享一个 bm_base，由 probe 从 BAR4 读取一次。
 */
struct ata_host {
    struct pci_dev  *pdev;          /* 所属 PCI 设备 */
    unsigned short   io_base;       /* Command Block I/O base (0x1F0/0x170) */
    unsigned short   ctrl_base;     /* Control Block I/O base (0x3F6/0x376) */
    unsigned short   bm_base;       /* Bus Master IDE I/O base (Primary=base, Sec=base+8) */
    unsigned char    irq;           /* ISA IRQ 号 (14/15) */
    unsigned char    present;       /* 通道上有无设备 */
    unsigned char    dma_ok;        /* BM 硬件可用（BAR4 有效 + Bus Master 已使能） */
    struct ata_prd  *prd_table;     /* PRD 表虚拟地址（每个通道一份，4K 对齐） */
    unsigned int     prd_phys;      /* PRD 表物理地址 */
    wait_queue_head_t wait_queue;   /* DMA 完成等待队列 */
};

/* ======================== ata_device 结构 ======================== */

/*
 * ata_device - 单个 ATA 硬盘设备
 *
 * 每个 ata_host 通道最多 2 个设备（Master / Slave）。
 * dma_ok：由 IDENTIFY word 49 bit8 + BM 硬件状态共同决定。
 */
struct ata_device {
    struct ata_host *host;          /* 所属通道 */
    unsigned char    drive;         /* 0=Master, 1=Slave */
    unsigned char    present;       /* 0=不存在, 1=存在 */
    unsigned char    dma_ok;        /* 1=DMA 可用（驱动器+控制器均支持） */
    unsigned int     sectors;       /* LBA28 总扇区数 */
    unsigned short   identify[256]; /* IDENTIFY 原始数据（256 words = 512 bytes） */
    char             model[41];     /* 型号字符串（40 字符 + '\0'） */
    char             serial[21];    /* 序列号字符串（20 字符 + '\0'） */
};

/* ======================== 公共 API ======================== */

/*
 * ata_init - ATA 子系统初始化入口
 *
 * 注册 PCI 驱动并匹配已枚举的 PIIX3/PIIX4 IDE 控制器。
 * 必须在 pci_init() 之后调用。
 */
void ata_init(void);

/*
 * ata_pio_read_sectors - PIO 方式读取扇区
 *
 * @host:   ATA 通道
 * @drive:  0=Master, 1=Slave
 * @lba:    起始 LBA 扇区号（28 位）
 * @count:  扇区数（1~255）
 * @buf:    目标缓冲区（至少 count * 512 字节）
 * 返回：0 成功，-1 失败
 */
int ata_pio_read_sectors(struct ata_host *host, unsigned char drive,
                         unsigned int lba, unsigned char count, void *buf);

/*
 * ata_pio_write_sectors - PIO 方式写入扇区
 */
int ata_pio_write_sectors(struct ata_host *host, unsigned char drive,
                          unsigned int lba, unsigned char count, const void *buf);

/*
 * ata_dma_read_sectors - Bus Master DMA 方式读取扇区
 *
 * 前提：host->dma_ok == 1 且 dev->dma_ok == 1
 * 内部使用通道级 PRD 表（host->prd_table），不可并发。
 *
 * @host:   ATA 通道
 * @drive:  0=Master, 1=Slave
 * @lba:    起始 LBA 扇区号（28 位）
 * @count:  扇区数（1~255）
 * @buf:    目标缓冲区（至少 count * 512 字节，物理地址 = 虚拟地址）
 * 返回：0 成功，-1 失败
 */
int ata_dma_read_sectors(struct ata_host *host, unsigned char drive,
                         unsigned int lba, unsigned char count, void *buf);

/*
 * ata_dma_write_sectors - Bus Master DMA 方式写入扇区
 *
 * 与 DMA 读相比：
 *   - BM Command 设置 BM_CMD_WRITE 位（Memory → Disk）
 *   - ATA 命令使用 WRITE DMA (0xCA)
 *   - 完成后自动发送 CACHE FLUSH (0xE7)
 *
 * @host:   ATA 通道
 * @drive:  0=Master, 1=Slave
 * @lba:    起始 LBA 扇区号（28 位）
 * @count:  扇区数（1~128，超过 128 扇区需多条 PRD）
 * @buf:    源缓冲区（至少 count * 512 字节）
 * 返回：0 成功，-1 失败
 */
int ata_dma_write_sectors(struct ata_host *host, unsigned char drive,
                          unsigned int lba, unsigned char count,
                          const void *buf);

#endif /* __ATA_H__ */
