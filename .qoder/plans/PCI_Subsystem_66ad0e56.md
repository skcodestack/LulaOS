# PCI 子系统实现计划

## 现状分析

| 现有资源 | 状态 |
|---------|------|
| `io.h` | 有 inb/outb/inw/outw，**缺少 inl/outl**（PCI 需 32 位端口 I/O） |
| `list.h` | 完整 Linux 风格链表，可直接使用 |
| `slab.h` | kmalloc/kfree 可用（须在 kmem_cache_init 之后调用） |
| 初始化顺序 | PCI 须在 kmem_cache_init() 之后、bsp_start_idle() 之前 |

---

## Task 1: 补充 `includes/arch/x86/io.h` — 添加 inl/outl

**文件**: `includes/arch/x86/io.h`

PCI 配置空间寄存器为 32 位，需要 `inl`/`outl`：

```c
static inline void outl(unsigned long value, unsigned short port) {
    asm volatile("outl %0, %1" : : "a" (value), "Nd" (port));
}

static inline unsigned long inl(unsigned short port) {
    unsigned long value;
    asm volatile("inl %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}
```

---

## Task 2: 创建 `includes/pci/pci.h` — PCI 头文件

**文件**: `includes/pci/pci.h`（新建）

### 配置空间偏移（标准 PCI 256 字节 Header Type 0）

```c
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* Header 字段偏移 */
#define PCI_VENDOR_ID       0x00   /* 16 bit */
#define PCI_DEVICE_ID       0x02   /* 16 bit */
#define PCI_COMMAND         0x04   /* 16 bit */
#define PCI_STATUS          0x06   /* 16 bit */
#define PCI_REVISION_ID     0x08   /* 8 bit  */
#define PCI_CLASS_PROG      0x09   /* 8 bit  */
#define PCI_CLASS_DEVICE    0x0A   /* 16 bit (subclass + class) */
#define PCI_HEADER_TYPE     0x0E   /* 8 bit  */
#define PCI_BAR0            0x10   /* 32 bit x6 (BAR0~BAR5) */
#define PCI_INTERRUPT_LINE  0x3C   /* 8 bit  */
#define PCI_INTERRUPT_PIN   0x3D   /* 8 bit  */

#define PCI_VENDOR_NONE     0xFFFF
#define PCI_MAX_BUS         256
#define PCI_MAX_DEV         32
#define PCI_MAX_FUNC        8
#define PCI_NUM_BARS        6
```

### 核心数据结构（参考 Linux 2.6.20 `include/linux/pci.h`）

```c
struct pci_resource {
    unsigned long start;     /* BAR 起始地址（物理/IO） */
    unsigned long end;       /* BAR 结束地址 */
    unsigned long flags;     /* IORESOURCE_MEM / IORESOURCE_IO */
};

#define IORESOURCE_IO   0x00000100
#define IORESOURCE_MEM  0x00000200

struct pci_dev {
    struct list_head list;           /* 链入全局 pci_devices */
    unsigned char  bus;              /* 总线号 */
    unsigned char  devfn;            /* (device << 3) | function */
    unsigned short vendor;
    unsigned short device;
    unsigned int   class;            /* (class << 16) | (subclass << 8) | prog_if */
    unsigned char  revision;
    unsigned char  header_type;
    unsigned char  irq;              /* 中断向量号（映射后） */
    unsigned char  int_pin;          /* INTA~INTD (1~4) */
    struct pci_resource resource[PCI_NUM_BARS];
    void           *driver_data;     /* 驱动私有数据 */
};

struct pci_device_id {
    unsigned short vendor;           /* 0 = 匹配任意 */
    unsigned short device;           /* 0 = 匹配任意 */
    unsigned int   class;            /* 0 = 匹配任意 */
};

struct pci_driver {
    struct list_head list;           /* 链入全局 pci_drivers */
    const char    *name;
    const struct pci_device_id *id_table;   /* NULL 结尾 */
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
};
```

### API 声明

```c
/* 配置空间访问 */
unsigned int  pci_config_read32(unsigned char bus, unsigned char devfn, unsigned char offset);
void          pci_config_write32(unsigned char bus, unsigned char devfn, unsigned char offset, unsigned int val);

/* 子系统初始化（枚举所有设备） */
void pci_init(void);

/* 设备查询 */
struct pci_dev *pci_find_device(unsigned short vendor, unsigned short device);
struct pci_dev *pci_find_class(unsigned int class);

/* 驱动框架 */
int  pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);

/* 命令寄存器操作 */
void pci_enable_device(struct pci_dev *dev);

/* 全局设备链表 */
extern struct list_head pci_devices;
```

---

## Task 3: 创建 `kernel/pci/pci.c` — PCI 子系统实现

**文件**: `kernel/pci/pci.c`（新建）

### 配置空间 I/O 端口访问

```c
unsigned int pci_config_read32(unsigned char bus, unsigned char devfn, unsigned char offset) {
    unsigned int address = (1U << 31) | ((unsigned int)bus << 16)
                         | ((unsigned int)devfn << 8) | (offset & 0xFC);
    outl(address, PCI_CONFIG_ADDRESS);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(unsigned char bus, unsigned char devfn, unsigned char offset, unsigned int val) {
    unsigned int address = (1U << 31) | ((unsigned int)bus << 16)
                         | ((unsigned int)devfn << 8) | (offset & 0xFC);
    outl(address, PCI_CONFIG_ADDRESS);
    outl(val, PCI_CONFIG_DATA);
}
```

### 设备枚举（参考 Linux `drivers/pci/probe.c`）

扫描流程：
1. 遍历 Bus 0~255，每个总线 32 个设备 x 8 个 Function
2. 读 Vendor ID（0xFFFF 表示不存在，跳过）
3. 检查 Header Type bit7（multi-function），若为 0 只扫描 Function 0
4. 读取完整配置空间，填充 `pci_dev`
5. 解析 BAR（Memory vs IO、大小计算）
6. 读取中断引脚和 IRQ 线，映射到向量号

```c
static struct pci_dev *pci_scan_device(unsigned char bus, unsigned char devfn) {
    unsigned int vend = pci_config_read32(bus, devfn, PCI_VENDOR_ID);
    if ((vend & 0xFFFF) == PCI_VENDOR_NONE) return NULL;

    struct pci_dev *dev = kmalloc(sizeof(*dev), GFP_KERNEL);
    /* 填充所有字段，解析 BAR... */
    return dev;
}

static void pci_scan_bus(unsigned char bus) {
    for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
        for (int fn = 0; fn < PCI_MAX_FUNC; fn++) {
            unsigned char devfn = (dev << 3) | fn;
            struct pci_dev *pdev = pci_scan_device(bus, devfn);
            if (pdev) {
                list_add_tail(&pdev->list, &pci_devices);
                pci_device_count++;
            }
            if (fn == 0) {
                /* 检查 multi-function bit */
                unsigned int hdr = pci_config_read32(bus, devfn, PCI_HEADER_TYPE);
                if (!(hdr & 0x80)) break;
            }
        }
    }
}
```

### BAR 解析

```c
static void pci_read_bars(struct pci_dev *dev) {
    for (int i = 0; i < PCI_NUM_BARS; i++) {
        unsigned int bar = pci_config_read32(dev->bus, dev->devfn, PCI_BAR0 + i*4);
        if (bar == 0) continue;

        if (bar & 1) {
            /* I/O 空间 */
            dev->resource[i].start = bar & ~0x3;
            dev->resource[i].flags = IORESOURCE_IO;
        } else {
            /* 内存空间 */
            dev->resource[i].start = bar & ~0xF;
            dev->resource[i].flags = IORESOURCE_MEM;
        }
        /* 写入全 1 计算大小 */
        pci_config_write32(dev->bus, dev->devfn, PCI_BAR0 + i*4, 0xFFFFFFFF);
        unsigned int size_mask = pci_config_read32(dev->bus, dev->devfn, PCI_BAR0 + i*4);
        pci_config_write32(dev->bus, dev->devfn, PCI_BAR0 + i*4, bar);  /* 恢复 */
        dev->resource[i].end = dev->resource[i].start + (~size_mask & (bar & 1 ? ~0x3 : ~0xF));
    }
}
```

### 驱动匹配框架

```c
int pci_register_driver(struct pci_driver *drv) {
    list_add_tail(&drv->list, &pci_drivers_list);
    /* 遍历已枚举设备，尝试匹配 */
    struct list_head *pos;
    list_for_each(pos, &pci_devices) {
        struct pci_dev *dev = list_entry(pos, struct pci_dev, list);
        const struct pci_device_id *id = pci_match_device(drv->id_table, dev);
        if (id && drv->probe)
            drv->probe(dev, id);
    }
    return 0;
}
```

### pci_init() 入口

```c
void pci_init(void) {
    printk("PCI: scanning buses...\n");
    pci_scan_bus(0);   /* 从 Bus 0 开始 */
    printk("PCI: found %d devices\n", pci_device_count);
    /* 打印设备列表 */
    struct list_head *pos;
    list_for_each(pos, &pci_devices) {
        struct pci_dev *dev = list_entry(pos, struct pci_dev, list);
        printk("PCI: %02x:%02x.%x [%04x:%04x] class=%06x irq=%d\n",
               dev->bus, dev->devfn >> 3, dev->devfn & 7,
               dev->vendor, dev->device, dev->class, dev->irq);
    }
}
```

---

## Task 4: 在 `kernel/kernel.c` 中集成 pci_init()

**文件**: `kernel/kernel.c`

在 `kmem_cache_init()` 之后、`bsp_start_idle()` 之前插入：

```c
kmem_cache_init();

/* PCI 总线枚举（需要 kmalloc 就绪） */
pci_init();
```

---

## 文件变更总结

| 文件 | 操作 |
|------|------|
| `includes/arch/x86/io.h` | 添加 inl/outl |
| `includes/pci/pci.h` | 新建，常量 + 结构 + API 声明 |
| `kernel/pci/pci.c` | 新建，配置空间访问 + 枚举 + BAR + 驱动框架 |
| `kernel/kernel.c` | 添加 pci_init() 调用（kmem_cache_init 之后） |

---

## 预期启动日志

```
PCI: scanning buses...
PCI: 00:00.0 [8086:1237] class=060000 irq=0    ← Host Bridge
PCI: 00:01.0 [8086:7000] class=060100 irq=0    ← ISA Bridge
PCI: 00:01.1 [8086:7010] class=010180 irq=0    ← IDE Controller
PCI: 00:02.0 [1234:1111] class=030000 irq=11   ← Bochs VGA
PCI: found 4 devices
```
