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

/* ======================== ata_host 结构 ======================== */

/*
 * ata_host - ATA 通道/端口描述
 *
 * 参考 Linux ata_host 简化版，每个 PIIX3 IDE 控制器有 2 个通道。
 * Legacy 模式下使用固定 I/O 端口地址。
 */
struct ata_host {
    struct pci_dev  *pdev;          /* 所属 PCI 设备 */
    unsigned short   io_base;       /* Command Block I/O base (0x1F0/0x170) */
    unsigned short   ctrl_base;     /* Control Block I/O base (0x3F6/0x376) */
    unsigned char    irq;           /* ISA IRQ 号 (14/15) */
    unsigned char    present;       /* 通道上有无设备 */
};

/* ======================== ata_device 结构 ======================== */

/*
 * ata_device - 单个 ATA 硬盘设备
 *
 * 每个 ata_host 通道最多 2 个设备（Master / Slave）。
 */
struct ata_device {
    struct ata_host *host;          /* 所属通道 */
    unsigned char    drive;         /* 0=Master, 1=Slave */
    unsigned char    present;       /* 0=不存在, 1=存在 */
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

#endif /* __ATA_H__ */
