/*
 * LulaOS PCI 子系统实现
 *
 * 参考  drivers/pci/probe.c, drivers/pci/pci.c
 *
 * 实现功能：
 *   - 配置空间 I/O 端口访问（Type 1 机制）
 *   - Bus/Device/Function 递归枚举
 *   - BAR 资源解析（Memory/IO 类型识别 + 大小计算）
 *   - 中断引脚/线读取
 *   - pci_driver 注册与设备匹配框架
 *
 * 初始化时机：须在 kmem_cache_init() 之后调用（需要 kmalloc）
 */

#include <pci/pci.h>
#include <arch/x86/io.h>
#include <arch/x86/ptrace.h>
#include <interrupts/interrupts.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <stddef.h>
#include <mm/slab.h>

/* ======================== 全局状态 ======================== */

/* PCI 总线类型实例 */
struct bus_type pci_bus_type = {
    .name = "pci",
};

/* 已枚举设备计数 */
static unsigned int pci_device_count = 0;

/* ======================== 配置空间访问 ======================== */

/*
 * pci_config_read32 - 读取 PCI 配置空间 32 位寄存器
 *
 * Type 1 配置机制：
 *   CONFIG_ADDRESS (0xCF8): [31:使能][30:24:保留][23:16:Bus][15:11:Dev][10:8:Func][7:2:Reg]
 *   CONFIG_DATA    (0xCFC): 32 位数据
 *
 * offset 必须 4 字节对齐（低 2 位清零）
 */
unsigned int pci_config_read32(unsigned char bus, unsigned char devfn,
                               unsigned char offset)
{
    unsigned int address;

    address = (1U << 31)                           /* 使能位 */
            | ((unsigned int)bus << 16)            /* 总线号 */
            | ((unsigned int)devfn << 8)           /* devfn = (dev<<3)|fn */
            | (offset & 0xFC);                     /* 寄存器偏移（4 字节对齐） */

    outl(address, PCI_CONFIG_ADDRESS);
    return inl(PCI_CONFIG_DATA);
}

/*
 * pci_config_write32 - 写入 PCI 配置空间 32 位寄存器
 */
void pci_config_write32(unsigned char bus, unsigned char devfn,
                        unsigned char offset, unsigned int val)
{
    unsigned int address;

    address = (1U << 31)
            | ((unsigned int)bus << 16)
            | ((unsigned int)devfn << 8)
            | (offset & 0xFC);

    outl(address, PCI_CONFIG_ADDRESS);
    outl(val, PCI_CONFIG_DATA);
}

/* 辅助：读取 16 位（从 32 位寄存器中提取） */
static unsigned short pci_config_read16(unsigned char bus, unsigned char devfn,
                                        unsigned char offset)
{
    unsigned int val = pci_config_read32(bus, devfn, offset & ~0x03);
    return (unsigned short)(val >> ((offset & 0x02) * 8));
}

/* 辅助：读取 8 位 */
static unsigned char pci_config_read8(unsigned char bus, unsigned char devfn,
                                      unsigned char offset)
{
    unsigned int val = pci_config_read32(bus, devfn, offset & ~0x03);
    return (unsigned char)(val >> ((offset & 0x03) * 8));
}

/* ======================== BAR 解析 ======================== */

/*
 * pci_read_bars - 解析设备的 6 个 BAR 寄存器
 *
 * 流程：
 *   1. 读取 BAR 原始值
 *   2. 根据 bit0 判断类型：1=IO 空间，0=内存空间
 *   3. 写入全 1，读回 size mask（低位为 0 的位数 = BAR 大小）
 *   4. 恢复原始值
 *   5. 计算 start/end 地址
 */
static void pci_read_bars(struct pci_dev *dev)
{
    int i;

    for (i = 0; i < PCI_NUM_BARS; i++) {
        unsigned int bar_val, size_mask;
        unsigned char bar_offset = PCI_BAR0 + i * 4;

        bar_val = pci_config_read32(dev->bus, dev->devfn, bar_offset);
        if (bar_val == 0) {
            dev->resource[i].start = 0;
            dev->resource[i].end = 0;
            dev->resource[i].flags = 0;
            continue;
        }

        if (bar_val & 1) {
            /* I/O 空间 BAR */
            dev->resource[i].start = bar_val & ~0x3UL;
            dev->resource[i].flags = IORESOURCE_IO;

            /* 写全 1 计算大小 */
            pci_config_write32(dev->bus, dev->devfn, bar_offset, 0xFFFFFFFF);
            size_mask = pci_config_read32(dev->bus, dev->devfn, bar_offset);
            pci_config_write32(dev->bus, dev->devfn, bar_offset, bar_val);

            dev->resource[i].end = dev->resource[i].start + (~size_mask & ~0x3UL);
        } else {
            /* 内存空间 BAR */
            unsigned int type = (bar_val >> 1) & 0x3;  /* bits[2:1] */

            dev->resource[i].start = bar_val & ~0xFUL;
            dev->resource[i].flags = IORESOURCE_MEM;

            /* 写全 1 计算大小 */
            pci_config_write32(dev->bus, dev->devfn, bar_offset, 0xFFFFFFFF);
            size_mask = pci_config_read32(dev->bus, dev->devfn, bar_offset);
            pci_config_write32(dev->bus, dev->devfn, bar_offset, bar_val);

            dev->resource[i].end = dev->resource[i].start + (~size_mask & ~0xFUL);

            if (type == 2) {
                /* 64 位 BAR：下一个 BAR 是高 32 位，跳过 */
                if (i + 1 < PCI_NUM_BARS) {
                    i++;
                    dev->resource[i].start = 0;
                    dev->resource[i].end = 0;
                    dev->resource[i].flags = 0;
                }
            }
        }
    }
}

/* ======================== PCI 总线类型 ======================== */

/*
 * pci_match_device - 检查设备是否匹配 ID 表
 *
 * 遍历 id_table，任一字段为 0 表示通配
 */
static const struct pci_device_id *pci_match_device(
    const struct pci_device_id *ids, struct pci_dev *dev)
{
    if (!ids)
        return NULL;

    while (ids->vendor || ids->device || ids->class) {
        int match = 1;

        if (ids->vendor && ids->vendor != dev->vendor)
            match = 0;
        if (ids->device && ids->device != dev->device)
            match = 0;
        if (ids->class && ids->class != (dev->class >> 8))
            match = 0;

        if (match)
            return ids;
        ids++;
    }
    return NULL;
}

/*
 * pci_bus_match - PCI 总线匹配函数
 *
 * 检查设备的 vendor/device/class 是否匹配驱动的 id_table。
 */
static int pci_bus_match(struct device *dev, struct device_driver *drv)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = to_pci_driver(drv);

    return pci_match_device(pdrv->id_table, pdev) != NULL;
}

/*
 * pci_bus_probe - PCI 总线 probe 转发
 *
 * 匹配成功后，将统一设备模型的 probe 转发到 pci_driver->probe()，
 * 并传入匹配到的 pci_device_id 项。
 */
static int pci_bus_probe(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = to_pci_driver(dev->driver);
    const struct pci_device_id *id = pci_match_device(pdrv->id_table, pdev);

    if (id && pdrv->probe)
        return pdrv->probe(pdev, id);
    return 0;
}

/*
 * pci_bus_remove - PCI 总线 remove 转发
 */
static void pci_bus_remove(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = to_pci_driver(dev->driver);

    if (pdrv->remove)
        pdrv->remove(pdev);
}

/*
 * pci_bus_init - 注册 PCI 总线类型
 */
static int pci_bus_init(void)
{
    pci_bus_type.match = pci_bus_match;
    pci_bus_type.probe = pci_bus_probe;
    pci_bus_type.remove = pci_bus_remove;

    return bus_register(&pci_bus_type);
}

/* ======================== 设备扫描 ======================== */

/*
 * pci_scan_device - 扫描单个 PCI 设备
 *
 * 读取 Vendor ID，若有效则分配 pci_dev 结构并填充所有字段。
 * 返回：设备指针，或 NULL（无设备）
 */
static struct pci_dev *pci_scan_device(unsigned char bus, unsigned char devfn)
{
    unsigned int vend;
    unsigned int class_rev;
    unsigned int hdr;
    struct pci_dev *dev;

    /* 读取 Vendor ID（低 16 位），0xFFFF 表示无设备 */
    vend = pci_config_read32(bus, devfn, PCI_VENDOR_ID);
    if ((vend & 0xFFFF) == PCI_VENDOR_NONE)
        return NULL;

    /* 分配设备描述符 */
    dev = kmalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        printk("PCI: kmalloc failed for device %02x:%02x.%x\n",
               bus, devfn >> 3, devfn & 7);
        return NULL;
    }

    /* 清零 */
    unsigned int i;
    unsigned char *p = (unsigned char *)dev;
    for (i = 0; i < sizeof(*dev); i++)
        p[i] = 0;

    /* 填充基本标识 */
    dev->bus = bus;
    dev->devfn = devfn;
    dev->vendor = (unsigned short)(vend & 0xFFFF);
    dev->device = (unsigned short)((vend >> 16) & 0xFFFF);

    /* 类别码（24 位）和修订版本（8 位） */
    class_rev = pci_config_read32(bus, devfn, PCI_REVISION_ID);
    dev->revision = (unsigned char)(class_rev & 0xFF);
    dev->class = (class_rev >> 8) & 0xFFFFFF;  /* class(8) | subclass(8) | progif(8) */

    /* 头部类型 */
    hdr = pci_config_read32(bus, devfn, PCI_HEADER_TYPE);
    dev->header_type = (unsigned char)(hdr & 0xFF);

    /* 中断引脚和中断线 */
    unsigned int int_line = pci_config_read32(bus, devfn, PCI_INTERRUPT_LINE);
    dev->irq = (unsigned char)(int_line & 0xFF);
    dev->int_pin = (unsigned char)((int_line >> 8) & 0xFF);

    /* 解析 BAR */
    pci_read_bars(dev);

    /* 初始化链表节点 */
    INIT_LIST_HEAD(&dev->dev.bus_node);

    return dev;
}

/*
 * pci_scan_bus - 扫描指定总线上的所有设备
 *
 * 遍历 32 个设备 x 8 个功能：
 *   - 检查 Vendor ID 是否有效
 *   - 检查 Function 0 的 multi-function bit，决定是否需要扫描其他 Function
 */
static void pci_scan_bus(unsigned char bus)
{
    int dev, fn;

    for (dev = 0; dev < PCI_MAX_DEV; dev++) {
        unsigned char devfn_base = (unsigned char)(dev << 3);
        int is_multifunc = 0;

        for (fn = 0; fn < PCI_MAX_FUNC; fn++) {
            unsigned char devfn = devfn_base | fn;
            struct pci_dev *pdev;

            pdev = pci_scan_device(bus, devfn);
            if (pdev) {
                pdev->dev.bus = &pci_bus_type;
                pdev->dev.driver = NULL;
                pdev->dev.driver_data = NULL;

                /* 生成设备名称：BB:DD.F */
                snprintf(pdev->dev.name, DEVICE_NAME_SIZE, "%02x:%02x.%x",
                         bus, devfn >> 3, devfn & 7);

                device_register(&pdev->dev);
                pci_device_count++;
            }

            /* 只对 Function 0 检查 multi-function 标志 */
            if (fn == 0) {
                if (pdev && (pdev->header_type & PCI_HEADER_MULTIFUNC))
                    is_multifunc = 1;

                if (!is_multifunc)
                    break;  /* 单功能设备，跳过其他 Function */
            }
        }
    }
}

/* ======================== 设备查询 ======================== */

/*
 * pci_find_device - 按 Vendor/Device ID 查找设备
 *
 * vendor=0 匹配任意厂商，device=0 匹配任意设备
 */
struct pci_dev *pci_find_device(unsigned short vendor, unsigned short device)
{
    struct list_head *pos;

    list_for_each(pos, &pci_bus_type.devices) {
        struct device *d = list_entry(pos, struct device, bus_node);
        struct pci_dev *dev = to_pci_dev(d);

        if ((vendor == 0 || dev->vendor == vendor) &&
            (device == 0 || dev->device == device))
            return dev;
    }
    return NULL;
}

/*
 * pci_find_class - 按设备类别查找
 *
 * class 为 24 位类别码（高 8 位为基类）
 */
struct pci_dev *pci_find_class(unsigned int class)
{
    struct list_head *pos;

    list_for_each(pos, &pci_bus_type.devices) {
        struct device *d = list_entry(pos, struct device, bus_node);
        struct pci_dev *dev = to_pci_dev(d);

        /* 只比较高 16 位（基类 + 子类） */
        if ((dev->class >> 8) == (class >> 8))
            return dev;
    }
    return NULL;
}

/* ======================== 设备使能 ======================== */

/*
 * pci_enable_device - 使能设备的 I/O 和内存空间响应
 *
 * 设置命令寄存器 bit0（IO）和 bit1（MEM）
 */
void pci_enable_device(struct pci_dev *dev)
{
    unsigned int cmd;

    cmd = pci_config_read32(dev->bus, dev->devfn, PCI_COMMAND);
    cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEM);
    pci_config_write32(dev->bus, dev->devfn, PCI_COMMAND, cmd);
}

/* ======================== 驱动框架 ======================== */

/*
 * pci_register_driver - 注册 PCI 驱动
 *
 * 设置驱动所属总线为 pci_bus_type，然后调用统一设备模型接口
 * driver_register() 触发匹配。
 */
int pci_register_driver(struct pci_driver *drv)
{
    if (!drv || !drv->driver.name)
        return -1;

    drv->driver.bus = &pci_bus_type;
    INIT_LIST_HEAD(&drv->driver.bus_node);

    return driver_register(&drv->driver);
}

/*
 * pci_unregister_driver - 注销 PCI 驱动
 *
 * 调用统一设备模型接口 driver_unregister() 解除所有绑定。
 */
void pci_unregister_driver(struct pci_driver *drv)
{
    if (!drv)
        return;

    driver_unregister(&drv->driver);
}

/* ======================== 初始化入口 ======================== */

/*
 * pci_init - PCI 子系统初始化
 *
 * 流程：
 *   1. 从 Bus 0 开始扫描所有设备
 *   2. 打印枚举结果（类似 lspci）
 */
void pci_init(void)
{
    struct list_head *pos;

    printk("PCI: scanning buses...\n");

    /* 注册 PCI 总线类型 */
    pci_bus_init();

    /* 从 Bus 0 开始枚举（单总线系统通常只有 Bus 0） */
    pci_scan_bus(0);

    printk("PCI: found %d devices\n", pci_device_count);

    /* 打印设备列表 */
    list_for_each(pos, &pci_bus_type.devices) {
        struct device *d = list_entry(pos, struct device, bus_node);
        struct pci_dev *dev = to_pci_dev(d);
        int i;

        printk("PCI: %02x:%02x.%x [%04x:%04x] class=%06x rev=%02x irq=%d pin=%d\n",
               dev->bus,
               dev->devfn >> 3,
               dev->devfn & 7,
               dev->vendor,
               dev->device,
               dev->class,
               dev->revision,
               dev->irq,
               dev->int_pin);

        /* 打印 BAR 资源 */
        for (i = 0; i < PCI_NUM_BARS; i++) {
            if (dev->resource[i].flags == 0)
                continue;
            printk("      BAR%d: %s [%08lx - %08lx]\n",
                   i,
                   (dev->resource[i].flags & IORESOURCE_IO) ? "IO " : "MEM",
                   dev->resource[i].start,
                   dev->resource[i].end);
        }
    }
}
