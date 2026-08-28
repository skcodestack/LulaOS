# LulaOS DRM/KMS/GEM 子系统

参考 Linux DRM 子系统实现的简化版图形驱动框架，支持 Bochs VBE 和 QEMU std-vga 虚拟显卡。

---

## 目录

- [概述](#概述)
- [核心架构](#核心架构)
- [数据结构](#数据结构)
- [启动流程](#启动流程)
- [GEM 显存管理](#gem-显存管理)
- [KMS 显示控制](#kms-显示控制)
- [Bochs/QEMU VBE 驱动](#bochsqemu-vbe-驱动)
- [ioctl 接口](#ioctl-接口)
- [文件结构](#文件结构)

---

## 概述

DRM (Direct Rendering Manager) 是内核中管理 GPU 硬件的子系统，解决三个核心问题：

1. **显存管理 (GEM)**：多程序共享显存，统一分配/释放
2. **显示控制 (KMS)**：分辨率、多显示器、双缓冲
3. **命令提交**：GPU 渲染命令队列管理（本次实现未包含）

```
┌─────────────────────────────────────────────────────────┐
│                      用户态应用                          │
│    OpenGL/Vulkan App  ──►  Mesa 3D (用户态驱动)          │
└───────────────────────────┬─────────────────────────────┘
                            │ ioctl
┌───────────────────────────▼─────────────────────────────┐
│                    DRM 核心框架                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐ │
│  │    GEM      │  │    KMS      │  │  ioctl 分发      │ │
│  │ 显存管理    │  │ 显示控制    │  │  命令路由        │ │
│  └─────────────┘  └─────────────┘  └─────────────────┘ │
└───────────────────────────┬─────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────┐
│                  Bochs/QEMU VBE 驱动                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐ │
│  │   CRTC      │  │   Plane     │  │   Connector     │ │
│  │ 显示控制器  │  │   图层      │  │   连接器        │ │
│  └─────────────┘  └─────────────┘  └─────────────────┘ │
└───────────────────────────┬─────────────────────────────┘
                            │ BGA 寄存器 (0x01CE/0x01CF)
┌───────────────────────────▼─────────────────────────────┐
│                    GPU 硬件                              │
│         QEMU std-vga / Bochs VBE 虚拟显卡               │
└─────────────────────────────────────────────────────────┘
```

---

## 核心架构

### 三层架构

```
┌─────────────────────────────────────────────────────────┐
│  第一层：DRM 核心 (drm_core.c)                          │
│  ├── 驱动注册表（全局链表管理）                          │
│  ├── ioctl 命令分发（核心 + 驱动私有）                   │
│  └── 设备生命周期（alloc/free）                          │
├─────────────────────────────────────────────────────────┤
│  第二层：子系统 (drm_gem.c + drm_kms.c)                 │
│  ├── GEM：显存对象分配/释放/映射                         │
│  └── KMS：CRTC/Plane/Connector/FB 对象管理              │
├─────────────────────────────────────────────────────────┤
│  第三层：硬件驱动 (drm_bochs.c)                         │
│  ├── BGA 寄存器编程                                     │
│  ├── VRAM ioremap 映射                                  │
│  └── CRTC/Plane/Connector 回调实现                      │
└─────────────────────────────────────────────────────────┘
```

### 对象层次关系

```
drm_device
    ├── driver: drm_driver
    ├── gem_objects: [drm_gem_object, ...]
    └── mode_config: drm_mode_config
            ├── crtc_list: [drm_crtc, ...]
            ├── plane_list: [drm_plane, ...]
            ├── connector_list: [drm_connector, ...]
            ├── encoder_list: [drm_encoder, ...]
            └── fb_list: [drm_framebuffer, ...]

drm_framebuffer ──► drm_gem_object（像素数据）
drm_crtc ──► drm_framebuffer（当前显示内容）
drm_plane ──► drm_framebuffer（图层内容）
drm_encoder ──► drm_crtc + drm_connector（信号转换）
```

---

## 数据结构

### drm_device - DRM 设备

```c
struct drm_device {
    struct device dev;              // 嵌入统一设备模型
    struct drm_driver *driver;      // 绑定的驱动
    void *dev_private;              // 驱动私有数据（如 bochs_device）
    struct list_head gem_objects;   // GEM 对象链表
    uint32_t next_handle;           // handle 分配计数器
    struct drm_mode_config *mode_config;  // KMS 配置
    int registered;                 // 是否已注册
};
```

### drm_driver - DRM 驱动

```c
struct drm_driver {
    const char *name;               // 驱动名称
    const char *desc;               // 描述
    int  (*load)(struct drm_device *dev);    // 硬件初始化
    void (*unload)(struct drm_device *dev);  // 硬件清理
    const struct drm_ioctl_desc *ioctls;     // 驱动私有 ioctl 表
    int num_ioctls;
    struct list_head list;          // 链入全局驱动链表
};
```

### drm_gem_object - 显存对象

```c
struct drm_gem_object {
    struct drm_device *dev;         // 所属设备
    unsigned long size;             // 字节数（PAGE_SIZE 对齐）
    void *vaddr;                    // 内核虚拟地址
    unsigned long phys_addr;        // 物理地址
    uint32_t handle;                // 用户态句柄
    int refcount;                   // 引用计数
    struct list_head list;          // 链入设备 gem_objects
};
```

### KMS 对象

```c
struct drm_crtc {
    struct drm_device *dev;
    struct drm_display_mode mode;   // 当前分辨率/刷新率
    struct drm_framebuffer *fb;     // 当前 FB
    int active;
    const struct drm_crtc_funcs *funcs;  // 驱动回调
    void *driver_private;
};

struct drm_framebuffer {
    struct drm_device *dev;
    struct drm_gem_object *obj;     // 像素数据
    uint32_t width, height, pitch;
    uint32_t format;                // DRM_FORMAT_XRGB8888 等
};

struct drm_connector {
    struct drm_device *dev;
    enum drm_connector_type type;   // VGA/HDMI/VIRTUAL
    enum drm_connector_status status;
    struct drm_display_mode modes[8]; // 支持的分辨率
    int num_modes;
};
```

---

## 启动流程

### 初始化时序

```
kernel.c::_kernel_main()
    │
    ├── setup_arch()              ← 架构初始化
    ├── mm_init()                 ← 内存管理
    ├── kmem_cache_init()         ← Slab 分配器
    ├── platform_bus_init()       ← Platform 总线
    ├── pci_init()                ← PCI 枚举（发现 VGA 设备）
    │
    ├── drm_core_init()           ← 【DRM 核心初始化】
    │       └── INIT_LIST_HEAD(&drm_driver_list)
    │
    └── drm_bochs_init()          ← 【Bochs 驱动初始化】
            │
            ├── drm_register_driver()     注册驱动到全局链表
            │
            └── drm_device_alloc()        创建 drm_device
                    │
                    ├── drm_gem_init_device()    初始化 GEM 链表
                    ├── drm_mode_config_init()   初始化 KMS 配置
                    │
                    └── bochs_drm_load()          驱动加载
                            │
                            ├── pci_find_device(0x1234, 0x1111)
                            │       查找 QEMU std-vga
                            │
                            ├── fb_ioremap(VRAM)
                            │       映射 BAR0 到虚拟地址
                            │
                            ├── bochs_set_resolution(1024x768x32)
                            │       写 BGA 寄存器设置默认分辨率
                            │
                            ├── drm_crtc_init()      创建 CRTC
                            ├── drm_plane_init()     创建 Primary Plane
                            ├── drm_connector_init() 创建 Connector
                            └── drm_encoder_init()   创建 Encoder
```

### BGA 寄存器编程流程

```c
// 设置分辨率的寄存器写入顺序
bochs_write_reg(BGA_REG_ENABLE, 0);          // 1. 禁用显示
bochs_write_reg(BGA_REG_XRES, 1024);         // 2. 设置宽度
bochs_write_reg(BGA_REG_YRES, 768);          // 3. 设置高度
bochs_write_reg(BGA_REG_BPP, 32);            // 4. 设置色深
bochs_write_reg(BGA_REG_VIRT_WIDTH, 1024);   // 5. 虚拟宽度
bochs_write_reg(BGA_REG_VIRT_HEIGHT, 1536);  // 6. 虚拟高度（2x，双缓冲）
bochs_write_reg(BGA_REG_ENABLE, 0x41);       // 7. 启用（LFB + Enable）
```

---

## GEM 显存管理

### 显存分配流程

```
用户请求: drm_gem_create(dev, 1920*1080*4)
        │
        ▼
PAGE_SIZE 对齐: size = 8,294,400 → 8,294,400 (已对齐)
        │
        ▼
页数计算: pages = 2025, order = 11
        │
        ▼
__alloc_pages(GFP_KERNEL, 11)
        │
        ├── 伙伴系统分配 2^11 = 2048 连续页
        │
        └── 返回 struct page *
                │
                ├── pfn = page - mem_map
                ├── phys = pfn << PAGE_SHIFT
                └── vaddr = page->virtual 或 __va(phys)

结果:
  obj->phys_addr = 0x12340000    物理地址
  obj->vaddr     = 0xD2340000    内核虚拟地址
  obj->handle    = 1              用户态句柄
  obj->size      = 8,388,608      实际分配大小
```

### Handle 机制

```
用户态通过 handle (uint32_t) 引用 GEM 对象：

handle=1 ──► drm_gem_find(dev, 1) ──► 遍历 gem_objects 链表 ──► 返回 obj

内核隔离物理地址，用户态无法直接访问显存物理地址。
```

---

## KMS 显示控制

### Mode Setting 流程

```
drm_mode_setcrtc(dev, crtc_id=0, fb_id=1, mode=1920x1080@60)
        │
        ▼
drm_crtc_find(dev, 0)           查找 CRTC 0
        │
        ▼
drm_framebuffer_find(dev, 1)    查找 FB 1
        │
        ▼
crtc->funcs->set_mode(crtc, mode)
        │
        ▼
bochs_crtc_set_mode()
        │
        └── bochs_set_resolution(bochs, 1920, 1080, 32)
                    │
                    └── 写 BGA 寄存器切换分辨率
```

### Page Flip 双缓冲

```
VRAM 布局（虚拟高度 = 实际高度 × 2）：

┌─────────────────────────┐  Y=0
│      前缓冲 (显示中)     │  1080 行
├─────────────────────────┤  Y=1080
│      后缓冲 (写入中)     │  1080 行
└─────────────────────────┘  Y=2160

Page Flip 流程：
  1. 新帧内容写入后缓冲 (Y=1080)
  2. bochs_write_reg(BGA_REG_Y_OFFSET, 1080)
  3. 显示器开始扫描后缓冲 → 无撕裂
  4. 下一帧写入前缓冲 (Y=0)
  5. bochs_write_reg(BGA_REG_Y_OFFSET, 0)
  6. 交替切换...
```

---

## Bochs/QEMU VBE 驱动

### PCI 设备识别

| 设备 | Vendor ID | Device ID | VRAM 来源 |
|---|---|---|---|
| QEMU std-vga | 0x1234 | 0x1111 | PCI BAR0 |
| Bochs VBE | N/A (ISA) | N/A | 固定 0xE0000000 |

### 预设分辨率

| 宽度 | 高度 | 刷新率 |
|---|---|---|
| 640 | 480 | 60Hz |
| 800 | 600 | 60Hz |
| 1024 | 768 | 60Hz（默认） |
| 1280 | 1024 | 60Hz |
| 1600 | 1200 | 60Hz |
| 1920 | 1080 | 60Hz |

### 驱动私有数据

```c
struct bochs_device {
    struct drm_device *drm;
    struct pci_dev *pci_dev;
    unsigned long vram_phys;      // VRAM 物理地址
    unsigned long vram_size;      // VRAM 大小
    void *vram_virt;              // VRAM 虚拟地址
    struct drm_crtc crtc;
    struct drm_plane primary_plane;
    struct drm_connector connector;
    struct drm_encoder encoder;
    uint32_t current_yoffset;     // 双缓冲偏移
};
```

---

## ioctl 接口

### 命令列表

| 编号 | 命令 | 功能 |
|---|---|---|
| 0x00 | DRM_IOCTL_GET_CAP | 查询设备能力 |
| 0x01 | DRM_IOCTL_VERSION | 查询驱动版本 |
| 0x10 | DRM_IOCTL_GEM_CREATE | 分配显存，返回 handle |
| 0x11 | DRM_IOCTL_GEM_MMAP | 映射显存到用户空间 |
| 0x12 | DRM_IOCTL_GEM_CLOSE | 释放显存 |
| 0x20 | DRM_IOCTL_MODE_GETRESOURCES | 枚举 KMS 资源 |
| 0x21 | DRM_IOCTL_MODE_ADDFB2 | 从 GEM handle 创建 FB |
| 0x22 | DRM_IOCTL_MODE_RMFB | 销毁 FB |
| 0x23 | DRM_IOCTL_MODE_SETCRTC | 设置分辨率 + 绑定 FB |
| 0x24 | DRM_IOCTL_MODE_PAGE_FLIP | 双缓冲翻页 |

### 使用示例

```c
// 分配显存
struct drm_gem_create req = { .size = 1920 * 1080 * 4 };
drm_ioctl(dev, DRM_IOCTL_GEM_CREATE, &req);
// req.handle = 1

// 创建 Framebuffer
struct drm_mode_fb_cmd2 fb_req = {
    .width = 1920, .height = 1080,
    .pixel_format = DRM_FORMAT_XRGB8888,
    .handles[0] = 1,
    .pitches[0] = 1920 * 4
};
drm_ioctl(dev, DRM_IOCTL_MODE_ADDFB2, &fb_req);
// fb_req.fb_id = 0

// 设置 CRTC
struct drm_mode_crtc crtc_req = {
    .crtc_id = 0, .fb_id = 0,
    .mode_valid = 1,
    .hdisplay = 1920, .vdisplay = 1080, .vrefresh = 60
};
drm_ioctl(dev, DRM_IOCTL_MODE_SETCRTC, &crtc_req);
```

---

## 文件结构

```
includes/drm/
├── drm_core.h        ← drm_device, drm_driver, drm_ioctl_desc
├── drm_gem.h         ← drm_gem_object, GEM API
├── drm_kms.h         ← drm_crtc/plane/connector/fb/display_mode
└── drm_ioctl.h       ← ioctl 编号 + 参数结构体

kernel/drm/
├── drm_core.c        ← 驱动注册、ioctl 分发、设备管理
├── drm_gem.c         ← 显存分配（__alloc_pages）、handle 管理
├── drm_kms.c         ← KMS 对象管理、mode setting、page flip
└── drm_bochs.c       ← Bochs/QEMU VBE 驱动
```

---

## 参考

- Linux DRM 源码：`drivers/gpu/drm/`
- Linux GEM：`drivers/gpu/drm/drm_gem.c`
- Linux KMS：`drivers/gpu/drm/drm_crtc.c`
- Bochs VBE 规范：http://www.osdever.net/tutorials/view/vbe-the-vesa-bios-extensions
- Linux bochs-drm 驱动：`drivers/gpu/drm/tiny/bochs.c`
