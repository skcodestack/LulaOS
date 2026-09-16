# USB 枚举流程实现

## 现状与目标

当前状态：
- `usb_bus_type` / `usb_match_id()` / `usb_new_device()` 已就绪
- UHCI 控制器启动（帧列表 + 调度器运行）已完成
- `hc_driver->hub_control` 和 `hub_status_data` 为 NULL，**无端口感知能力**
- 无控制传输通道，无法发起 GET_DESCRIPTOR / SET_ADDRESS

目标：实现从端口插拔检测到设备注册到总线的完整枚举链路。

---

## Task 1：UHCI 控制传输底层（uhci-hcd.c + uhci.h）

在 UHCI 驱动中实现控制传输的 TD/QH 构建与提交。

### 1.1 新增 UHCI 辅助函数（uhci-hcd.c）

```c
/* 从 TD 池分配一个 TD */
static struct uhci_td *uhci_alloc_td(struct uhci_hcd *uhci);

/* 释放 TD 回池（简化：清零标记位） */
static void uhci_free_td(struct uhci_hcd *uhci, struct uhci_td *td);

/* 从 QH 池分配一个 QH */
static struct uhci_qh *uhci_alloc_qh(struct uhci_hcd *uhci);

/* 构建控制传输 TD 链：SETUP → DATA(IN/OUT) → STATUS
 * 返回链头 TD 指针，QH 的 element 指向它 */
static int uhci_build_control_urb(
    struct uhci_hcd *uhci,
    unsigned int devaddr,       /* 设备地址（0=默认地址） */
    unsigned int maxpacket,     /* EP0 最大包长 */
    struct usb_ctrlrequest *setup,
    void *data,                 /* 数据缓冲区（可为 NULL） */
    unsigned int datalen,
    struct uhci_qh *qh);

/* 将 QH 插入帧列表并等待完成（轮询 TD Active 位） */
static int uhci_submit_and_wait(struct uhci_hcd *uhci, struct uhci_qh *qh,
                                 unsigned int timeout_ms);
```

**TD 链结构（控制传输，以 IN 为例）：**
```
[SETUP TD] → [DATA IN TD] → [STATUS OUT TD] → TERMINATE
```
- SETUP TD：PID=SETUP，len=8，指向 `usb_ctrlrequest`
- DATA TD：PID=IN/OUT，按 maxpacket 拆分（若 datalen=0 则跳过）
- STATUS TD：PID 与 DATA 相反，len=0，IOC 位置位

**提交方式**：将 QH 链入帧列表的某个空位（帧列表每项初始为 TERMINATE，改为指向 QH 物理地址 | UHCI_FL_QH），然后循环读取 QH element 指向的 TD 的 Active 位，超时则报错。

### 1.2 uhci.h 新增字段

```c
struct uhci_hcd {
    ...
    unsigned int td_alloc_idx;   /* TD 池轮询分配下标 */
    unsigned int qh_alloc_idx;   /* QH 池轮询分配下标 */
};
```

### 1.3 实现 hub_control 回调（uhci-hcd.c）

```c
static int uhci_hub_control(struct usb_hcd *hcd,
                             unsigned int type,      /* bmRequestType */
                             unsigned int request,   /* bRequest */
                             unsigned int value,
                             unsigned int index,     /* 端口号（1-based） */
                             char *buf,
                             unsigned int len);
```

支持的请求（Hub Class Request）：
- `GetPortStatus`（request=0x00）：读 USBPORTSC1/2，填充 4 字节状态
- `SetPortReset`（request=0x04，value=PORT_RESET）：置 PR 位，等待复位完成
- `SetPortEnable`（request=0x04，value=PORT_ENABLE）：置 PE 位
- `ClearPortFeature`（request=0x01）：清除 CSC/PEDC 等变化位

### 1.4 实现 hub_status_data 回调（uhci-hcd.c）

```c
static int uhci_hub_status_data(struct usb_hcd *hcd, char *buf);
```
读取 USBPORTSC1/2 的 CSC（Connection Status Change）位，填充 hub 状态变化字节，供枚举状态机轮询。

### 1.5 将回调注册到 uhci_hc_driver

```c
static const struct hc_driver uhci_hc_driver = {
    .description     = "uhci_hcd",
    .start           = uhci_start,
    .stop            = uhci_stop,
    .hub_status_data = uhci_hub_status_data,
    .hub_control     = uhci_hub_control,
    ...
};
```

---

## Task 2：USB Core 控制传输接口（usb.c + usb.h）

### 2.1 usb_control_msg()（新增到 usb.c）

```c
int usb_control_msg(struct usb_device *dev,
                    unsigned int pipe,      /* 方向信息 */
                    unsigned char request,
                    unsigned char requesttype,
                    unsigned short value,
                    unsigned short index,
                    void *data,
                    unsigned short size,
                    unsigned int timeout);
```

流程：
1. 从 `dev->bus->root_hub` 获取 `usb_hcd` 指针（通过 `container_of`）
2. 构建 `usb_ctrlrequest`
3. 调用 `hcd->driver->urb_enqueue()` 或直接新增 `submit_control()` 回调
4. 等待完成，返回实际传输字节数

**需要在 `hc_driver` 新增回调**：

```c
/* hcd.h */
struct hc_driver {
    ...
    int (*submit_control)(struct usb_hcd *hcd,
                          unsigned int devaddr,
                          unsigned int maxpacket,
                          struct usb_ctrlrequest *setup,
                          void *data,
                          unsigned int datalen);
};
```

UHCI 侧实现即为 Task 1 中的 TD 链构建 + 提交逻辑的封装。

### 2.2 usb_alloc_dev()（新增到 usb.c）

```c
struct usb_device *usb_alloc_dev(struct usb_device *parent,
                                  struct usb_bus *bus,
                                  unsigned int port);
```
分配并初始化 `usb_device`，设置 parent/bus/portnum/speed，设备名格式 `{parent_name}.{port}`（如 `usb1.2`）。

---

## Task 3：USB Hub 枚举状态机（新增 kernel/usb/hub.c + includes/usb/hub.h）

### 3.1 新增头文件 includes/usb/hub.h

```c
/* Hub Class 请求常量 */
#define USB_RT_HUB          (USB_TYPE_CLASS | USB_RECIP_DEVICE)
#define USB_RT_PORT         (USB_TYPE_CLASS | USB_RECIP_OTHER)

#define USB_PORT_FEAT_RESET         4
#define USB_PORT_FEAT_ENABLE        1
#define USB_PORT_FEAT_CONNECTION    0
#define USB_PORT_FEAT_C_CONNECTION  16

/* Hub 端口状态位（GetPortStatus 返回的 4 字节） */
#define USB_PORT_STAT_CONNECTION    0x0001
#define USB_PORT_STAT_ENABLE        0x0002
#define USB_PORT_STAT_RESET         0x0010
#define USB_PORT_STAT_LOW_SPEED     0x0200
#define USB_PORT_STAT_C_CONNECTION  0x0001  /* status change word */

void usb_hub_init(void);
void usb_hub_events(void);   /* 轮询调用，扫描所有 HCD 的 Root Hub 端口 */
```

### 3.2 kernel/usb/hub.c 实现

核心函数：

```c
/* 枚举单个端口上的设备 */
static int usb_enumerate_device(struct usb_device *parent,
                                 unsigned int port);
```

流程：
1. `hub_control(GetPortStatus)` → 检测 CSC
2. 清除 CSC（`ClearPortFeature(C_CONNECTION)`）
3. `hub_control(SetPortReset)` → 端口复位
4. 等待复位完成（PORT_STAT_RESET 清零）
5. 读端口状态，判断速度（LOW_SPEED / FULL）
6. **GET_DESCRIPTOR（设备描述符前 8 字节）**，获取 `bMaxPacketSize0`
   - devaddr=0，maxpacket=8（初始假设）
7. **SET_ADDRESS**，分配新地址（全局 `usb_devnum++`，范围 1~127）
8. **GET_DESCRIPTOR（完整设备描述符）**，devaddr=新地址
9. **GET_DESCRIPTOR（配置描述符 + 接口描述符）**，解析 `wTotalLength`
10. 调用 `usb_alloc_dev()` 创建设备
11. 填充 `usb_device` 描述符字段
12. `usb_new_device()` → 注册到总线，触发匹配

```c
/* 扫描所有已注册 HCD 的 Root Hub 端口 */
void usb_hub_events(void)
{
    /* 遍历全局 usb_bus 链表 */
    /* 对每个 Root Hub 调用 hub_status_data */
    /* 若 CSC 位置位，调用 usb_enumerate_device() */
}
```

### 3.3 注册 Hub "驱动"

Hub 本身作为 `usb_driver` 注册，匹配 `bDeviceClass=USB_CLASS_HUB`：

```c
static const struct usb_device_id hub_id_table[] = {
    { .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
      .bDeviceClass = USB_CLASS_HUB },
    { }
};

static struct usb_driver hub_driver = {
    .driver    = { .name = "hub" },
    .id_table  = hub_id_table,
    .probe     = hub_probe,      /* 启动端口扫描 */
    .disconnect = hub_disconnect,
};
```

`hub_probe()` 被调用时（Root Hub 匹配成功），触发初始端口扫描。

---

## Task 4：全局总线链表（usb.c + hcd.h）

当前 `usb_bus` 有 `bus_list` 字段但未被使用。需要：

1. 定义全局链表头：
   ```c
   /* usb.c */
   static LIST_HEAD(usb_bus_list);
   ```
2. `usb_add_hcd()` 中将 `hcd->self.bus_list` 链入 `usb_bus_list`
3. 导出访问接口供 `hub.c` 遍历：
   ```c
   struct list_head *usb_get_bus_list(void);
   ```

---

## Task 5：Makefile + kernel.c 接入

### 5.1 kernel.c 新增调用

```c
usb_init();        /* 已有 */
uhci_init();       /* 已有 */
usb_hub_init();    /* 新增：注册 hub_driver，触发 Root Hub 匹配和端口扫描 */
```

### 5.2 Makefile 无需改动

Makefile 使用 `$(shell find -name "*.[cS]")` 自动发现新 `.c` 文件，新增 `kernel/usb/hub.c` 后自动编译。

### 5.3 QEMU 启动参数（config/make-debug-tool 或手动）

当前 `run-qemu` 无 `-usb` 参数。需在 Makefile 的 `run-qemu` 目标中添加：
```
-usb -device usb-mouse -device usb-kbd
```
使 QEMU 的 PIIX3 UHCI 控制器下挂有可枚举设备。

---

## 文件变更汇总

| 文件 | 变更类型 |
|---|---|
| `includes/usb/uhci.h` | 新增 `td_alloc_idx`/`qh_alloc_idx` 字段 |
| `includes/usb/hcd.h` | 新增 `submit_control` 回调 |
| `includes/usb/usb.h` | 新增 `usb_control_msg()` / `usb_alloc_dev()` 声明 |
| `includes/usb/hub.h` | **新建**，Hub 常量与函数声明 |
| `kernel/usb/uhci-hcd.c` | 新增 TD/QH 分配、控制传输、hub_control/hub_status_data |
| `kernel/usb/usb.c` | 新增 `usb_control_msg()` / `usb_alloc_dev()` / 全局总线链表 |
| `kernel/usb/hcd.c` | `usb_add_hcd()` 中链入全局总线链表 |
| `kernel/usb/hub.c` | **新建**，枚举状态机 + hub_driver |
| `kernel/kernel.c` | 新增 `usb_hub_init()` 调用 |
| `Makefile` | `run-qemu` 添加 `-usb` 参数 |
