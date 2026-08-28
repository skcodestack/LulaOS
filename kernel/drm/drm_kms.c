/*
 * DRM KMS (Kernel Mode Setting) 显示控制实现
 *
 * 参考 Linux DRM KMS（drivers/gpu/drm/drm_crtc.c 等）：
 *   - CRTC / Plane / Connector / Encoder / Framebuffer 对象管理
 *   - drm_mode_setcrtc(): 设置分辨率和 FB
 *   - drm_mode_page_flip(): 双缓冲翻页
 *
 * 对象注册流程：
 *   驱动在 load() 中创建 CRTC/Plane/Connector，
 *   调用 drm_crtc_init() 等函数注册到 mode_config，
 *   后续由 ioctl 层通过 ID 查找和操作。
 */

#include <drm/drm_kms.h>
#include <drm/drm_core.h>
#include <drm/drm_gem.h>
#include <mm/slab.h>
#include <printk.h>
#include <libs/memcpy.h>

/* ======================== 辅助函数 ======================== */

/*
 * drm_mode_make_default - 生成标准显示模式
 *
 * 根据分辨率和刷新率，计算完整的时序参数。
 * 使用 CVT-RB (Reduced Blanking) 简化计算。
 */
void drm_mode_make_default(struct drm_display_mode *mode,
                           uint32_t hdisplay, uint32_t vdisplay,
                           uint32_t vrefresh)
{
    /* 简化时序计算（CVT-RB 风格） */
    uint32_t hblank = 160;        /* 水平消隐 */
    uint32_t vblank = 45;         /* 垂直消隐 */

    mode->hdisplay    = hdisplay;
    mode->vdisplay    = vdisplay;
    mode->hsync_start = hdisplay + 48;
    mode->hsync_end   = hdisplay + 80;
    mode->vsync_start = vdisplay + 3;
    mode->vsync_end   = vdisplay + 7;
    mode->htotal      = hdisplay + hblank;
    mode->vtotal      = vdisplay + vblank;

    /* 像素时钟 = htotal * vtotal * vrefresh / 1000 (kHz) */
    mode->vrefresh    = vrefresh;
    mode->clock       = (mode->htotal * mode->vtotal * vrefresh) / 1000;

    /* 生成名称 */
    /* 简单 sprintf 替代 */
    char *p = mode->name;
    int h = hdisplay, v = vdisplay;
    int i = 0;

    /* 手动数字转字符串 */
    char buf[8];
    int idx = 0;
    int tmp = h;
    do {
        buf[idx++] = '0' + (tmp % 10);
        tmp /= 10;
    } while (tmp > 0);
    while (idx > 0) p[i++] = buf[--idx];
    p[i++] = 'x';
    tmp = v;
    idx = 0;
    do {
        buf[idx++] = '0' + (tmp % 10);
        tmp /= 10;
    } while (tmp > 0);
    while (idx > 0) p[i++] = buf[--idx];
    p[i] = '\0';
}

/* ======================== mode_config 管理 ======================== */

void drm_mode_config_init(struct drm_device *dev)
{
    struct drm_mode_config *config;

    config = (struct drm_mode_config *)kmalloc(sizeof(*config), GFP_KERNEL);
    if (!config) {
        printk("[drm:kms] Failed to allocate mode_config\n");
        return;
    }

    memset(config, 0, sizeof(*config));

    INIT_LIST_HEAD(&config->crtc_list);
    INIT_LIST_HEAD(&config->plane_list);
    INIT_LIST_HEAD(&config->connector_list);
    INIT_LIST_HEAD(&config->encoder_list);
    INIT_LIST_HEAD(&config->fb_list);

    config->min_width  = 320;
    config->min_height = 200;
    config->max_width  = 4096;
    config->max_height = 4096;

    dev->mode_config = config;
}

void drm_mode_config_cleanup(struct drm_device *dev)
{
    struct drm_mode_config *config = dev->mode_config;
    struct list_head *pos, *n;

    if (!config)
        return;

    /* 释放所有 FB */
    list_for_each_safe(pos, n, &config->fb_list) {
        struct drm_framebuffer *fb = list_entry(pos, struct drm_framebuffer, list);
        drm_framebuffer_cleanup(fb);
        kfree(fb);
    }

    /* 释放所有 Connector */
    list_for_each_safe(pos, n, &config->connector_list) {
        struct drm_connector *conn = list_entry(pos, struct drm_connector, list);
        drm_connector_cleanup(conn);
    }

    /* 释放所有 Encoder */
    list_for_each_safe(pos, n, &config->encoder_list) {
        struct drm_encoder *enc = list_entry(pos, struct drm_encoder, list);
        drm_encoder_cleanup(enc);
    }

    /* 释放所有 Plane */
    list_for_each_safe(pos, n, &config->plane_list) {
        struct drm_plane *plane = list_entry(pos, struct drm_plane, list);
        drm_plane_cleanup(plane);
    }

    /* 释放所有 CRTC */
    list_for_each_safe(pos, n, &config->crtc_list) {
        struct drm_crtc *crtc = list_entry(pos, struct drm_crtc, list);
        drm_crtc_cleanup(crtc);
    }

    kfree(config);
    dev->mode_config = (void *)0;
}

/* ======================== CRTC 管理 ======================== */

int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
                  const struct drm_crtc_funcs *funcs)
{
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return DRM_ERR_NODEV;

    crtc->dev = dev;
    crtc->funcs = funcs;
    crtc->id = config->num_crtc;
    crtc->active = 0;
    crtc->fb = (void *)0;

    list_add_tail(&crtc->list, &config->crtc_list);
    config->num_crtc++;

    printk("[drm:kms] CRTC %d registered\n", crtc->id);
    return 0;
}

void drm_crtc_cleanup(struct drm_crtc *crtc)
{
    if (crtc->dev && crtc->dev->mode_config) {
        list_del(&crtc->list);
        crtc->dev->mode_config->num_crtc--;
    }
}

/* ======================== Plane 管理 ======================== */

int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
                   enum drm_plane_type type,
                   const struct drm_plane_funcs *funcs)
{
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return DRM_ERR_NODEV;

    plane->dev = dev;
    plane->type = type;
    plane->funcs = funcs;
    plane->id = config->num_plane;
    plane->fb = (void *)0;
    plane->crtc = (void *)0;

    list_add_tail(&plane->list, &config->plane_list);
    config->num_plane++;

    printk("[drm:kms] Plane %d registered (type=%d)\n", plane->id, type);
    return 0;
}

void drm_plane_cleanup(struct drm_plane *plane)
{
    if (plane->dev && plane->dev->mode_config) {
        list_del(&plane->list);
        plane->dev->mode_config->num_plane--;
    }
}

/* ======================== Connector 管理 ======================== */

int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       enum drm_connector_type type,
                       const struct drm_connector_funcs *funcs)
{
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return DRM_ERR_NODEV;

    connector->dev = dev;
    connector->type = type;
    connector->funcs = funcs;
    connector->id = config->num_connector;
    connector->status = DRM_CONNECTOR_UNKNOWN;
    connector->num_modes = 0;

    list_add_tail(&connector->list, &config->connector_list);
    config->num_connector++;

    printk("[drm:kms] Connector %d registered (type=%d)\n", connector->id, type);
    return 0;
}

void drm_connector_cleanup(struct drm_connector *connector)
{
    if (connector->dev && connector->dev->mode_config) {
        list_del(&connector->list);
        connector->dev->mode_config->num_connector--;
    }
}

/* ======================== Encoder 管理 ======================== */

int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder)
{
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return DRM_ERR_NODEV;

    encoder->dev = dev;
    encoder->id = config->num_encoder;

    list_add_tail(&encoder->list, &config->encoder_list);
    config->num_encoder++;

    printk("[drm:kms] Encoder %d registered\n", encoder->id);
    return 0;
}

void drm_encoder_cleanup(struct drm_encoder *encoder)
{
    if (encoder->dev && encoder->dev->mode_config) {
        list_del(&encoder->list);
        encoder->dev->mode_config->num_encoder--;
    }
}

/* ======================== Framebuffer 管理 ======================== */

int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb,
                         struct drm_gem_object *obj,
                         uint32_t width, uint32_t height,
                         uint32_t pitch, uint32_t format)
{
    struct drm_mode_config *config = dev->mode_config;

    if (!config || !obj)
        return DRM_ERR_INVAL;

    fb->dev = dev;
    fb->obj = obj;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->format = format;
    fb->id = config->num_fb;

    /* 增加 GEM 对象引用 */
    drm_gem_get(obj);

    list_add_tail(&fb->list, &config->fb_list);
    config->num_fb++;

    printk("[drm:kms] FB %d created: %dx%d, pitch=%d, format=0x%x\n",
           fb->id, width, height, pitch, format);
    return 0;
}

void drm_framebuffer_cleanup(struct drm_framebuffer *fb)
{
    if (fb->obj)
        drm_gem_put(fb->obj);

    if (fb->dev && fb->dev->mode_config) {
        list_del(&fb->list);
        fb->dev->mode_config->num_fb--;
    }
}

/* ======================== 对象查找 ======================== */

struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id)
{
    struct list_head *pos;
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return (void *)0;

    list_for_each(pos, &config->crtc_list) {
        struct drm_crtc *crtc = list_entry(pos, struct drm_crtc, list);
        if (crtc->id == id)
            return crtc;
    }
    return (void *)0;
}

struct drm_plane *drm_plane_find(struct drm_device *dev, uint32_t id)
{
    struct list_head *pos;
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return (void *)0;

    list_for_each(pos, &config->plane_list) {
        struct drm_plane *plane = list_entry(pos, struct drm_plane, list);
        if (plane->id == id)
            return plane;
    }
    return (void *)0;
}

struct drm_connector *drm_connector_find(struct drm_device *dev, uint32_t id)
{
    struct list_head *pos;
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return (void *)0;

    list_for_each(pos, &config->connector_list) {
        struct drm_connector *conn = list_entry(pos, struct drm_connector, list);
        if (conn->id == id)
            return conn;
    }
    return (void *)0;
}

struct drm_framebuffer *drm_framebuffer_find(struct drm_device *dev, uint32_t id)
{
    struct list_head *pos;
    struct drm_mode_config *config = dev->mode_config;

    if (!config)
        return (void *)0;

    list_for_each(pos, &config->fb_list) {
        struct drm_framebuffer *fb = list_entry(pos, struct drm_framebuffer, list);
        if (fb->id == id)
            return fb;
    }
    return (void *)0;
}

/* ======================== Mode Setting 操作 ======================== */

int drm_mode_setcrtc(struct drm_device *dev, uint32_t crtc_id,
                     uint32_t fb_id, struct drm_display_mode *mode)
{
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb = (void *)0;

    crtc = drm_crtc_find(dev, crtc_id);
    if (!crtc) {
        printk("[drm:kms] setcrtc: CRTC %d not found\n", crtc_id);
        return DRM_ERR_INVAL;
    }

    if (fb_id != 0) {
        fb = drm_framebuffer_find(dev, fb_id);
        if (!fb) {
            printk("[drm:kms] setcrtc: FB %d not found\n", fb_id);
            return DRM_ERR_INVAL;
        }
    }

    /* 如果提供了新模式，调用驱动回调设置 */
    if (mode && crtc->funcs && crtc->funcs->set_mode) {
        int ret = crtc->funcs->set_mode(crtc, mode);
        if (ret) {
            printk("[drm:kms] setcrtc: set_mode failed: %d\n", ret);
            return ret;
        }
        crtc->mode = *mode;
    }

    /* 绑定 FB */
    if (fb && crtc->funcs && crtc->funcs->set_fb) {
        int ret = crtc->funcs->set_fb(crtc, fb);
        if (ret)
            return ret;
    }
    crtc->fb = fb;

    /* 启用 CRTC */
    if (fb && crtc->funcs && crtc->funcs->enable) {
        crtc->funcs->enable(crtc);
    }
    crtc->active = (fb != (void *)0);

    printk("[drm:kms] CRTC %d: %s, FB=%d, mode=%dx%d@%d\n",
           crtc_id,
           crtc->active ? "enabled" : "disabled",
           fb_id,
           crtc->mode.hdisplay, crtc->mode.vdisplay, crtc->mode.vrefresh);

    return 0;
}

int drm_mode_page_flip(struct drm_device *dev, uint32_t crtc_id,
                       uint32_t fb_id)
{
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;

    crtc = drm_crtc_find(dev, crtc_id);
    if (!crtc) {
        printk("[drm:kms] page_flip: CRTC %d not found\n", crtc_id);
        return DRM_ERR_INVAL;
    }

    fb = drm_framebuffer_find(dev, fb_id);
    if (!fb) {
        printk("[drm:kms] page_flip: FB %d not found\n", fb_id);
        return DRM_ERR_INVAL;
    }

    if (!crtc->active) {
        printk("[drm:kms] page_flip: CRTC %d not active\n", crtc_id);
        return DRM_ERR_INVAL;
    }

    /* 调用驱动的 page_flip 回调 */
    if (crtc->funcs && crtc->funcs->page_flip) {
        int ret = crtc->funcs->page_flip(crtc, fb);
        if (ret)
            return ret;
    } else {
        /* 无 page_flip 回调，直接切换 FB */
        crtc->fb = fb;
    }

    return 0;
}
