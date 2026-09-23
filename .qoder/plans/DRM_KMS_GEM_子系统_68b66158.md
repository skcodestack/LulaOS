# DRM / KMS / GEM 子系统实现计划

## 现状分析

- **PCI 子系统** (`kernel/pci/pci.c`, `includes/pci/pci.h`)：已完成枚举、BAR 解析、驱动注册
- **Framebuffer** (`kernel/video/fb.c`)：基于 GRUB VBE 的文本渲染，无 KMS 抽象
- **设备模型** (`kernel/device/`)：bus_type + device + driver 匹配框架已就绪
- **缺失**：无字符设备层、无 DRM 框架、无 GEM、无 KMS 对象抽象

---

## 文件结构规划

```
includes/drm/
  drm_core.h        -- drm_device, drm_driver, drm_file, ioctl 表
  drm_gem.h         -- drm_gem_object, GEM API
  drm_kms.h         -- drm_crtc, drm_plane, drm_connector, drm_encoder,
                       drm_framebuffer, drm_mode_config, drm_display_mode
  drm_ioctl.h       -- ioctl 编号与命令定义

kernel/drm/
  drm_core.c        -- DRM 核心初始化、驱动注册、ioctl 分发
  drm_gem.c         -- GEM 对象分配、释放、mmap
  drm_kms.c         -- KMS 对象管理、mode setting、atomic commit
  drm_bochs.c       -- Bochs/QEMU VBE DRM 驱动（CRTC/Plane/Connector）

kernel/drm/drm_bochs.c 目标设备:
  - QEMU std-vga:  PCI 0x1234:0x1111 (BAR0 = VRAM)
  - Bochs VBE:     BGA 寄存器端口 0x01CE/0x01CF，VRAM 在 0xE0000000（ISA）
```

---

## Task 1：DRM 核心框架

新建 `includes/drm/drm_core.h`，定义核心数据结构：

```c
struct drm_device {
    struct device dev;              // 嵌入统一设备模型
    struct drm_driver *driver;
    struct drm_mode_config *mode_config;
    void *dev_private;              // 驱动私有数据
    struct list_head gem_objects;   // 所有 GEM 对象链表
    uint32_t next_handle;           // handle 分配计数器
};

struct drm_driver {
    const char *name;
    int (*load)(struct drm_device *dev);
    void (*unload)(struct drm_device *dev);
    const struct drm_ioctl_desc *ioctls;
    int num_ioctls;
};

void drm_core_init(void);           // 子系统初始化
int  drm_register_driver(struct drm_driver *drv);
void drm_unregister_driver(struct drm_driver *drv);
```

新建 `kernel/drm/drm_core.c`，实现驱动注册表和 ioctl 分发逻辑。

---

## Task 2：GEM 内存管理

新建 `includes/drm/drm_gem.h`：

```c
struct drm_gem_object {
    struct drm_device *dev;
    unsigned long size;             // 字节数（PAGE_SIZE 对齐）
    void *vaddr;                    // 内核虚拟地址（kmalloc / ioremap）
    unsigned long phys_addr;        // 物理地址（或页数组）
    uint32_t handle;                // 用户态句柄
    int refcount;
    struct list_head list;          // 链入 drm_device.gem_objects
};

struct drm_gem_object *drm_gem_create(struct drm_device *dev, unsigned long size);
void drm_gem_destroy(struct drm_gem_object *obj);
int  drm_gem_mmap(struct drm_gem_object *obj, unsigned long user_vaddr);
struct drm_gem_object *drm_gem_find(struct drm_device *dev, uint32_t handle);
```

新建 `kernel/drm/drm_gem.c`：
- `drm_gem_create()`：用 `kmalloc`（或 `__get_free_pages`）分配物理连续页，记录物理地址
- `drm_gem_mmap()`：调用现有 `fb_ioremap()` 将物理页映射到用户空间虚拟地址
- handle 管理：用简单 idr（数组 + handle 计数器）实现句柄查找

---

## Task 3：KMS 显示控制对象

新建 `includes/drm/drm_kms.h`：

```c
struct drm_display_mode {
    uint32_t hdisplay, vdisplay;
    uint32_t hsync_start, hsync_end;
    uint32_t vsync_start, vsync_end;
    uint32_t htotal, vtotal;
    uint32_t clock;                 // kHz
    uint32_t vrefresh;
};

struct drm_crtc {
    struct drm_device *dev;
    struct drm_display_mode mode;
    struct drm_framebuffer *fb;     // 当前绑定的 FB
    bool active;
    const struct drm_crtc_funcs *funcs;
    void *driver_private;
};

struct drm_plane {
    struct drm_device *dev;
    struct drm_framebuffer *fb;
    struct drm_crtc *crtc;
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    enum drm_plane_type type;       // PRIMARY / CURSOR / OVERLAY
    const struct drm_plane_funcs *funcs;
};

struct drm_connector {
    struct drm_device *dev;
    enum drm_connector_status status;  // CONNECTED / DISCONNECTED
    struct drm_display_mode modes[8];  // 支持的分辨率列表
    int num_modes;
    const struct drm_connector_funcs *funcs;
};

struct drm_encoder {
    struct drm_device *dev;
    struct drm_crtc *crtc;
    struct drm_connector *connector;
};

struct drm_framebuffer {
    struct drm_device *dev;
    struct drm_gem_object *obj;     // 指向像素数据 GEM 对象
    uint32_t width, height, pitch;
    uint32_t format;                // DRM_FORMAT_XRGB8888
};

struct drm_mode_config {
    struct list_head crtc_list;
    struct list_head plane_list;
    struct list_head connector_list;
    int num_crtc, num_plane, num_connector;
};
```

新建 `kernel/drm/drm_kms.c`，实现：
- 对象注册/注销（链表管理）
- `drm_mode_setcrtc()`：验证参数 → 更新 CRTC → 通知驱动写寄存器
- `drm_page_flip()`：切换 CRTC 的 FB 指针（等 VBlank）

---

## Task 4：Bochs/QEMU VBE DRM 驱动

新建 `kernel/drm/drm_bochs.c`，核心逻辑：

```c
/* BGA 寄存器（Bochs VBE Extensions）*/
#define BGA_IO_INDEX   0x01CE
#define BGA_IO_DATA    0x01CF
#define BGA_REG_XRES   0x01
#define BGA_REG_YRES   0x02
#define BGA_REG_BPP    0x03
#define BGA_REG_ENABLE 0x04
#define BGA_REG_BANK   0x05
#define BGA_REG_VWIDTH 0x06
#define BGA_REG_VHEIGHT 0x07

static void bochs_set_mode(struct drm_crtc *crtc, struct drm_display_mode *mode)
{
    outw(BGA_IO_INDEX, BGA_REG_ENABLE); outw(BGA_IO_DATA, 0); // 禁用
    outw(BGA_IO_INDEX, BGA_REG_XRES);   outw(BGA_IO_DATA, mode->hdisplay);
    outw(BGA_IO_INDEX, BGA_REG_YRES);   outw(BGA_IO_DATA, mode->vdisplay);
    outw(BGA_IO_INDEX, BGA_REG_BPP);    outw(BGA_IO_DATA, 32);
    outw(BGA_IO_INDEX, BGA_REG_VWIDTH); outw(BGA_IO_DATA, mode->hdisplay);
    outw(BGA_IO_INDEX, BGA_REG_VHEIGHT);outw(BGA_IO_DATA, mode->vdisplay * 2); // 双缓冲
    outw(BGA_IO_INDEX, BGA_REG_ENABLE); outw(BGA_IO_DATA, 1); // 启用 LFB
}
```

驱动 probe 流程：
1. `pci_find_class(PCI_CLASS_DISPLAY)` 查找显示设备
2. 读取 BAR0 获取 VRAM 物理地址
3. `fb_ioremap()` 映射 VRAM
4. 创建 CRTC + Primary Plane + Connector
5. 设置默认分辨率（1024x768x32，与 GRUB 一致）

---

## Task 5：ioctl 接口与系统调用

新建 `includes/drm/drm_ioctl.h`，定义 ioctl 命令：

| ioctl 命令 | 功能 |
|---|---|
| `DRM_IOCTL_GET_CAP` | 查询设备能力 |
| `DRM_IOCTL_GEM_CREATE` | 分配显存，返回 handle |
| `DRM_IOCTL_GEM_MMAP` | 映射显存到用户虚拟地址 |
| `DRM_IOCTL_GEM_CLOSE` | 释放显存对象 |
| `DRM_IOCTL_MODE_GETRESOURCES` | 枚举 CRTC/Plane/Connector |
| `DRM_IOCTL_MODE_ADDFB2` | 从 GEM handle 创建 FB |
| `DRM_IOCTL_MODE_SETCRTC` | 设置分辨率 + 绑定 FB |
| `DRM_IOCTL_MODE_PAGE_FLIP` | 双缓冲翻页 |

在 `kernel/drm/drm_core.c` 中实现 ioctl 分发函数，由 syscall 层调用。

---

## Task 6：集成与初始化顺序

修改 `kernel/kernel.c` 的初始化顺序：

```c
// 当前顺序
platform_bus_init();
keyboard_init();
mouse_init();
pci_init();

// 新增（在 pci_init 之后）
drm_core_init();           // 初始化 DRM 框架
drm_bochs_init();          // 注册 Bochs/QEMU VBE 驱动（PCI 匹配）
```

驱动加载后接管现有 `fb.c` 的 FB 控制权，TTY 层切换到 DRM 模式。

---

## Task 7：验证

在 QEMU/Bochs 中验证：
- PCI 枚举能发现 VGA 设备，DRM 驱动自动 probe
- `drm_gem_create(1920*1080*4)` 分配成功，返回 handle
- `drm_mode_setcrtc(1920x1080)` 成功切换分辨率
- Page Flip 双缓冲：写入后缓冲 → 翻页 → 无撕裂切换
- TTY 文本输出在 DRM 模式下正常渲染

---

## 实现顺序依赖

```
Task 1 (DRM核心)
   │
   ├──► Task 2 (GEM)  ──► 可独立测试内存分配
   │
   └──► Task 3 (KMS)  ──► 可独立测试对象注册
              │
              └──► Task 4 (Bochs驱动)  ──► 依赖 Task 2+3
                        │
                        └──► Task 5 (ioctl)  ──► 依赖 Task 4
                                  │
                                  └──► Task 6 (集成)
                                            │
                                            └──► Task 7 (验证)
```
