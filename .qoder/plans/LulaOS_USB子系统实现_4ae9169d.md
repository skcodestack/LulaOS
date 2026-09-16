# LulaOS USB 子系统实现

## 架构概览

```
pci_init() 扫描 PCI 总线
    ↓
uhci_pci_probe() 发现 class=0x0C0300 的 UHCI 控制器
    ↓
usb_hcd_register() 创建 usb_bus + usb_device(root hub)
    ↓
device_register() 挂入 usb_bus_type，触发设备/驱动匹配
```

新增文件结构：
```
includes/usb/
  ├── usb_ch9.h       USB Chapter 9 标准描述符（常量+结构体）
  ├── usb.h           USB 核心类型（usb_device/usb_driver/usb_bus/usb_device_id）
  ├── hcd.h           主机控制器驱动抽象（usb_hcd/hc_driver）
  └── uhci.h          UHCI 寄存器定义与私有结构

kernel/usb/
  ├── usb.c           USB 总线注册、设备/驱动注册、match/probe
  ├── hcd.c           HCD 框架（注册/注销 HCD）
  └── uhci-hcd.c      UHCI 主机控制器 PCI 驱动
```

---

## Task 1：创建 `includes/usb/usb_ch9.h`

参考 `linux-2.6.20/include/linux/usb_ch9.h`，提取 USB 2.0 Chapter 9 标准内容：

- 方向/类型/接收者常量：`USB_DIR_IN/OUT`、`USB_TYPE_*`、`USB_RECIP_*`
- 标准请求码：`USB_REQ_GET_DESCRIPTOR`、`USB_REQ_SET_ADDRESS`、`USB_REQ_SET_CONFIGURATION` 等
- 描述符类型码：`USB_DT_DEVICE`、`USB_DT_CONFIG`、`USB_DT_INTERFACE`、`USB_DT_ENDPOINT` 等
- 描述符结构体（packed）：`usb_device_descriptor`、`usb_config_descriptor`、`usb_interface_descriptor`、`usb_endpoint_descriptor`
- 设备类别码：`USB_CLASS_HID`、`USB_CLASS_MASS_STORAGE`、`USB_CLASS_HUB` 等
- 端点属性常量：`USB_ENDPOINT_XFER_CONTROL/BULK/INT/ISOC`
- 速度枚举：`enum usb_device_speed`（LOW/FULL/HIGH）
- 状态枚举：`enum usb_device_state`

---

## Task 2：创建 `includes/usb/usb.h`

参考 `linux-2.6.20/include/linux/usb.h`，定义核心结构：

- `struct usb_host_endpoint`：嵌入 `usb_endpoint_descriptor` + 私有字段
- `struct usb_host_interface`：嵌入 `usb_interface_descriptor` + endpoint 数组指针
- `struct usb_interface`：altsetting 数组 + `struct device dev`（挂入设备模型）
- `struct usb_host_config`：嵌入 `usb_config_descriptor` + interface 指针数组
- `struct usb_bus`：busnum、root_hub 指针、bus_list
- `struct usb_device`：devnum、speed、state、parent、bus、descriptor、config/actconfig、`struct device dev`（挂入设备模型）
- `struct usb_device_id`：match_flags、idVendor、idProduct、bDeviceClass/SubClass/Protocol（用于驱动匹配表）
- `struct usb_driver`：name、probe、disconnect、id_table、`struct device_driver driver`（嵌入设备模型）
- `to_usb_device()` / `to_usb_driver()` 宏
- `usb_bus_type` 外部声明
- Pipe 宏：`usb_sndctrlpipe`、`usb_rcvctrlpipe`、`usb_sndbulkpipe` 等
- 公共 API 声明：`usb_register_driver()`、`usb_deregister()`、`usb_init()`

---

## Task 3：创建 `kernel/usb/usb.c`

参考 `drivers/usb/core/usb.c`、`drivers/usb/core/driver.c`：

- 定义 `struct bus_type usb_bus_type`：
  - `.name = "usb"`
  - `.match`：基于 `usb_device_id` 表匹配 vendor/product 或 class/subclass/protocol
  - `.probe`：调用 `usb_driver->probe()`
  - `.remove`：调用 `usb_driver->disconnect()`
- `usb_init()`：调用 `bus_register(&usb_bus_type)` 注册总线
- `usb_register_driver()`：设置 `driver.bus = &usb_bus_type`，调用 `driver_register()`
- `usb_deregister()`：调用 `driver_unregister()`
- `usb_new_device()`：分配并初始化 `usb_device`，调用 `device_register()`
- 辅助函数：`usb_match_id()` 遍历驱动 id_table 匹配设备

---

## Task 4：创建 `includes/usb/hcd.h` 和 `kernel/usb/hcd.c`

参考 `drivers/usb/core/hcd.h`、`drivers/usb/core/hcd.c`：

**hcd.h：**
- `struct hc_driver`：HCD 驱动回调（start、stop、reset、urb_enqueue、get_frame_number）
- `struct usb_hcd`：内嵌 `struct usb_bus self`、`const struct hc_driver *driver`、regs/io_base、irq、root_hub 指针、状态标志

**hcd.c：**
- `usb_create_hcd()`：分配 `usb_hcd` + 关联 `usb_bus`
- `usb_add_hcd()`：初始化 HCD 硬件（调用 `driver->start`）、创建 root hub `usb_device`、注册到 `usb_bus_type`
- `usb_remove_hcd()`：停止控制器、注销 root hub

---

## Task 5：创建 `includes/usb/uhci.h` 和 `kernel/usb/uhci-hcd.c`

参考 `drivers/usb/host/uhci-hcd.h`、`drivers/usb/host/uhci-hcd.c`：

**uhci.h：**
- UHCI I/O 寄存器偏移：`USBCMD`、`USBSTS`、`USBINTR`、`USBFRNUM`、`USBFLBASEADD`、`USBPORTSC1/2`
- 命令/状态位：`USBCMD_RS`、`USBCMD_HCRESET`、`USBSTS_HCH`、`USBPORTSC_CCS` 等
- `struct uhci_hcd`：私有数据（io_base、frame_list、qh_pool 等）
- TD/QH 结构体定义（简化版：`struct uhci_td`、`struct uhci_qh`）

**uhci-hcd.c：**
- 声明 `struct pci_driver uhci_pci_driver`，id_table 匹配 `class=0x0C0300`（USB UHCI 控制器）
- `uhci_pci_probe()`：
  1. 从 PCI BAR4 获取 I/O 基址
  2. 分配 `uhci_hcd` 私有数据
  3. 调用 `usb_create_hcd()` + `usb_add_hcd()` 注册 HCD
- `uhci_pci_remove()`：调用 `usb_remove_hcd()` 清理
- `uhci_start()`：初始化 Frame List、启动控制器（USBCMD_RS=1）
- `uhci_stop()`：停止控制器、释放资源
- `uhci_init()`：注册 PCI 驱动，供 `kernel.c` 调用

---

## Task 6：集成到内核

**kernel.c：**
- 在 `pci_init()` 之后调用 `usb_init()`（注册 `usb_bus_type`）
- 随后调用 `uhci_init()`（注册 UHCI PCI 驱动，触发 probe 发现控制器）

**Makefile：**
- `kernel/usb/*.c` 通过现有 `find -name "*.[cS]"` 自动扫描，无需手动添加（Makefile 已用 `$(shell find ...)` 自动发现所有源文件）

---

## 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 总线注册方式 | 复用 `bus_type` 统一设备模型 | 与 PCI/Platform 保持一致 |
| HCD 发现方式 | UHCI PCI 驱动（class match） | x86 QEMU/Bochs 均模拟 UHCI |
| 根 Hub 创建 | HCD 注册时静态创建 | 简化实现，无需真实 Hub 枚举 |
| 设备匹配策略 | `usb_device_id` 表（vendor+product 或 class） | 与 Linux 驱动模型一致 |
| URB 支持 | 暂不实现完整 URB | 先搭框架，后续按需补充 |
