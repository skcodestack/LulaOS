/*
 * LulaOS SATA/AHCI 子系统
 *
 * 参考：
 *   Intel AHCI 1.3 Specification
 *   Linux drivers/ata/ahci.c       — AHCI 控制器初始化
 *   Linux drivers/ata/libahci.c    — ahci_init_one(), ahci_port_start()
 *   Linux drivers/ata/libata-core.c — ata_dev_read_id() IDENTIFY 流程
 *
 * 架构（单文件实现，PCI 驱动注册 + AHCI 端口管理 + 总线扫描）：
 *   sata_init()          → pci_register_driver()
 *   sata_pci_probe()     → ioremap BAR5, 全局使能, 端口初始化
 *   sata_port_init()     → 分配 Command List / FIS Receive, 启动端口
 *   sata_scan_host()     → 遍历 PxSSTS，检测在线设备
 *   sata_identify()      → H2D FIS (0xEC) 获取设备信息
 *   sata_read_sectors()  → READ DMA EXT (0x25) 通过 PRDT
 *   sata_write_sectors() → WRITE DMA EXT (0x35) 通过 PRDT
 *
 * 支持的 PCI 设备：
 *   8086:2922  ICH9 AHCI (QEMU 默认)
 *   class 01h/06h/01h  任意 AHCI 1.x 控制器
 */

#ifndef __SATA_H__
#define __SATA_H__

#include <pci/pci.h>
#include <wait.h>
#include <arch/x86/page.h>    /* __pa() */
#include <arch/x86/highmem.h> /* ioremap() */

/* ======================== AHCI 全局寄存器偏移（相对 BAR5） ======================== */

#define AHCI_CAP        0x00   /* Host Capabilities：NP(bit4:0)=端口数-1 */
#define AHCI_GHC        0x04   /* Global Host Control：AE(bit31)=AHCI使能 */
#define AHCI_IS         0x08   /* Interrupt Status：bit N = port N 有中断 */
#define AHCI_PI         0x0C   /* Ports Implemented：bit N = port N 存在 */
#define AHCI_VS         0x10   /* AHCI Version：高16位=主版本，低16位=次版本 */

/* GHC 位 */
#define AHCI_GHC_AE     0x80000000   /* AHCI Enable：0=Legacy, 1=AHCI 模式 */
#define AHCI_GHC_HR     0x00000001   /* HBA Reset：写1触发，自清零 */
#define AHCI_GHC_IE     0x00000002   /* Interrupt Enable（全局中断使能） */

/* CAP 位掩码 */
#define AHCI_CAP_NP     0x0000001F   /* bit4:0 Number of Ports - 1 */
#define AHCI_CAP_S64A   0x80000000   /* bit31 Supports 64-bit Addressing */
#define AHCI_CAP_SNCQ   0x00010000   /* bit16 Supports NCQ */

/* ======================== AHCI 端口寄存器偏移 ======================== */
/* 相对地址 = BAR5 + 0x100 + port × 0x80 */

#define AHCI_PORT_BASE  0x100  /* 第一个端口寄存器起始偏移 */
#define AHCI_PORT_SIZE  0x80   /* 每个端口寄存器块大小 */

/* 端口内寄存器偏移（相对端口基址） */
#define PORT_CLB        0x00   /* Command List Base Address 低32位（1KB对齐） */
#define PORT_CLBU       0x04   /* Command List Base Address 高32位 */
#define PORT_FB         0x08   /* FIS Base Address 低32位（256B对齐） */
#define PORT_FBU        0x0C   /* FIS Base Address 高32位 */
#define PORT_IS         0x10   /* Interrupt Status（写1清零） */
#define PORT_IE         0x14   /* Interrupt Enable */
#define PORT_CMD        0x18   /* Command and Status */
#define PORT_TFD        0x20   /* Task File Data：bit7=BSY bit3=DRQ bit0=ERR */
#define PORT_SIG        0x24   /* Signature（设备类型识别） */
#define PORT_SSTS       0x28   /* SATA Status：DET(bit3:0) */
#define PORT_SCTL       0x2C   /* SATA Control */
#define PORT_SERR       0x30   /* SATA Error（写1清零） */
#define PORT_SACT       0x34   /* SATA Active（NCQ 用） */
#define PORT_CI         0x38   /* Command Issue：写1触发该 slot */

/* PORT_CMD 位定义 */
#define PORT_CMD_ST     0x00000001   /* bit0  Start：启动端口命令处理 */
#define PORT_CMD_SUD    0x00000002   /* bit1  Spin-Up Device */
#define PORT_CMD_POD    0x00000004   /* bit2  Power On Device */
#define PORT_CMD_FRE    0x00000010   /* bit4  FIS Receive Enable */
#define PORT_CMD_CLO    0x00000008   /* bit3  Command List Override */
#define PORT_CMD_FR     0x00004000   /* bit14 FIS Receive Running（只读） */
#define PORT_CMD_CR     0x00008000   /* bit15 Command List Running（只读） */

/* PORT_IS 位定义 */
#define PORT_IS_DHR     0x00000001   /* Device to Host Register FIS */
#define PORT_IS_PS      0x00000002   /* PIO Setup FIS */
#define PORT_IS_DS      0x00000004   /* DMA Setup FIS */
#define PORT_IS_SDB     0x00000008   /* Set Device Bits FIS */
#define PORT_IS_UF      0x00000010   /* Unknown FIS */
#define PORT_IS_DPE     0x00000020   /* Descriptor Processed Error */
#define PORT_IS_PCE     0x00000040   /* Port Change Error */
#define PORT_IS_TFE     0x00000080   /* Task File Error */
#define PORT_IS_HBF     0x00000100   /* Host Bus Fatal Error */
#define PORT_IS_HBD     0x00000200   /* Host Bus Data Error */
#define PORT_IS_IF      0x00000400   /* Interface Fatal Error */
#define PORT_IS_DPS     0x00008000   /* Device to Host FIS (成功完成) */
/* 错误位集合 */
#define PORT_IS_ERR     (PORT_IS_TFE | PORT_IS_HBF | PORT_IS_HBD | \
                         PORT_IS_IF  | PORT_IS_PCE)

/* PORT_TFD 位 */
#define PORT_TFD_BSY    0x80   /* bit7 BSY */
#define PORT_TFD_DRQ    0x08   /* bit3 DRQ */
#define PORT_TFD_ERR    0x01   /* bit0 ERR */

/* PORT_SSTS.DET 字段（bit3:0） */
#define SSTS_DET_NODEV      0x0   /* 无设备 / PHY 未建立 */
#define SSTS_DET_PRESENT    0x1   /* 设备存在，PHY 通信未建立 */
#define SSTS_DET_PHYUP      0x3   /* 设备存在，PHY 通信已建立 */

/* PORT_SIG 设备类型签名 */
#define SATA_SIG_DISK   0x00000101   /* SATA 硬盘 */
#define SATA_SIG_ATAPI  0xEB140101   /* SATAPI 光驱 */
#define SATA_SIG_SEMB   0xC33C0101   /* 机箱管理（Enclosure Management） */
#define SATA_SIG_PM     0x96690101   /* Port Multiplier */

/* ======================== AHCI 命令常量 ======================== */

#define AHCI_CMD_IDENTIFY       0xEC   /* IDENTIFY DEVICE */
#define AHCI_CMD_READ_DMA_EXT   0x25   /* READ DMA EXT (LBA48) */
#define AHCI_CMD_WRITE_DMA_EXT  0x35   /* WRITE DMA EXT (LBA48) */
#define AHCI_CMD_FLUSH_CACHE    0xE7   /* FLUSH CACHE */

/* H2D Register FIS 类型 */
#define FIS_TYPE_REG_H2D    0x27   /* Register FIS Host to Device */

/* ======================== Command List 标志位（byte 1 of DWORD 0） ======================== */
/*
 * AHCI 1.3 spec 4.2.2 Command List Structure:
 *   Byte 0: CFL(bit4:0) | A(bit5) | W(bit6) | P(bit7)
 *   Byte 1: R(bit0) | B(bit1) | C(bit2) | RSV(bit3) | RSV4(bit4) | PMP(bit7:5)
 */
#define AHCI_CMD_FLAG_CFL_MASK  0x1F   /* CFL: Command FIS Length in DWORDs */
#define AHCI_CMD_FLAG_A         0x20   /* ATAPI command */
#define AHCI_CMD_FLAG_W         0x40   /* Write direction (1=H2D, 0=D2H) */
#define AHCI_CMD_FLAG_P         0x80   /* Prefetchable */

/* PRDT DBC 字段：bit31 = IOC (Interrupt On Completion) */
#define AHCI_PRDT_IOC           0x80000000

/* ======================== IDENTIFY 数据字索引 ======================== */

#define SATA_ID_CAPS        49      /* Capabilities */
#define SATA_ID_MAJOR_VER   80      /* ATA major version */
#define SATA_ID_CMD_SET2    83      /* Command set/feature supported ext (LBA48 = bit10) */
#define SATA_ID_LBA_LO      60      /* LBA28 总扇区数（低 16 位） */
#define SATA_ID_LBA_HI      61      /* LBA28 总扇区数（高 16 位） */
#define SATA_ID_LBA48_LO    100     /* LBA48 总扇区数 bits 0-15 */
#define SATA_ID_LBA48_MID   101     /* LBA48 总扇区数 bits 16-31 */
#define SATA_ID_SERIAL      10      /* 序列号起始字 (word 10-19) */
#define SATA_ID_MODEL       27      /* 型号字符串起始字 (word 27-46) */

/* ======================== 数据结构 ======================== */

#define SATA_MAX_PORTS  6    /* 限制为 6 个端口（QEMU ICH9 最多 6 口） */
#define SATA_MAX_PRDT   8    /* 每次传输最多 8 个 PRDT 条目 */

/*
 * ahci_cmd_header - AHCI 命令头（32 字节，每端口 32 个 slot）
 *
 * 端口 Command List 由 32 个此结构组成，共 1024 字节，必须 1024 字节对齐。
 * 所有字段均为小端（x86 LE，直接赋值即可）。
 */
struct ahci_cmd_header {
    unsigned char   dw0_cfl_flags;  /* CFL(bit4:0) + A/W/P flags */
    unsigned char   dw0_flags_hi;   /* R/B/C/PMP 等 */
    unsigned short  dw0_prdtl;      /* PRDT Length（PRD 条目数） */
    unsigned int    dw1_prdbc;      /* PRD Byte Count（HBA 写回，传输前清零） */
    unsigned int    dw2_ctba;       /* Command Table 物理地址（128 字节对齐） */
    unsigned int    dw3_ctbau;      /* 高 32 位（32 位系统填 0） */
    unsigned int    dw4_reserved[4]; /* 保留（DW4-DW7） */
};

/*
 * ahci_prdt_entry - PRD Table 条目（16 字节）
 *
 * 描述一段连续物理内存区域，供 HBA DMA 传输。
 * DBC 字段为字节数减 1（0 = 1 字节），bit31 = IOC（完成时触发中断）。
 */
struct ahci_prdt_entry {
    unsigned int    dba;        /* Data Base Address（物理地址） */
    unsigned int    dbau;       /* Data Base Address Upper（=0） */
    unsigned int    reserved;
    unsigned int    dbc;        /* Data Byte Count-1（bit31=IOC） */
};

/*
 * ahci_cmd_table - AHCI 命令表（包含 CFIS + PRDT）
 *
 * 必须 128 字节对齐。
 * CFIS 区固定 64 字节（存放 H2D Register FIS 或其他 FIS），
 * ACMD 区 16 字节（ATAPI 命令，磁盘不用），
 * 之后为 PRDT 数组（每个 16 字节）。
 */
struct ahci_cmd_table {
    unsigned char           cfis[64];      /* Command FIS（H2D Register FIS） */
    unsigned char           acmd[16];      /* ATAPI Command（磁盘置零） */
    unsigned char           reserved[48];  /* 保留 */
    struct ahci_prdt_entry  prdt[SATA_MAX_PRDT]; /* PRDT 数组 */
}; /* 64 + 16 + 48 + 8*16 = 256 字节 */

/*
 * sata_port - SATA 端口描述
 *
 * 每个在线端口对应一个 SATA 设备（硬盘或 ATAPI 光驱）。
 * 拥有独立的 Command List / FIS Receive / Command Table 缓冲区，
 * 避免多端口并发操作冲突。
 */
struct sata_port {
    struct sata_host       *host;           /* 所属 AHCI 控制器 */
    unsigned int            port_no;        /* 端口号 0~31 */
    volatile unsigned int  *mmio;           /* 端口 MMIO 虚拟基址（ioremap 后） */
    unsigned char           present;        /* 0=无设备, 1=设备在线 */
    unsigned char           type;           /* 0=SATA 磁盘, 1=ATAPI 光驱 */
    unsigned int            sectors;        /* LBA28 总扇区数 */
    unsigned long long      sectors48;      /* LBA48 总扇区数（支持 >128GB 磁盘） */
    char                    model[41];      /* 型号（40 字符 + '\0'） */
    char                    serial[21];     /* 序列号（20 字符 + '\0'） */
    unsigned short          identify[256];  /* IDENTIFY 原始数据（512 字节） */

    /* 每端口独立的 DMA 缓冲区（物理地址写入 AHCI 寄存器） */
    struct ahci_cmd_header *cmd_list;       /* Command List 虚拟地址（1024B 对齐） */
    unsigned int            cmd_list_phys;  /* Command List 物理地址 */
    unsigned char          *fis_recv;       /* FIS Receive 虚拟地址（256B 对齐） */
    unsigned int            fis_recv_phys;  /* FIS Receive 物理地址 */
    struct ahci_cmd_table  *cmd_table;      /* Command Table 虚拟地址（128B 对齐） */
    unsigned int            cmd_table_phys; /* Command Table 物理地址 */

    /* 中断驱动完成机制 */
    wait_queue_head_t       wait_queue;     /* 命令完成等待队列 */
    unsigned int            cmd_status;     /* ISR 写入的 PORT_IS 快照（唤醒后读取） */
};

/*
 * sata_host - AHCI 控制器描述
 *
 * 一个 AHCI 控制器（PCI 设备）管理最多 32 个 SATA 端口。
 * mmio_base_virt 为 BAR5 经 ioremap 后的虚拟地址，
 * 所有端口寄存器均基于此地址计算偏移。
 */
struct sata_host {
    struct pci_dev         *pdev;               /* 所属 PCI 设备 */
    unsigned int            mmio_base_phys;     /* BAR5 物理地址（PCI config 读取） */
    volatile unsigned int  *mmio_base_virt;     /* BAR5 ioremap 后虚拟地址 */
    unsigned char           num_ports;          /* CAP.NP + 1 */
    unsigned int            ports_implemented;  /* PI 寄存器：bit N = port N 可用 */
    unsigned int            version;            /* VS 寄存器：AHCI 版本号 */
    struct sata_port        ports[SATA_MAX_PORTS];
};

/* ======================== MMIO 读写辅助（内联） ======================== */

static inline unsigned int sata_readl(volatile unsigned int *addr)
{
    return *(volatile unsigned int *)addr;
}

static inline void sata_writel(volatile unsigned int *addr, unsigned int val)
{
    *(volatile unsigned int *)addr = val;
}

/* ======================== 公共 API ======================== */

/*
 * sata_init - SATA 子系统初始化入口
 *
 * 注册 PCI 驱动，匹配 class=0x010601 的 AHCI 控制器，
 * 与现有 ata_piix（class 0x010100）互不干扰。
 */
void sata_init(void);

/*
 * sata_read_sectors - DMA 方式读取扇区（LBA48）
 *
 * @port:  SATA 端口（必须 present=1）
 * @lba:   起始 LBA 地址（48 位，支持 >128GB）
 * @count: 扇区数（1~255）
 * @buf:   目标缓冲区（至少 count×512 字节）
 * 返回：0 成功，-1 失败
 */
int sata_read_sectors(struct sata_port *port, unsigned long long lba,
                      unsigned int count, void *buf);

/*
 * sata_write_sectors - DMA 方式写入扇区（LBA48）
 *
 * @port:  SATA 端口
 * @lba:   起始 LBA 地址
 * @count: 扇区数（1~255）
 * @buf:   源数据缓冲区（至少 count×512 字节）
 * 返回：0 成功，-1 失败
 */
int sata_write_sectors(struct sata_port *port, unsigned long long lba,
                       unsigned int count, const void *buf);

#endif /* __SATA_H__ */
