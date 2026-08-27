/*
 * LulaOS PCI 子系统
 *
 *  include/linux/pci.h 和 drivers/pci/
 * 实现 PCI 配置空间访问、Bus/Device/Function 枚举、BAR 解析、
 * 中断路由和 pci_driver 注册框架。
 *
 * 配置空间访问方式：I/O 端口（Type 1）
 *   - CONFIG_ADDRESS (0xCF8): 写入 BDF + 偏移地址
 *   - CONFIG_DATA    (0xCFC): 读写 32 位配置数据
 */

#ifndef __PCI_H__
#define __PCI_H__

#include <device/device.h>
#include <libs/list.h>

/* ======================== 配置空间端口 ======================== */

#define PCI_CONFIG_ADDRESS  0xCF8   /* 配置地址端口 */
#define PCI_CONFIG_DATA     0xCFC   /* 配置数据端口 */

/* ======================== 配置空间偏移（Header Type 0） ======================== */

#define PCI_VENDOR_ID       0x00    /* 16 bit: 厂商 ID */
#define PCI_DEVICE_ID       0x02    /* 16 bit: 设备 ID */
#define PCI_COMMAND         0x04    /* 16 bit: 命令寄存器 */
#define PCI_STATUS          0x06    /* 16 bit: 状态寄存器 */
#define PCI_REVISION_ID     0x08    /* 8 bit: 修订版本 */
#define PCI_CLASS_PROG      0x09    /* 8 bit: 编程接口 */
#define PCI_CLASS_DEVICE    0x0A    /* 16 bit: 子类 + 基类 */
#define PCI_CACHE_LINE_SIZE 0x0C    /* 8 bit */
#define PCI_LATENCY_TIMER   0x0D    /* 8 bit */
#define PCI_HEADER_TYPE     0x0E    /* 8 bit: 头部类型 */
#define PCI_BIST            0x0F    /* 8 bit: 内置自检 */
#define PCI_BAR0            0x10    /* 32 bit x6: BAR0~BAR5 */
#define PCI_CARDBUS_CIS     0x28    /* 32 bit */
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C /* 16 bit */
#define PCI_SUBSYSTEM_ID    0x2E    /* 16 bit */
#define PCI_ROM_ADDRESS     0x30    /* 32 bit: 扩展 ROM 基址 */
#define PCI_CAPABILITY_LIST 0x34    /* 8 bit */
#define PCI_INTERRUPT_LINE  0x3C    /* 8 bit: 中断线 */
#define PCI_INTERRUPT_PIN   0x3D    /* 8 bit: 中断引脚 */
#define PCI_MIN_GNT         0x3E    /* 8 bit */
#define PCI_MAX_LAT         0x3F    /* 8 bit */

/* ======================== 常量 ======================== */

#define PCI_VENDOR_NONE     0xFFFF  /* 无设备时的 Vendor ID */
#define PCI_MAX_BUS         256     /* 最多 256 条总线 */
#define PCI_MAX_DEV         32      /* 每条总线最多 32 个设备 */
#define PCI_MAX_FUNC        8       /* 每个设备最多 8 个功能 */
#define PCI_NUM_BARS        6       /* Header Type 0 有 6 个 BAR */

/* 命令寄存器位 */
#define PCI_COMMAND_IO      0x0001  /* I/O 空间响应使能 */
#define PCI_COMMAND_MEM     0x0002  /* 内存空间响应使能 */
#define PCI_COMMAND_MASTER  0x0004  /* Bus Master 使能 */

/* 头部类型位 */
#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_HEADER_MULTIFUNC    0x80  /* bit7=1 表示多功能设备 */

/* 设备类别（高 8 位） */
#define PCI_CLASS_BRIDGE        0x06  /* 桥接设备 */
#define PCI_CLASS_DISPLAY       0x03  /* 显示控制器 */
#define PCI_CLASS_NETWORK       0x02  /* 网络控制器 */
#define PCI_CLASS_STORAGE       0x01  /* 存储控制器 */

/* ======================== 资源标志 ======================== */

#define IORESOURCE_IO       0x00000100
#define IORESOURCE_MEM      0x00000200

/* ======================== 数据结构 ======================== */

/*
 * pci_resource - 单个 BAR 的资源描述
 *
 * 参考  struct resource
 */
struct pci_resource {
    unsigned long start;     /* BAR 起始地址（物理地址或 I/O 端口） */
    unsigned long end;       /* BAR 结束地址 */
    unsigned long flags;     /* IORESOURCE_MEM / IORESOURCE_IO */
};

/*
 * pci_dev - PCI 设备描述符
 *
 * 参考   struct pci_dev（简化版）
 * 嵌入 struct device 作为第一个成员，以便挂入统一设备模型。
 */
struct pci_dev {
    struct device dev;                  /* 必须作为第一个成员嵌入 */

    /* 设备标识 */
    unsigned char   bus;                /* 总线号 */
    unsigned char   devfn;              /* (device << 3) | function */
    unsigned short  vendor;             /* 厂商 ID */
    unsigned short  device;             /* 设备 ID */
    unsigned int    class;              /* 类别码: (class<<16)|(subclass<<8)|prog_if */
    unsigned char   revision;           /* 修订版本 */
    unsigned char   header_type;        /* 头部类型（bit7=多功能） */

    /* 中断信息 */
    unsigned char   irq;                /* 中断向量号（映射后） */
    unsigned char   int_pin;            /* 中断引脚 INTA~INTD (1~4)，0=无 */

    /* BAR 资源 */
    struct pci_resource resource[PCI_NUM_BARS];
};

/*
 * pci_device_id - 设备匹配 ID 表项
 *
 * vendor/device 为 0 表示匹配任意
 * 驱动通过 id_table 数组声明支持的设备列表，以 {0} 结尾
 */
struct pci_device_id {
    unsigned short vendor;              /* 0 = 匹配任意厂商 */
    unsigned short device;              /* 0 = 匹配任意设备 */
    unsigned int   class;               /* 0 = 匹配任意类别 */
};

/*
 * pci_driver - PCI 驱动描述符
 *
 * 参考 Linux 2.6.20 struct pci_driver（简化版）
 * 嵌入 struct device_driver 作为第一个成员，以便挂入统一设备模型。
 */
struct pci_driver {
    struct device_driver driver;        /* 必须作为第一个成员嵌入 */
    const struct pci_device_id *id_table; /* 支持的设备 ID 表（{0}结尾） */
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
};

/* 辅助宏：从 struct device * 获取 pci_dev / pci_driver */
#define to_pci_dev(d)       container_of(d, struct pci_dev, dev)
#define to_pci_driver(d)    container_of(d, struct pci_driver, driver)

/* ======================== 公共 API ======================== */

/* 配置空间 32 位访问 */
unsigned int  pci_config_read32(unsigned char bus, unsigned char devfn,
                                unsigned char offset);
void          pci_config_write32(unsigned char bus, unsigned char devfn,
                                 unsigned char offset, unsigned int val);

/* 子系统初始化（枚举所有总线上的设备） */
void pci_init(void);

/* 总线类型（供统一设备模型使用） */
extern struct bus_type pci_bus_type;

/* 设备查询 */
struct pci_dev *pci_find_device(unsigned short vendor, unsigned short device);
struct pci_dev *pci_find_class(unsigned int class);

/* 驱动框架 */
int  pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);

/* 设备使能（开启 I/O 和内存空间响应） */
void pci_enable_device(struct pci_dev *dev);

#endif /* __PCI_H__ */
