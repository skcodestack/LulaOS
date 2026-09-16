# USB Hub 轮询与控制传输

## 现状分析

当前框架已就位：
- `usb_bus_type` 总线已注册，match/probe 机制可用
- UHCI 控制器已启动（RS=1），帧列表已初始化（全 TERMINATE）
- TD/QH 池已预分配但未使用
- Root Hub（`bDeviceClass=USB_CLASS_HUB`）已注册到总线，但无驱动匹配
- `hc_driver` 中 `urb_enqueue`/`hub_control` 均为 NULL
- `udelay()` 可用于延迟（`includes/arch/x86/apic.h`）

---

## Task 1：扩展头文件

### 1a. `includes/usb/usb_ch9.h` - 增加 Hub 类请求常量

在文件末尾 `#endif` 前添加：

```c
/* Hub 类特征请求（wValue 高字节） */
#define USB_PORT_FEAT_CONNECTION     0
#define USB_PORT_FEAT_ENABLE         1
#define USB_PORT_FEAT_SUSPEND        2
#define USB_PORT_FEAT_OVER_CURRENT   3
#define USB_PORT_FEAT_RESET          4
#define USB_PORT_FEAT_POWER          8

/* Hub 描述符类型 */
#define USB_DT_HUB                   0x29
```

### 1b. `includes/usb/usb.h` - 新增 API 声明

在 `/* 公共 API */` 区域追加：

```c
/* 同步控制传输（阻塞等待完成） */
int usb_control_msg(struct usb_device *dev, unsigned int pipe,
                    unsigned char request, unsigned char requesttype,
                    unsigned short value, unsigned short index,
                    void *data, unsigned short size,
                    unsigned int timeout);
```

### 1c. `includes/usb/hcd.h` - 增加辅助宏

在数据结构区域后添加：

```c
/* 从 usb_bus * 获取包含它的 usb_hcd * */
#define usb_bus_to_hcd(bus) container_of(bus, struct usb_hcd, self)
```

并在 `struct usb_bus` 中增加 `devnum_next` 字段（用于地址分配）：
```c
int devnum_next;    /* 下一个可用设备地址（2~127） */
```

### 1d. `includes/usb/uhci.h` - 新增 TD token 编码宏和传输 API

在数据结构区域后添加：

```c
/* TD Token 编码（bit 布局） */
#define uhci_token_pid(pid)         ((unsigned int)(pid) & 0xFF)
#define uhci_token_devaddr(addr)    (((unsigned int)(addr) & 0x7F) << 8)
#define uhci_token_endpoint(ep)     (((unsigned int)(ep) & 0xF) << 15)
#define uhci_token_toggle(t)        (((unsigned int)(t) & 0x1) << 19)
#define uhci_token_maxlen(len)      (((unsigned int)((len) - 1) & 0x7FF) << 21)

/* TD Control/Status 编码 */
#define TD_CTRL_LS          (1 << 26)   /* Low Speed */
#define TD_CTRL_IOC         (1 << 24)   /* Interrupt on Complete */
#define TD_CTRL_C_ERR_MASK  (3 << 27)   /* Error count field */
#define TD_CTRL_C_ERR_SHIFT 27
#define uhci_ctrl_errcnt(n) (((unsigned int)(n) & 0x3) << 27)

/* 控制传输 API */
int uhci_control_msg(struct usb_hcd *hcd, struct usb_device *dev,
                     unsigned char request, unsigned char requesttype,
                     unsigned short value, unsigned short index,
                     void *data, unsigned short size);
```

---

## Task 2：UHCI 控制传输引擎（`kernel/usb/uhci-hcd.c`）

这是核心实现，约 200 行新增代码。

### 2a. TD/QH 池分配器

在现有 `uhci_memzero()` 后添加简单的线性扫描分配：

```c
static struct uhci_td *uhci_alloc_td(struct uhci_hcd *uhci);
static void uhci_free_td(struct uhci_hcd *uhci, struct uhci_td *td);
static struct uhci_qh *uhci_alloc_qh(struct uhci_hcd *uhci);
static void uhci_free_qh(struct uhci_hcd *uhci, struct uhci_qh *qh);
```

通过检查 `td->link == 0`（未使用）来判定空闲。分配时标记 `link = 0xDEAD`（已分配哨兵）。

### 2b. 构建控制传输 TD 链

`uhci_build_control()` 函数：

```
输入: dev, setup_packet(8 bytes), data_buf, data_len, is_in
输出: (qh, first_td)

1. 分配 QH
2. 分配 SETUP TD:
   - cs_status = TD_CTRL_ACTIVE | uhci_ctrl_errcnt(3)
   - token = uhci_token_pid(SETUP) | devaddr(0或devnum) | endpoint(0)
           | toggle(0) | maxlen(8)
   - buffer = __pa(setup_buf)
3. 若有数据阶段，分配 DATA TD:
   - toggle = 1（DATA1）
   - PID = IN 或 OUT
   - maxlen = data_len（USB 1.1 单次最大 64 字节）
4. 分配 STATUS TD:
   - toggle = 1（DATA1）
   - PID = 与数据阶段相反方向
   - maxlen = 0（零长度）
   - cs_status 额外设 IOC
5. 链接: setup_td -> data_td -> status_td -> TERMINATE
6. qh->element = __pa(first_td)
```

### 2c. 提交并轮询等待

`uhci_submit_and_wait()` 函数：

```
1. 将 QH 插入所有帧列表项:
   for (i=0; i<1024; i++)
       frame_list[i] = __pa(qh) | UHCI_FL_QH
2. udelay(100)  -- 至少等 1 帧
3. 轮询直到 last_td->cs_status 的 TD_CTRL_ACTIVE 清零
   或超时（最多 1000 次 * udelay(100) = 100ms）
4. 从帧列表移除 QH:
   for (i=0; i<1024; i++)
       frame_list[i] = UHCI_FL_TERMINATE
5. 检查错误状态位，返回传输字节数
```

### 2d. 完整控制传输入口

```c
int uhci_control_msg(struct usb_hcd *hcd, struct usb_device *dev,
                     unsigned char request, unsigned char requesttype,
                     unsigned short value, unsigned short index,
                     void *data, unsigned short size)
{
    struct uhci_hcd *uhci = hcd->hcd_priv;
    // 1. 构造 8 字节 setup packet
    // 2. 调用 uhci_build_control()
    // 3. 调用 uhci_submit_and_wait()
    // 4. 释放 TD/QH
    // 5. 返回传输字节数或错误码
}
```

### 2e. 注册到 hc_driver

```c
static const struct hc_driver uhci_hc_driver = {
    ...
    .hub_control = uhci_hub_control,   // 新增
};
```

---

## Task 3：UHCI Hub 控制回调（`kernel/usb/uhci-hcd.c`）

实现 `uhci_hub_control()` 处理两个 Hub 类请求：

```c
static int uhci_hub_control(struct usb_hcd *hcd, unsigned int type,
                            unsigned int request, unsigned int value,
                            unsigned int index, char *buf, unsigned int len)
{
    // 端口号 = index (1-based)，对应 USBPORTSC1/USBPORTSC2
    unsigned short portsc_reg = UHCI_USBPORTSC1 + (index - 1) * 2;
    unsigned short portsc = uhci_read16(uhci, portsc_reg);

    switch (request) {
    case USB_REQ_GET_STATUS:    // 返回 4 字节端口状态
        // 将 UHCI portsc 位映射到标准 Hub 端口状态位
        // buf[0:1] = port status, buf[2:3] = port change
        break;
    case USB_REQ_SET_FEATURE:   // value = feature selector
        if (value == USB_PORT_FEAT_RESET) {
            // 1. 写 PR=1，udelay(50000)
            // 2. 写 PR=0，udelay(10000)
            // 3. 检查 PE 和 CCS
        }
        break;
    case USB_REQ_CLEAR_FEATURE: // 清除特征（如 CSC）
        break;
    }
}
```

---

## Task 4：usb_control_msg() 实现（`kernel/usb/hcd.c`）

```c
int usb_control_msg(struct usb_device *dev, unsigned int pipe,
                    unsigned char request, unsigned char requesttype,
                    unsigned short value, unsigned short index,
                    void *data, unsigned short size,
                    unsigned int timeout)
{
    struct usb_hcd *hcd = usb_bus_to_hcd(dev->bus);
    // 调用 hcd->driver 的对应方法
    // 对于 UHCI: 直接调用 uhci_control_msg(hcd, dev, ...)
    return hcd->driver->urb_enqueue  // 或新增 control_msg 回调
}
```

实际操作：在 `hc_driver` 结构中添加 `control_msg` 回调指针，`usb_control_msg()` 通过此指针调用 UHCI 实现。

**修改 `hcd.h`：**

```c
struct hc_driver {
    ...
    /* 同步控制传输 */
    int (*control_msg)(struct usb_hcd *hcd, struct usb_device *dev,
                       unsigned char request, unsigned char requesttype,
                       unsigned short value, unsigned short index,
                       void *data, unsigned short size);
};
```

---

## Task 5：Hub 驱动（新建 `kernel/usb/hub.c`）

约 250 行新文件。

### 5a. 驱动定义

```c
static const struct usb_device_id hub_id_table[] = {
    { .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
      .bDeviceClass = USB_CLASS_HUB },
    { }
};

static struct usb_driver hub_driver = {
    .driver     = { .name = "hub" },
    .id_table   = hub_id_table,
    .probe      = hub_probe,
    .disconnect = hub_disconnect,
};
```

### 5b. hub_probe() - 触发端口扫描

```c
static int hub_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    // 通过全局链表找到 root hub 对应的 hcd
    // 调用 hub_scan_ports(hcd, root_hub)
}
```

由于简化实现中 probe 接收 `usb_interface=NULL`，改为在 `usb_bus_probe` 中将 `usb_device*` 传给 probe，或在 hub 驱动中直接遍历 `usb_bus_type.devices` 找到 Root Hub。

**更简洁方案**：在 `usb_bus_probe()` 中，当 `dev` 是 Root Hub 时，通过 `usb_bus_to_hcd(dev->bus)` 获取 hcd 并传入。或者直接修改 `usb_bus_probe` 传入 `usb_device*` 而非 `usb_interface*`：

```c
// usb.c 中修改 probe 回调签名
static int usb_bus_probe(struct device *dev) {
    struct usb_device *udev = to_usb_device(dev);
    if (udrv->probe)
        return udrv->probe(udev, id);  // 传 usb_device 而非 usb_interface
}
```

对应修改 `usb_driver.probe` 签名为 `int (*probe)(struct usb_device *, const struct usb_device_id *)`。

### 5c. hub_scan_ports() - 扫描所有端口

```c
static void hub_scan_ports(struct usb_hcd *hcd, struct usb_device *hub_dev)
{
    int i;
    for (i = 1; i <= 2; i++) {   // UHCI 2 端口
        char status[4];
        hcd->driver->hub_control(hcd, USB_DIR_IN | USB_TYPE_CLASS,
                                 USB_REQ_GET_STATUS, 0, i, status, 4);
        if (status[0] & 0x01) {   // CCS = 1
            hub_port_reset(hcd, i);
            usb_enumerate_device(hcd, hub_dev, i);
        }
    }
}
```

### 5d. hub_port_reset() - 端口复位

```c
static void hub_port_reset(struct usb_hcd *hcd, int port)
{
    hcd->driver->hub_control(hcd, USB_TYPE_CLASS,
                             USB_REQ_SET_FEATURE, USB_PORT_FEAT_RESET,
                             port, NULL, 0);
    udelay(50000);   // 50ms 复位
}
```

### 5e. usb_enumerate_device() - 设备枚举

```c
static struct usb_device *usb_enumerate_device(
    struct usb_hcd *hcd, struct usb_device *parent, int port)
{
    struct usb_device *dev = kmalloc(sizeof(*dev), GFP_KERNEL);
    int devnum;

    // 1. 分配地址
    devnum = hcd->self.devnum_next++;

    // 2. GET_DESCRIPTOR (addr=0): 读取前 8 字节获取 bMaxPacketSize0
    usb_control_msg(dev, ..., USB_REQ_GET_DESCRIPTOR,
                    USB_DT_DEVICE << 8, 0, buf, 8, 0);

    // 3. SET_ADDRESS
    usb_control_msg(dev, ..., USB_REQ_SET_ADDRESS, devnum, 0, NULL, 0, 0);
    dev->devnum = devnum;

    // 4. GET_DESCRIPTOR (addr=devnum): 完整设备描述符
    usb_control_msg(dev, ..., USB_REQ_GET_DESCRIPTOR,
                    USB_DT_DEVICE << 8, 0, &dev->descriptor, 18, 0);

    // 5. GET_DESCRIPTOR: 配置描述符
    usb_control_msg(dev, ..., USB_REQ_GET_DESCRIPTOR,
                    USB_DT_CONFIG << 8, 0, config_buf, total_len, 0);

    // 6. SET_CONFIGURATION
    usb_control_msg(dev, ..., USB_REQ_SET_CONFIGURATION,
                    config_value, 0, NULL, 0, 0);

    // 7. 填充 usb_device 字段
    dev->speed = (portsc & USBPORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->parent = parent;
    dev->bus = &hcd->self;
    dev->portnum = port;

    // 8. 注册到总线
    usb_new_device(dev);

    // 9. 加入 parent->child[]
    parent->child[port - 1] = dev;
    parent->children++;

    printk("USB: new %s speed device at address %d\n",
           dev->speed == USB_SPEED_LOW ? "low" : "full", devnum);
    return dev;
}
```

### 5f. hub_init() - 注册 Hub 驱动

```c
void hub_init(void) {
    usb_register_driver(&hub_driver);
}
```

---

## Task 6：集成到内核

### 修改 `kernel/kernel.c`

在 `uhci_init()` 之后添加：

```c
#include <usb/hub.h>
...
    uhci_init();
    hub_init();       // 注册 Hub 驱动，触发 Root Hub 匹配和端口扫描
```

### 新增 `includes/usb/hub.h`

```c
#ifndef __HUB_H__
#define __HUB_H__
void hub_init(void);
#endif
```

---

## 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 传输模式 | 同步轮询（poll TD active bit） | 无需中断处理，实现简单 |
| 帧列表策略 | 传输时临时写入所有 1024 项，完成后恢复 TERMINATE | 简单可靠，单传输无并发 |
| 地址分配 | `usb_bus.devnum_next` 线性递增 | 简化实现，足够用 |
| Hub 驱动匹配 | 修改 `usb_driver.probe` 签名接受 `usb_device*` | 简化实现，无需 interface 层 |
| 延迟 | 复用 `udelay()` | 已有 PIT 校准的精确微秒延迟 |

---

## 文件变更汇总

| 文件 | 操作 | 行数（估） |
|------|------|-----------|
| `includes/usb/usb_ch9.h` | 修改 | +10 |
| `includes/usb/usb.h` | 修改 | +8 |
| `includes/usb/hcd.h` | 修改 | +15 |
| `includes/usb/uhci.h` | 修改 | +25 |
| `includes/usb/hub.h` | **新建** | ~10 |
| `kernel/usb/uhci-hcd.c` | 修改 | +220 |
| `kernel/usb/hcd.c` | 修改 | +20 |
| `kernel/usb/usb.c` | 修改 | +5 |
| `kernel/usb/hub.c` | **新建** | ~250 |
| `kernel/kernel.c` | 修改 | +3 |
