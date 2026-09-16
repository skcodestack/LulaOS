# 统一设备模型 + PCI 重构 + Platform 总线

## 设计思路

参考 Linux 2.6.20 的 Device Model，核心思想：
- `bus_type` 定义匹配策略（每种总线不同）
- `device` / `device_driver` 是基础抽象，各总线扩展
- 核心代码处理注册 + 匹配 + probe 调用，不关心总线细节

```
            bus_type (match 策略)
           /          \
    PCI bus_type    Platform bus_type
    (vendor/device)  (name 匹配)
         |                |
    pci_dev           platform_device
    pci_driver        platform_driver
```

---

## Task 1: 创建 `includes/device/device.h` -- 统一设备模型头文件

**文件**: `includes/device/device.h`（新建）

核心数据结构：

```c
#define DEVICE_NAME_SIZE  32

/* bus_type - 总线类型抽象 */
struct bus_type {
    const char *name;
    struct list_head list;          /* 链入全局 bus_list */
    struct list_head devices;       /* 该总线上的设备链表 */
    struct list_head drivers;       /* 该总线上的驱动链表 */
    int (*match)(struct device *dev, struct device_driver *drv);
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
};

/* device - 设备基础结构（各总线扩展） */
struct device {
    char name[DEVICE_NAME_SIZE];
    struct bus_type *bus;           /* 所属总线 */
    struct list_head bus_node;      /* 链入 bus->devices */
    void *driver_data;              /* 驱动私有数据 */
};

/* device_driver - 驱动基础结构（各总线扩展） */
struct device_driver {
    const char *name;
    struct bus_type *bus;           /* 所属总线 */
    struct list_head bus_node;      /* 链入 bus->drivers */
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
};
```

核心 API：
```c
int  bus_register(struct bus_type *bus);
int  device_register(struct device *dev);     /* 注册设备并触发匹配 */
int  driver_register(struct device_driver *drv); /* 注册驱动并触发匹配 */
void device_unregister(struct device *dev);
void driver_unregister(struct device_driver *drv);
```

---

## Task 2: 创建 `kernel/device/device.c` -- 统一设备模型核心实现

**文件**: `kernel/device/device.c`（新建）

核心逻辑：

```c
static LIST_HEAD(bus_list);  /* 全局总线链表 */

int bus_register(struct bus_type *bus) {
    INIT_LIST_HEAD(&bus->devices);
    INIT_LIST_HEAD(&bus->drivers);
    list_add_tail(&bus->list, &bus_list);
    printk("bus: '%s' registered\n", bus->name);
    return 0;
}

/* 注册设备时，遍历该总线上所有已注册驱动尝试匹配 */
int device_register(struct device *dev) {
    struct list_head *pos;
    list_add_tail(&dev->bus_node, &dev->bus->devices);

    list_for_each(pos, &dev->bus->drivers) {
        struct device_driver *drv = list_entry(pos, struct device_driver, bus_node);
        if (dev->bus->match(dev, drv)) {
            if (dev->bus->probe)
                dev->bus->probe(dev);
            break;
        }
    }
    return 0;
}

/* 注册驱动时，遍历该总线上所有已注册设备尝试匹配 */
int driver_register(struct device_driver *drv) {
    struct list_head *pos;
    list_add_tail(&drv->bus_node, &drv->bus->drivers);

    list_for_each(pos, &drv->bus->devices) {
        struct device *dev = list_entry(pos, struct device, bus_node);
        if (drv->bus->match(dev, drv)) {
            if (drv->bus->probe)
                drv->bus->probe(dev);
        }
    }
    return 0;
}
```

---

## Task 3: 创建 `includes/device/platform.h` -- Platform 总线头文件

**文件**: `includes/device/platform.h`（新建）

```c
#define PLATFORM_NAME_SIZE  32
#define PLATFORM_MAX_RESOURCES 6

struct platform_resource {
    unsigned long start;
    unsigned long end;
    unsigned long flags;     /* IORESOURCE_MEM / IORESOURCE_IO / IORESOURCE_IRQ */
};

#define IORESOURCE_IRQ   0x00000400

struct platform_device {
    struct device dev;       /* 嵌入基础 device */
    int id;                  /* 设备实例 ID（-1 = 唯一） */
    int num_resources;
    struct platform_resource resource[PLATFORM_MAX_RESOURCES];
};

struct platform_driver {
    struct device_driver driver;  /* 嵌入基础 driver */
    int (*probe)(struct platform_device *pdev);
    int (*remove)(struct platform_device *pdev);
};

extern struct bus_type platform_bus_type;

int platform_device_register(struct platform_device *pdev);
int platform_driver_register(struct platform_driver *pdrv);
struct platform_device *platform_find_device(const char *name);
```

---

## Task 4: 创建 `kernel/device/platform.c` -- Platform 总线实现

**文件**: `kernel/device/platform.c`（新建）

关键实现：

```c
/* Platform 匹配：用 name 字符串比较 */
static int platform_match(struct device *dev, struct device_driver *drv) {
    struct platform_device *pdev = container_of(dev, struct platform_device, dev);
    return strcmp(pdev->dev.name, drv->name) == 0;
}

/* Platform probe 转发 */
static int platform_probe(struct device *dev) {
    struct platform_driver *pdrv = container_of(dev->driver_data,
                                                 struct platform_driver, driver);
    struct platform_device *pdev = container_of(dev, struct platform_device, dev);
    if (pdrv->probe)
        return pdrv->probe(pdev);
    return 0;
}

struct bus_type platform_bus_type = {
    .name   = "platform",
    .match  = platform_match,
    .probe  = platform_probe,
};
```

---

## Task 5: 重构 `includes/pci/pci.h` -- PCI 使用统一设备模型

**文件**: `includes/pci/pci.h`

主要改动：

1. `pci_dev` 嵌入 `struct device`（替换独立 `list_head`）：
```c
struct pci_dev {
    struct device dev;              /* 嵌入统一设备模型 */
    unsigned char   bus;
    unsigned char   devfn;
    unsigned short  vendor;
    unsigned short  device;
    /* ... 其余字段不变 ... */
};
```

2. `pci_driver` 嵌入 `struct device_driver`（替换独立 `list_head`）：
```c
struct pci_driver {
    struct device_driver driver;        /* 嵌入统一驱动模型 */
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
};
```

3. 新增：
```c
extern struct bus_type pci_bus_type;
```

4. 新增辅助宏：
```c
#define to_pci_dev(d)   container_of(d, struct pci_dev, dev)
#define to_pci_driver(d) container_of(d, struct pci_driver, driver)
```

---

## Task 6: 重构 `kernel/pci/pci.c` -- PCI 基于统一模型

**文件**: `kernel/pci/pci.c`

主要改动：

1. **PCI bus_type 实现**：
```c
static int pci_bus_match(struct device *dev, struct device_driver *drv) {
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = to_pci_driver(drv);
    return pci_match_device(pdrv->id_table, pdev) != NULL;
}

static int pci_bus_probe(struct device *dev) {
    struct pci_dev *pdev = to_pci_dev(dev);
    struct pci_driver *pdrv = to_pci_driver(dev->driver_data);
    const struct pci_device_id *id = pci_match_device(pdrv->id_table, pdev);
    if (pdrv->probe)
        return pdrv->probe(pdev, id);
    return 0;
}

struct bus_type pci_bus_type = {
    .name   = "pci",
    .match  = pci_bus_match,
    .probe  = pci_bus_probe,
};
```

2. **扫描设备时使用 `device_register()`**：
```c
// pci_scan_device() 末尾：
dev->dev.bus = &pci_bus_type;
snprintf(dev->dev.name, DEVICE_NAME_SIZE, "%02x:%02x.%x",
         bus, devfn >> 3, devfn & 7);
device_register(&dev->dev);  // 替代 list_add_tail
```

3. **`pci_register_driver()` 改为调用 `driver_register()`**：
```c
int pci_register_driver(struct pci_driver *drv) {
    drv->driver.bus = &pci_bus_type;
    return driver_register(&drv->driver);
}
```

4. **删除旧的全局链表和手动遍历代码**（`pci_devices`、`pci_drivers_list`）

5. **`pci_find_device()` 改为遍历 `pci_bus_type.devices`**

---

## Task 7: 更新 `kernel/kernel.c` 初始化顺序

**文件**: `kernel/kernel.c`

```c
#include <device/device.h>
#include <device/platform.h>

// _kernel_main() 中，kmem_cache_init() 之后：

/* 注册 Platform 总线 */
bus_register(&platform_bus_type);

/* PCI 总线初始化（内部注册 pci_bus_type + 扫描设备） */
pci_init();
```

---

## Task 8: 将键盘和鼠标重构为 Platform 设备

**文件**: `kernel/keyboard.c`, `kernel/mouse.c`, `kernel/kernel.c`

键盘和鼠标属于 PS/2 控制器上的设备，不是 PCI 设备，应挂在 Platform 总线上。

### 8.1 `kernel/keyboard.c` 重构

原 `keyboard_init()` 直接操作固定端口和中断向量。重构后分为：

```c
// 1. 声明 Platform 设备
static struct platform_resource kbd_resources[] = {
    { .start = 0x60, .end = 0x64, .flags = IORESOURCE_IO },
    { .start = KEYBOARD_VECTOR, .end = KEYBOARD_VECTOR, .flags = IORESOURCE_IRQ },
};

static struct platform_device keyboard_device = {
    .dev.name = "ps2-keyboard",
    .id = -1,
    .resource = kbd_resources,
    .num_resources = 2,
};

// 2. 实现 Platform 驱动的 probe
static int keyboard_probe(struct platform_device *pdev) {
    /* 原 keyboard_init() 中除注册 request_irq 外的初始化逻辑 */
    int ret = request_irq(KEYBOARD_VECTOR, keyboard_handler, "keyboard", NULL);
    if (ret == 0)
        printk("keyboard: IRQ1 registered (vector=%#x)\n", KEYBOARD_VECTOR);
    return ret;
}

static struct platform_driver keyboard_driver = {
    .driver.name = "ps2-keyboard",
    .probe = keyboard_probe,
};

// 3. 新的键盘初始化入口
void keyboard_init(void) {
    platform_device_register(&keyboard_device);
    platform_driver_register(&keyboard_driver);
}
```

### 8.2 `kernel/mouse.c` 重构

同理：

```c
static struct platform_resource mouse_resources[] = {
    { .start = 0x60, .end = 0x64, .flags = IORESOURCE_IO },
    { .start = MOUSE_VECTOR, .end = MOUSE_VECTOR, .flags = IORESOURCE_IRQ },
};

static struct platform_device mouse_device = {
    .dev.name = "ps2-mouse",
    .id = -1,
    .resource = mouse_resources,
    .num_resources = 2,
};

static int mouse_probe(struct platform_device *pdev) {
    /* 原 mouse_init() 中除注册 request_irq 外的初始化逻辑 */
    ...
    return request_irq(MOUSE_VECTOR, mouse_handler, "mouse", NULL);
}

static struct platform_driver mouse_driver = {
    .driver.name = "ps2-mouse",
    .probe = mouse_probe,
};

void mouse_init(void) {
    platform_device_register(&mouse_device);
    platform_driver_register(&mouse_driver);
}
```

### 8.3 `kernel/kernel.c` 初始化顺序

```c
kmem_cache_init();

/* 注册总线 */
bus_register(&platform_bus_type);

/* 注册 Platform 设备 */
keyboard_init();    // 内部 register device + driver
mouse_init();       // 内部 register device + driver

/* PCI 总线初始化 */
pci_init();
```

---

## 文件变更总结

| 文件 | 操作 |
|------|------|
| `includes/device/device.h` | 新建，bus_type / device / device_driver |
| `kernel/device/device.c` | 新建，bus/device/driver 注册 + 匹配引擎 |
| `includes/device/platform.h` | 新建，platform_device / platform_driver |
| `kernel/device/platform.c` | 新建，platform_bus_type + name 匹配 |
| `includes/pci/pci.h` | 重构，pci_dev/pci_driver 嵌入统一结构 |
| `kernel/pci/pci.c` | 重构，pci_bus_type + 使用 device_register |
| `kernel/keyboard.c` | 重构，改为 Platform 设备和驱动 |
| `kernel/mouse.c` | 重构，改为 Platform 设备和驱动 |
| `kernel/kernel.c` | 添加总线注册 + 调整初始化顺序 |

---

## container_of 宏说明

所有"从基类取子类"的操作都依赖 `container_of`，该宏已存在于 Linux 风格代码中。如果项目没有，需在 `device.h` 中添加：

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))
```
