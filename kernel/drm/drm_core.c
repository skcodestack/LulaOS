/*
 * DRM 核心框架实现
 *
 * 参考 Linux DRM 核心（drivers/gpu/drm/drm_drv.c）：
 *   - 全局驱动链表管理
 *   - 驱动注册/注销
 *   - ioctl 命令分发
 *
 * 流程：
 *   drm_core_init() 初始化全局链表
 *   drm_register_driver() 将驱动加入链表
 *   drm_ioctl() 根据 cmd 查找驱动 ioctl 表并调用对应 func
 */

#include <drm/drm_core.h>
#include <drm/drm_gem.h>
#include <drm/drm_kms.h>
#include <drm/drm_ioctl.h>
#include <mm/slab.h>
#include <printk.h>
#include <libs/memcpy.h>

/* ======================== 全局状态 ======================== */

/* 全局驱动链表：所有已注册的 drm_driver */
struct list_head drm_driver_list;

/* 全局 DRM 设备（简化实现：单设备） */
static struct drm_device *drm_global_dev = (void *)0;

/* ======================== 核心 ioctl 处理 ======================== */

/*
 * drm_ioctl_get_cap - 处理设备能力查询
 */
static int drm_ioctl_get_cap(struct drm_device *dev, void *data)
{
    struct drm_get_cap *req = (struct drm_get_cap *)data;

    switch (req->capability) {
    case DRM_CAP_DUMB_BUFFER:
        req->value = 1;       /* 支持 dumb buffer */
        return 0;
    case DRM_CAP_VBLANK:
        req->value = 0;       /* 暂不支持 VBlank 中断 */
        return 0;
    case DRM_CAP_PAGE_FLIP:
        req->value = 1;       /* 支持 Page Flip */
        return 0;
    default:
        return DRM_ERR_INVAL;
    }
}

/*
 * drm_ioctl_version - 处理版本查询
 */
static int drm_ioctl_version(struct drm_device *dev, void *data)
{
    struct drm_version *ver = (struct drm_version *)data;

    ver->version_major = 1;
    ver->version_minor = 0;

    if (dev->driver->name) {
        /* 手动拷贝名称，避免依赖 strcpy */
        const char *src = dev->driver->name;
        int i;
        for (i = 0; src[i] && i < 31; i++)
            ver->name[i] = src[i];
        ver->name[i] = '\0';
    }

    if (dev->driver->desc) {
        const char *src = dev->driver->desc;
        int i;
        for (i = 0; src[i] && i < 63; i++)
            ver->desc[i] = src[i];
        ver->desc[i] = '\0';
    }

    return 0;
}

/* ======================== GEM ioctl 处理 ======================== */

/*
 * drm_ioctl_gem_create - 分配 GEM 对象
 */
static int drm_ioctl_gem_create(struct drm_device *dev, void *data)
{
    struct drm_gem_create *req = (struct drm_gem_create *)data;
    struct drm_gem_object *obj;

    if (req->size == 0)
        return DRM_ERR_INVAL;

    obj = drm_gem_create(dev, (unsigned long)req->size);
    if (!obj)
        return DRM_ERR_NOMEM;

    req->handle = obj->handle;
    return 0;
}

/*
 * drm_ioctl_gem_mmap - 映射 GEM 对象到用户空间
 */
static int drm_ioctl_gem_mmap(struct drm_device *dev, void *data)
{
    struct drm_gem_mmap *req = (struct drm_gem_mmap *)data;
    struct drm_gem_object *obj;

    obj = drm_gem_find(dev, req->handle);
    if (!obj)
        return DRM_ERR_INVAL;

    /* 返回内核虚拟地址（简化实现，真实内核需要隔离用户态） */
    req->offset = (uint64_t)(unsigned long)obj->vaddr;
    return 0;
}

/*
 * drm_ioctl_gem_close - 释放 GEM 对象
 */
static int drm_ioctl_gem_close(struct drm_device *dev, void *data)
{
    struct drm_gem_close *req = (struct drm_gem_close *)data;
    struct drm_gem_object *obj;

    obj = drm_gem_find(dev, req->handle);
    if (!obj)
        return DRM_ERR_INVAL;

    drm_gem_put(obj);
    return 0;
}

/* ======================== KMS ioctl 处理 ======================== */

/*
 * drm_ioctl_mode_getresources - 枚举 KMS 资源
 */
static int drm_ioctl_mode_getresources(struct drm_device *dev, void *data)
{
    struct drm_mode_getresources *req = (struct drm_mode_getresources *)data;
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return DRM_ERR_NOTSUPP;

    /* 返回数量（用户据此分配数组） */
    req->count_crtcs     = config->num_crtc;
    req->count_connectors= config->num_connector;
    req->count_encoders  = config->num_encoder;
    req->count_fbs       = config->num_fb;

    /* 如果用户提供了数组指针，填充 ID 列表 */
    if (req->crtc_ids && req->count_crtcs > 0) {
        struct list_head *pos;
        int i = 0;
        list_for_each(pos, &config->crtc_list) {
            struct drm_crtc *crtc = list_entry(pos, struct drm_crtc, list);
            if (i < (int)req->count_crtcs)
                req->crtc_ids[i++] = crtc->id;
        }
    }

    if (req->connector_ids && req->count_connectors > 0) {
        struct list_head *pos;
        int i = 0;
        list_for_each(pos, &config->connector_list) {
            struct drm_connector *conn = list_entry(pos, struct drm_connector, list);
            if (i < (int)req->count_connectors)
                req->connector_ids[i++] = conn->id;
        }
    }

    return 0;
}

/*
 * drm_ioctl_mode_addfb2 - 创建 Framebuffer
 */
static int drm_ioctl_mode_addfb2(struct drm_device *dev, void *data)
{
    struct drm_mode_fb_cmd2 *req = (struct drm_mode_fb_cmd2 *)data;
    struct drm_gem_object *obj;
    struct drm_framebuffer *fb;

    obj = drm_gem_find(dev, req->handles[0]);
    if (!obj)
        return DRM_ERR_INVAL;

    fb = (struct drm_framebuffer *)kmalloc(sizeof(*fb), GFP_KERNEL);
    if (!fb)
        return DRM_ERR_NOMEM;

    memset(fb, 0, sizeof(*fb));

    int ret = drm_framebuffer_init(dev, fb, obj,
                                    req->width, req->height,
                                    req->pitches[0], req->pixel_format);
    if (ret) {
        kfree(fb);
        return ret;
    }

    req->fb_id = fb->id;
    return 0;
}

/*
 * drm_ioctl_mode_setcrtc - 设置 CRTC 模式
 */
static int drm_ioctl_mode_setcrtc(struct drm_device *dev, void *data)
{
    struct drm_mode_crtc *req = (struct drm_mode_crtc *)data;
    struct drm_display_mode mode;

    if (req->mode_valid) {
        drm_mode_make_default(&mode, req->hdisplay, req->vdisplay, req->vrefresh);
        return drm_mode_setcrtc(dev, req->crtc_id, req->fb_id, &mode);
    }

    /* 不修改模式，仅切换 FB */
    return drm_mode_setcrtc(dev, req->crtc_id, req->fb_id, (void *)0);
}

/*
 * drm_ioctl_mode_page_flip - 双缓冲翻页
 */
static int drm_ioctl_mode_page_flip(struct drm_device *dev, void *data)
{
    struct drm_mode_page_flip *req = (struct drm_mode_page_flip *)data;

    return drm_mode_page_flip(dev, req->crtc_id, req->fb_id);
}

/* ======================== 内置 ioctl 表 ======================== */

static const struct drm_ioctl_desc drm_core_ioctls[] = {
    { DRM_IOCTL_GET_CAP,           drm_ioctl_get_cap },
    { DRM_IOCTL_VERSION,           drm_ioctl_version },
    { DRM_IOCTL_GEM_CREATE,        drm_ioctl_gem_create },
    { DRM_IOCTL_GEM_MMAP,          drm_ioctl_gem_mmap },
    { DRM_IOCTL_GEM_CLOSE,         drm_ioctl_gem_close },
    { DRM_IOCTL_MODE_GETRESOURCES, drm_ioctl_mode_getresources },
    { DRM_IOCTL_MODE_ADDFB2,       drm_ioctl_mode_addfb2 },
    { DRM_IOCTL_MODE_SETCRTC,      drm_ioctl_mode_setcrtc },
    { DRM_IOCTL_MODE_PAGE_FLIP,    drm_ioctl_mode_page_flip },
};

#define DRM_CORE_IOCTL_COUNT    (sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0]))

/* ======================== 公共 API ======================== */

void drm_core_init(void)
{
    INIT_LIST_HEAD(&drm_driver_list);

    printk("[drm] DRM core initialized\n");
}

int drm_register_driver(struct drm_driver *drv)
{
    if (!drv || !drv->name)
        return DRM_ERR_INVAL;

    INIT_LIST_HEAD(&drv->list);
    list_add_tail(&drv->list, &drm_driver_list);

    printk("[drm] Registered driver: %s\n", drv->name);

    return 0;
}

void drm_unregister_driver(struct drm_driver *drv)
{
    if (!drv)
        return;

    list_del(&drv->list);
    printk("[drm] Unregistered driver: %s\n", drv->name);
}

int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *data)
{
    unsigned int i;

    if (!dev || !dev->driver)
        return DRM_ERR_NODEV;

    /* 先在核心 ioctl 表中查找 */
    for (i = 0; i < DRM_CORE_IOCTL_COUNT; i++) {
        if (drm_core_ioctls[i].cmd == cmd) {
            return drm_core_ioctls[i].func(dev, data);
        }
    }

    /* 再在驱动私有 ioctl 表中查找 */
    if (dev->driver->ioctls) {
        for (i = 0; i < (unsigned int)dev->driver->num_ioctls; i++) {
            if (dev->driver->ioctls[i].cmd == cmd) {
                return dev->driver->ioctls[i].func(dev, data);
            }
        }
    }

    return DRM_ERR_NOTSUPP;
}

/* ======================== 设备管理辅助 ======================== */

/*
 * drm_device_alloc - 分配并初始化 drm_device
 *
 * 由 GPU 驱动在 probe 中调用
 */
struct drm_device *drm_device_alloc(struct drm_driver *drv)
{
    struct drm_device *dev;

    dev = (struct drm_device *)kmalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return (void *)0;

    memset(dev, 0, sizeof(*dev));

    dev->driver = drv;
    INIT_LIST_HEAD(&dev->gem_objects);
    dev->next_handle = 1;   /* handle 从 1 开始，0 保留为无效值 */

    /* 初始化 GEM 子系统 */
    drm_gem_init_device(dev);

    /* 初始化 KMS mode_config */
    drm_mode_config_init(dev);

    /* 记录全局设备（简化：单设备） */
    drm_global_dev = dev;

    return dev;
}

/*
 * drm_device_free - 释放 drm_device
 */
void drm_device_free(struct drm_device *dev)
{
    if (!dev)
        return;

    /* 清理 KMS 对象 */
    drm_mode_config_cleanup(dev);

    /* 清理 GEM 对象 */
    drm_gem_cleanup_device(dev);

    if (drm_global_dev == dev)
        drm_global_dev = (void *)0;

    kfree(dev);
}

/*
 * drm_get_global_device - 获取全局 DRM 设备
 *
 * 用于测试和内部调用
 */
struct drm_device *drm_get_global_device(void)
{
    return drm_global_dev;
}
