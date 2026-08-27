# Linux 总线-设备-驱动模型

> 本文档总结 Linux 内核的统一设备模型架构，作为 LulaOS 驱动子系统设计的参考。

---

## 1. 核心设计思想

Linux 驱动模型的核心是**三层分离**：

```
总线（Bus）   ← 定义"怎么匹配"
设备（Device） ← 描述"有什么硬件"
驱动（Driver） ← 描述"怎么用硬件"
```

三者通过 `bus_type` 连接，核心代码不关心具体总线细节，每种总线各自实现匹配和探测逻辑。

---

## 2. bus_type：匹配策略的抽象

### 2.1 为什么需要 bus_type

不同总线的匹配规则完全不同：

| 总线 | 匹配方式 | 发现方式 |
|------|---------|---------|
| PCI | vendor_id + device_id（16位数字） | 启动时扫描配置空间 |
| USB | vendor + product + class | 插入时热插拔枚举 |
| Platform | name 或 compatible 字符串 | 设备树/ACPI/板级代码声明 |
| I2C | device name | 总线扫描或设备树 |

`bus_type` 让核心代码统一处理，具体实现各自负责：

```c
struct bus_type {
    const char *name;

    // 每个总线自己实现 match 函数
    int (*match)(struct device *dev, struct device_driver *drv);

    // 每个总线自己实现 probe 调用方式
    int (*probe)(struct device *dev);

    // 每个总线自己实现 uevent（通知用户空间）
    int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
};
```

### 2.2 核心流程

```
device_register(dev)
     │
     ▼
dev->bus->match(dev, drv)   ← 调用总线特定的匹配函数
     │
     ▼
dev->bus->probe(dev)        ← 调用总线特定的 probe
```

---

## 3. PCI 总线

### 3.1 设备发现：启动时扫描

PCI 设备通过**扫描配置空间**发现：

```
CPU 写 0xCF8（BDF 地址）→ 读 0xCFC（数据）
     │
     ▼
Vendor ID == 0xFFFF → 无设备，跳过
Vendor ID != 0xFFFF → 有设备，创建 pci_dev
```

扫描流程：
1. 遍历 Bus 0~255，每个总线 32 个设备 × 8 个 Function
2. 读 Vendor ID，0xFFFF 跳过
3. 检查 Header Type bit7（multi-function），决定是否扫描其他 Function
4. 解析 BAR（Memory/IO 地址 + 大小）
5. 读取中断引脚和 IRQ 线

### 3.2 配置空间结构（Header Type 0）

```
偏移    大小    字段
0x00    16bit   Vendor ID
0x02    16bit   Device ID
0x04    16bit   Command（IO/MEM/Master 使能）
0x06    16bit   Status
0x08    8bit    Revision ID
0x09    24bit   Class Code（基类+子类+编程接口）
0x0E    8bit    Header Type（bit7=多功能）
0x10    32bit×6 BAR0~BAR5（资源基地址）
0x3C    8bit    Interrupt Line（ISA IRQ 号）
0x3D    8bit    Interrupt Pin（INTA~INTD）
```

### 3.3 BAR 解析

```c
// 1. 读取 BAR 原始值
unsigned int bar = pci_config_read32(bus, devfn, PCI_BAR0 + i*4);

// 2. 判断类型
if (bar & 1) → I/O 空间（bit0=1）
else          → 内存空间

// 3. 写全 1 计算大小
pci_config_write32(bus, devfn, offset, 0xFFFFFFFF);
size_mask = pci_config_read32(bus, devfn, offset);
pci_config_write32(bus, devfn, offset, bar);  // 恢复

// 4. size_mask 低位 0 的位数 = BAR 大小
```

### 3.4 PCI 中断路由

```
PCI 设备
  │
  ├─ PCI_INTERRUPT_PIN (1=INTA, 2=INTB, 3=INTC, 4=INTD)
  │    ↓ 主板中断路由
  ├─ PCI_INTERRUPT_LINE (BIOS 分配的 ISA IRQ 号)
  │    ↓ ACPI MADT Interrupt Source Override
  ├─ GSI (Global System Interrupt)
  │    ↓ IOAPIC RTE 映射
  └─ Vector (CPU 中断向量号)
```

### 3.5 PCI 驱动注册

```c
// 声明支持的设备列表
static struct pci_device_id my_ids[] = {
    { vendor: 0x1234, device: 0x1111 },  // Bochs VGA
    { 0 }  // 结尾
};

// 驱动结构
static struct pci_driver my_driver = {
    .name = "my_vga",
    .id_table = my_ids,
    .probe = my_probe,
    .remove = my_remove,
};

// 注册
pci_register_driver(&my_driver);
```

---

## 4. USB 总线

### 4.1 设备发现：热插拔事件驱动

USB 设备不是"扫描"出来的，而是**插入时由 Host Controller 检测**：

```
用户插入 USB 设备
     │
     ▼
Host Controller (xHCI/EHCI/UHCI) 检测端口变化
     │
     ▼
硬件中断 → Host Controller Driver 响应
     │
     ▼
USB Core 发起枚举：
  1. 读取设备描述符（idVendor, idProduct, bDeviceClass）
  2. 分配 Device Address
  3. 读取配置描述符 + 接口描述符
  4. 创建 usb_device 结构
  5. device_register(&udev->dev)  → 挂到 usb_bus_type
     │
     ▼
usb_bus_type.match() 遍历已注册驱动
     │
     ▼
匹配成功 → 调用 driver->probe()
```

### 4.2 USB 与 PCI 对比

| 步骤 | PCI | USB |
|------|-----|-----|
| 发现时机 | 启动时一次性扫描 | 运行时热插拔 |
| 触发者 | CPU 主动扫描 | 硬件中断（端口变化） |
| 读取方式 | 配置空间（MMIO/IO） | 控制传输（Control Transfer） |
| 设备结构 | `struct pci_dev` | `struct usb_device` |
| 挂载总线 | `pci_bus_type` | `usb_bus_type` |
| 匹配键 | vendor + device | vendor + product + class |
| 热插拔 | 有限支持 | 原生支持 |

### 4.3 为什么 USB 不能像 PCI 那样扫描？

- USB 设备通过端口动态插拔，没有固定地址
- 必须由 Host Controller 分配 Device Address
- 通信协议复杂（4 种传输类型），不能直接读写
- 设备发现是**异步事件驱动**的过程

---

## 5. Platform 总线

### 5.1 用途

给**没有标准发现机制**的设备使用：
- SoC 内部外设（UART、GPIO、Timer）
- 固定地址的硬件
- 中断控制器

### 5.2 设备来源（三种方式）

#### 方式 1：设备树（Device Tree）

```
// .dts 文件
&uart0 {
    compatible = "ti,omap4-uart";   ← 驱动用这个匹配
    reg = <0x4806a000 0x100>;       ← MMIO 地址
    interrupts = <72>;              ← IRQ 号
};
```

内核解析设备树，为每个节点创建 `platform_device`。

#### 方式 2：ACPI 表

```
// ACPI DSDT
Device (UART) {
    Name (_HID, "PNP0501")         ← 硬件 ID
    Name (_CRS, ResourceTemplate() {
        Memory32Fixed(ReadWrite, 0xFEC00000, 0x100)
        Interrupt(ResourceConsumer, Level, ActiveHigh) { 4 }
    })
}
```

内核解析 ACPI，创建设备。

#### 方式 3：板级代码（旧式）

```c
static struct platform_device uart_device = {
    .name = "omap-uart",
    .id = 0,
    .resource = uart_resources,
};
platform_device_register(&uart_device);  // 静态注册
```

---

## 6. 注册匹配时机

驱动和设备的注册顺序不确定，Linux 两种情况都支持：

### 情况 1：驱动先注册，设备后枚举

```
pci_register_driver(vga_driver)   ← 驱动先注册，设备链表为空
     │
     ▼ 保存驱动到链表，等待...

pci_init()                        ← 之后枚举设备
     │
     ▼ 每个新设备检查所有已注册驱动的 id_table
     │
     └─ 匹配 → 调用 probe()
```

### 情况 2：设备先枚举，驱动后注册

```
pci_init()                        ← 先枚举，设备入链表

pci_register_driver(nic_driver)   ← 之后加载驱动模块
     │
     ▼ 遍历所有已枚举设备，用 id_table 匹配
     │
     └─ 匹配 → 调用 probe()
```

**核心规则**：每次注册驱动时扫描设备，每次枚举设备时扫描驱动。

---

## 7. 完整驱动模型分层

```
┌─────────────────────────────────────────────────────────┐
│                    用户空间                               │
│  udev / systemd                                         │
│  ├─ 读取 /sys 发现新设备                                 │
│  ├─ 根据 modalias 自动加载模块                            │
│  └─ 创建设备节点 /dev/sda, /dev/dri/card0               │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────┼─────────────────────────────────┐
│                    内核                                   │
│                       │                                   │
│  ┌────────────────────┼───────────────────────────────┐ │
│  │         统一设备模型（Device Model）                 │ │
│  │                                                      │ │
│  │  bus_type       device         device_driver         │ │
│  │  ├─ pci_bus     ├─ pci_dev     ├─ pci_driver        │ │
│  │  ├─ platform    ├─ platform_dev├─ platform_driver   │ │
│  │  └─ usb_bus     ├─ usb_device  └─ usb_driver        │ │
│  │                                                      │ │
│  │  统一匹配: bus->match(dev, drv)                      │ │
│  │  统一探测: drv->probe(dev)                           │ │
│  └──────────────────────────────────────────────────────┘ │
│                       │                                   │
│  ┌────────────────────┼───────────────────────────────┐ │
│  │         总线特定层                                   │ │
│  │                                                      │ │
│  │  PCI                    Platform                     │ │
│  │  ├─ pci_scan_bus()     ├─ 设备树/ACPI 描述          │ │
│  │  ├─ BAR 解析           ├─ MMIO 地址                  │ │
│  │  ├─ pci_enable_device()├─ platform_device_register() │ │
│  │  └─ pci_register_driver()                            │ │
│  └──────────────────────────────────────────────────────┘ │
│                       │                                   │
│  ┌────────────────────┼───────────────────────────────┐ │
│  │         资源管理层                                   │ │
│  │                                                      │ │
│  │  ├─ request_mem_region()  独占声明内存区域           │ │
│  │  ├─ ioremap()             物理→虚拟地址映射          │ │
│  │  ├─ request_irq()         注册中断处理               │ │
│  │  └─ dma_set_mask()        DMA 地址能力声明           │ │
│  └──────────────────────────────────────────────────────┘ │
│                       │                                   │
│  ┌────────────────────┼───────────────────────────────┐ │
│  │         实际驱动                                     │ │
│  │                                                      │ │
│  │  probe()   → 初始化硬件、映射资源、注册中断          │ │
│  │  remove()  → 释放资源、注销中断                      │ │
│  │  suspend() / resume() → 电源管理                     │ │
│  └──────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

---

## 8. 职责分工总结

| 职责 | 总线子系统（内核） | 驱动（开发者） |
|------|------------------|---------------|
| 发现硬件 | 扫描/枚举设备 | 不负责 |
| 读取资源（BAR/IRQ） | 解析配置空间 | 不负责 |
| 声明支持哪些设备 | 不知道 | id_table 数组 |
| 初始化设备 | 不知道怎么用 | probe() 回调 |
| 释放资源 | | remove() 回调 |
| 匹配判断 | 比较 vendor/device/class/name | 只提供匹配条件 |
| 资源独占声明 | | request_mem_region() |
| 地址映射 | | ioremap() |
| DMA 配置 | | dma_set_mask() |

---

## 9. 对 LulaOS 的启示

### 当前已实现

- PCI 枚举（pci_scan_bus）
- BAR 解析（pci_read_bars）
- 驱动注册（pci_register_driver + id_table 匹配）

### 后续可扩展

| 功能 | 优先级 | 说明 |
|------|--------|------|
| Platform 总线 | 高 | 支持 UART、GPIO 等非 PCI 设备 |
| 统一设备模型 | 中 | struct device + struct bus_type |
| request_mem_region | 中 | 资源独占，防止驱动冲突 |
| DMA 支持 | 中 | 磁盘/网卡驱动必需 |
| MSI/MSI-X | 低 | 高性能中断，PCIe 设备 |
| Sysfs 导出 | 低 | 用户空间设备管理 |
| 模块自动加载 | 低 | 需要用户空间支持 |
