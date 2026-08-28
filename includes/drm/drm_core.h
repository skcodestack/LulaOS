/*
 * LulaOS DRM 核心框架
 *
 * 参考 Linux DRM 子系统（drivers/gpu/drm/）实现简化版：
 *   - drm_device: DRM 设备描述符，嵌入统一设备模型
 *   - drm_driver: DRM 驱动描述符，定义 load/unload/ioctl 回调
 *   - drm_file: 用户态打开的文件上下文（简化版）
 *
 * 核心流程：
 *   drm_core_init() → drm_register_driver() → 驱动 probe →
 *   创建 drm_device → 调用 driver->load() 初始化硬件
 */

#ifndef __DRM_CORE_H__
#define __DRM_CORE_H__

#include <stdint.h>
#include <libs/list.h>
#include <device/device.h>

/* 前向声明（在各自头文件中完整定义） */
struct drm_mode_config;
struct drm_gem_object;
struct drm_crtc;
struct drm_plane;
struct drm_connector;
struct drm_framebuffer;
struct drm_display_mode;

/* ======================== ioctl 描述 ======================== */

/*
 * drm_ioctl_desc - 单个 ioctl 命令的描述
 *
 * cmd  : ioctl 编号（见 drm_ioctl.h）
 * func : 处理函数，返回 0 表示成功，负数表示错误码
 */
struct drm_ioctl_desc {
    unsigned int cmd;
    int (*func)(struct drm_device *dev, void *data);
};

/* ======================== DRM 驱动 ======================== */

/*
 * drm_driver - DRM 驱动描述符
 *
 * 由各 GPU 驱动（bochs/amdgpu/i915）填充，
 * 注册到 DRM 核心，PCI 匹配成功后由核心调用 load()。
 */
struct drm_driver {
    const char *name;                           /* 驱动名称，如 "bochs-drm" */
    const char *desc;                           /* 描述信息 */

    /* 生命周期回调 */
    int  (*load)(struct drm_device *dev);       /* 硬件初始化 */
    void (*unload)(struct drm_device *dev);     /* 硬件清理 */

    /* ioctl 表 */
    const struct drm_ioctl_desc *ioctls;
    int num_ioctls;

    /* 链入全局 drm_driver_list */
    struct list_head list;
};

/* ======================== DRM 设备 ======================== */

/*
 * drm_device - DRM 设备描述符
 *
 * 嵌入 struct device 作为第一个成员，以便挂入统一设备模型。
 * 一个 drm_device 对应一个 GPU 实例。
 */
struct drm_device {
    struct device dev;                          /* 必须作为第一个成员 */

    struct drm_driver *driver;                  /* 绑定的驱动 */
    void *dev_private;                          /* 驱动私有数据 */

    /* GEM 内存管理 */
    struct list_head gem_objects;               /* 所有 GEM 对象链表 */
    uint32_t next_handle;                       /* handle 分配计数器 */

    /* KMS 显示控制 */
    struct drm_mode_config *mode_config;        /* 显示模式配置 */

    /* 状态 */
    int registered;                             /* 是否已注册 */
};

/* ======================== 全局驱动链表 ======================== */

extern struct list_head drm_driver_list;

/* ======================== 公共 API ======================== */

/*
 * drm_core_init - 初始化 DRM 子系统
 *
 * 在 kernel.c 中 pci_init() 之后调用
 */
void drm_core_init(void);

/*
 * drm_register_driver - 注册 DRM 驱动
 *
 * 将驱动加入全局链表，并尝试匹配已发现的 PCI 显示设备
 *
 * @drv: 驱动描述符
 * 返回: 0 表示成功，负数表示错误
 */
int drm_register_driver(struct drm_driver *drv);

/*
 * drm_unregister_driver - 注销 DRM 驱动
 *
 * @drv: 驱动描述符
 */
void drm_unregister_driver(struct drm_driver *drv);

/*
 * drm_ioctl - DRM ioctl 分发入口
 *
 * 由 syscall 层调用，根据 cmd 查找驱动 ioctl 表并执行
 *
 * @dev: DRM 设备
 * @cmd: ioctl 命令编号
 * @data: 用户传入的参数指针
 * 返回: 0 表示成功，负数表示错误
 */
int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *data);

/* ======================== 辅助宏 ======================== */

/* 从 struct device * 获取 drm_device */
#define to_drm_device(d)    container_of(d, struct drm_device, dev)

/* 设备管理 */
struct drm_device *drm_device_alloc(struct drm_driver *drv);
void drm_device_free(struct drm_device *dev);
struct drm_device *drm_get_global_device(void);

/* Bochs/QEMU VBE 驱动入口 */
void drm_bochs_init(void);

/* DRM 错误码（与 Linux errno 风格一致） */
#define DRM_ERR_INVAL       (-1)    /* 参数无效 */
#define DRM_ERR_NOMEM       (-2)    /* 内存不足 */
#define DRM_ERR_NODEV       (-3)    /* 设备不存在 */
#define DRM_ERR_BUSY        (-4)    /* 设备忙 */
#define DRM_ERR_NOTSUPP     (-5)    /* 不支持的操作 */

#endif /* __DRM_CORE_H__ */
