/*
 * LulaOS DRM KMS (Kernel Mode Setting) 显示控制
 *
 * 参考 Linux DRM KMS（drivers/gpu/drm/drm_crtc.c 等）实现简化版：
 *   - drm_crtc: 显示控制器，控制扫描输出时序
 *   - drm_plane: 图层，指向 Framebuffer，CRTC 从 Plane 取像素数据
 *   - drm_connector: 物理接口（HDMI/DP/VGA），检测显示器
 *   - drm_encoder: 编码器，将像素数据转换为接口信号
 *   - drm_framebuffer: 封装 GEM 对象 + 格式/尺寸信息
 *   - drm_display_mode: 分辨率/刷新率时序参数
 *
 * KMS 解决的问题：
 *   1. 统一管理分辨率设置（Mode Setting）
 *   2. 多显示器输出
 *   3. 双缓冲翻页（Page Flip）防撕裂
 */

#ifndef __DRM_KMS_H__
#define __DRM_KMS_H__

#include <stdint.h>
#include <libs/list.h>

struct drm_device;
struct drm_gem_object;

/* ======================== 常量定义 ======================== */

/* 像素格式（FourCC 编码，与 Linux DRM 一致） */
#define DRM_FORMAT_XRGB8888     0x34325258  /* XR24, 32bpp, 无 alpha */
#define DRM_FORMAT_ARGB8888     0x34325241  /* AR24, 32bpp, 有 alpha */
#define DRM_FORMAT_RGB888       0x34324742  /* RG24, 24bpp */
#define DRM_FORMAT_RGB565       0x36314752  /* RG16, 16bpp */

/* Plane 类型 */
enum drm_plane_type {
    DRM_PLANE_TYPE_PRIMARY = 0,     /* 主画面（必须有） */
    DRM_PLANE_TYPE_CURSOR  = 1,     /* 鼠标光标 */
    DRM_PLANE_TYPE_OVERLAY = 2,     /* 视频叠加层 */
};

/* Connector 状态 */
enum drm_connector_status {
    DRM_CONNECTOR_CONNECTED    = 1,
    DRM_CONNECTOR_DISCONNECTED = 2,
    DRM_CONNECTOR_UNKNOWN      = 3,
};

/* Connector 类型 */
enum drm_connector_type {
    DRM_CONNECTOR_TYPE_VGA     = 1,
    DRM_CONNECTOR_TYPE_DVI     = 2,
    DRM_CONNECTOR_TYPE_HDMI    = 5,
    DRM_CONNECTOR_TYPE_DP      = 10,
    DRM_CONNECTOR_TYPE_VIRTUAL = 15,    /* 虚拟显示器 */
};

/* 每个 Connector 支持的最大分辨率数 */
#define DRM_MAX_MODES           8

/* ======================== 显示模式 ======================== */

/*
 * drm_display_mode - 显示模式（分辨率 + 时序）
 *
 * 描述一个完整的显示时序，包括有效区域和消隐区间。
 * 参考 Linux struct drm_display_mode。
 */
struct drm_display_mode {
    /* 有效显示区域 */
    uint32_t hdisplay;              /* 水平有效像素数 */
    uint32_t vdisplay;              /* 垂直有效行数 */

    /* 同步信号时序 */
    uint32_t hsync_start;           /* 水平同步开始 */
    uint32_t hsync_end;             /* 水平同步结束 */
    uint32_t vsync_start;           /* 垂直同步开始 */
    uint32_t vsync_end;             /* 垂直同步结束 */

    /* 总像素/行数（有效 + 消隐） */
    uint32_t htotal;                /* 水平总像素数 */
    uint32_t vtotal;                /* 垂直总行数 */

    /* 时钟和刷新率 */
    uint32_t clock;                 /* 像素时钟，单位 kHz */
    uint32_t vrefresh;              /* 垂直刷新率，单位 Hz */

    /* 模式名称（如 "1920x1080"） */
    char name[16];
};

/* ======================== CRTC ======================== */

/* CRTC 操作回调 */
struct drm_crtc_funcs {
    /* 设置显示模式（分辨率/刷新率） */
    int (*set_mode)(struct drm_crtc *crtc, struct drm_display_mode *mode);
    /* 绑定 Framebuffer */
    int (*set_fb)(struct drm_crtc *crtc, struct drm_framebuffer *fb);
    /* 启用/禁用 CRTC */
    void (*enable)(struct drm_crtc *crtc);
    void (*disable)(struct drm_crtc *crtc);
    /* Page Flip：切换到新 FB，等 VBlank 后生效 */
    int (*page_flip)(struct drm_crtc *crtc, struct drm_framebuffer *fb);
};

/*
 * drm_crtc - 显示控制器
 *
 * 控制一路显示输出：从 Framebuffer 读取像素，按指定时序扫描到显示器。
 * 一个 GPU 可以有多个 CRTC（支持多显示器）。
 */
struct drm_crtc {
    struct drm_device *dev;                     /* 所属 DRM 设备 */
    struct list_head list;                      /* 链入 mode_config.crtc_list */

    struct drm_display_mode mode;               /* 当前显示模式 */
    struct drm_framebuffer *fb;                 /* 当前绑定的 FB */
    int active;                                 /* 是否启用 */

    const struct drm_crtc_funcs *funcs;         /* 驱动回调 */
    void *driver_private;                       /* 驱动私有数据 */

    uint32_t id;                                /* CRTC ID（从 0 开始） */
};

/* ======================== Plane ======================== */

/* Plane 操作回调 */
struct drm_plane_funcs {
    /* 更新 Plane 位置和 FB */
    int (*update)(struct drm_plane *plane,
                  struct drm_crtc *crtc,
                  struct drm_framebuffer *fb,
                  int32_t crtc_x, int32_t crtc_y,
                  uint32_t crtc_w, uint32_t crtc_h);
    /* 禁用 Plane */
    int (*disable)(struct drm_plane *plane);
};

/*
 * drm_plane - 图层
 *
 * 指向一块 Framebuffer，由 CRTC 合成输出。
 * Primary Plane: 主画面（全屏）
 * Cursor Plane: 鼠标光标（64x64）
 * Overlay Plane: 视频叠加（硬件合成）
 */
struct drm_plane {
    struct drm_device *dev;                     /* 所属 DRM 设备 */
    struct list_head list;                      /* 链入 mode_config.plane_list */

    struct drm_framebuffer *fb;                 /* 当前 FB */
    struct drm_crtc *crtc;                      /* 绑定到的 CRTC */

    int32_t crtc_x, crtc_y;                     /* 在 CRTC 上的位置 */
    uint32_t crtc_w, crtc_h;                    /* 在 CRTC 上的大小 */

    enum drm_plane_type type;                   /* PRIMARY / CURSOR / OVERLAY */
    const struct drm_plane_funcs *funcs;        /* 驱动回调 */
    void *driver_private;                       /* 驱动私有数据 */

    uint32_t id;                                /* Plane ID（从 0 开始） */
};

/* ======================== Encoder ======================== */

/*
 * drm_encoder - 编码器
 *
 * 将 CRTC 输出的像素数据转换为接口信号格式（HDMI TMDS / DP LVDS / VGA DAC）
 * 简化实现中通常为空操作。
 */
struct drm_encoder {
    struct drm_device *dev;                     /* 所属 DRM 设备 */
    struct list_head list;                      /* 链入 mode_config.encoder_list */

    struct drm_crtc *crtc;                      /* 绑定的 CRTC */
    struct drm_connector *connector;            /* 绑定的 Connector */

    uint32_t id;                                /* Encoder ID */
};

/* ======================== Connector ======================== */

/* Connector 操作回调 */
struct drm_connector_funcs {
    /* 检测显示器连接状态 */
    enum drm_connector_status (*detect)(struct drm_connector *connector);
    /* 读取显示器支持的分辨率列表（EDID） */
    int (*get_modes)(struct drm_connector *connector);
};

/*
 * drm_connector - 物理连接器
 *
 * 对应一个物理显示接口（HDMI/DP/VGA），检测显示器是否接入，
 * 读取 EDID 获取支持的分辨率列表。
 */
struct drm_connector {
    struct drm_device *dev;                     /* 所属 DRM 设备 */
    struct list_head list;                      /* 链入 mode_config.connector_list */

    enum drm_connector_type type;               /* VGA / HDMI / DP */
    enum drm_connector_status status;           /* CONNECTED / DISCONNECTED */

    struct drm_display_mode modes[DRM_MAX_MODES];  /* 支持的分辨率列表 */
    int num_modes;                              /* 支持的分辨率数量 */

    const struct drm_connector_funcs *funcs;    /* 驱动回调 */
    void *driver_private;                       /* 驱动私有数据 */

    uint32_t id;                                /* Connector ID */
};

/* ======================== Framebuffer ======================== */

/*
 * drm_framebuffer - 帧缓冲
 *
 * 封装一个 GEM 对象（像素数据）+ 格式/尺寸信息。
 * CRTC 从 FB 读取像素扫描到显示器。
 */
struct drm_framebuffer {
    struct drm_device *dev;                     /* 所属 DRM 设备 */
    struct list_head list;                      /* 链入 mode_config.fb_list */

    struct drm_gem_object *obj;                 /* 指向像素数据 GEM 对象 */

    uint32_t width;                             /* 像素宽度 */
    uint32_t height;                            /* 像素高度 */
    uint32_t pitch;                             /* 每行字节数 */
    uint32_t format;                            /* DRM_FORMAT_XRGB8888 等 */

    uint32_t id;                                /* FB ID（从 0 开始） */
};

/* ======================== Mode Config ======================== */

/*
 * drm_mode_config - KMS 全局配置
 *
 * 管理一个 DRM 设备的所有 KMS 对象（CRTC/Plane/Connector/Encoder/FB）
 */
struct drm_mode_config {
    struct list_head crtc_list;                 /* 所有 CRTC */
    struct list_head plane_list;                /* 所有 Plane */
    struct list_head connector_list;            /* 所有 Connector */
    struct list_head encoder_list;              /* 所有 Encoder */
    struct list_head fb_list;                   /* 所有 Framebuffer */

    int num_crtc;                               /* CRTC 数量 */
    int num_plane;                              /* Plane 数量 */
    int num_connector;                          /* Connector 数量 */
    int num_encoder;                            /* Encoder 数量 */
    int num_fb;                                 /* FB 数量 */

    /* 显示能力限制 */
    uint32_t min_width, min_height;             /* 最小分辨率 */
    uint32_t max_width, max_height;             /* 最大分辨率 */
};

/* ======================== 公共 API ======================== */

/* 初始化/清理 mode_config */
void drm_mode_config_init(struct drm_device *dev);
void drm_mode_config_cleanup(struct drm_device *dev);

/* CRTC 注册/注销 */
int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
                  const struct drm_crtc_funcs *funcs);
void drm_crtc_cleanup(struct drm_crtc *crtc);

/* Plane 注册/注销 */
int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
                   enum drm_plane_type type,
                   const struct drm_plane_funcs *funcs);
void drm_plane_cleanup(struct drm_plane *plane);

/* Connector 注册/注销 */
int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       enum drm_connector_type type,
                       const struct drm_connector_funcs *funcs);
void drm_connector_cleanup(struct drm_connector *connector);

/* Encoder 注册/注销 */
int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder);
void drm_encoder_cleanup(struct drm_encoder *encoder);

/* Framebuffer 创建/销毁 */
int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb,
                         struct drm_gem_object *obj,
                         uint32_t width, uint32_t height,
                         uint32_t pitch, uint32_t format);
void drm_framebuffer_cleanup(struct drm_framebuffer *fb);

/* Mode Setting 操作 */
int drm_mode_setcrtc(struct drm_device *dev, uint32_t crtc_id,
                     uint32_t fb_id,
                     struct drm_display_mode *mode);

/* Page Flip */
int drm_mode_page_flip(struct drm_device *dev, uint32_t crtc_id,
                       uint32_t fb_id);

/* 辅助：根据 ID 查找 KMS 对象 */
struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id);
struct drm_plane *drm_plane_find(struct drm_device *dev, uint32_t id);
struct drm_connector *drm_connector_find(struct drm_device *dev, uint32_t id);
struct drm_framebuffer *drm_framebuffer_find(struct drm_device *dev, uint32_t id);

/* 预设分辨率辅助函数 */
void drm_mode_make_default(struct drm_display_mode *mode,
                           uint32_t hdisplay, uint32_t vdisplay,
                           uint32_t vrefresh);

#endif /* __DRM_KMS_H__ */
