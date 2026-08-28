/*
 * LulaOS DRM ioctl 命令定义
 *
 * 参考 Linux DRM ioctl 编号方案（include/uapi/drm/drm.h）：
 *   - 每个 ioctl 命令有一个编号和对应的参数结构
 *   - 由 syscall 层调用 drm_ioctl() 分发
 *
 * ioctl 编号分配：
 *   0x00 ~ 0x0F: 核心 ioctl（GET_CAP 等）
 *   0x10 ~ 0x1F: GEM ioctl
 *   0x20 ~ 0x3F: KMS ioctl
 */

#ifndef __DRM_IOCTL_H__
#define __DRM_IOCTL_H__

#include <stdint.h>

/* ======================== ioctl 编号 ======================== */

/* 核心 ioctl */
#define DRM_IOCTL_GET_CAP           0x00    /* 查询设备能力 */
#define DRM_IOCTL_VERSION           0x01    /* 查询驱动版本 */

/* GEM ioctl */
#define DRM_IOCTL_GEM_CREATE        0x10    /* 分配显存，返回 handle */
#define DRM_IOCTL_GEM_MMAP         0x11    /* 映射显存到用户虚拟地址 */
#define DRM_IOCTL_GEM_CLOSE         0x12    /* 释放显存对象 */

/* KMS ioctl */
#define DRM_IOCTL_MODE_GETRESOURCES 0x20    /* 枚举 CRTC/Plane/Connector */
#define DRM_IOCTL_MODE_ADDFB2       0x21    /* 从 GEM handle 创建 FB */
#define DRM_IOCTL_MODE_RMFB         0x22    /* 销毁 FB */
#define DRM_IOCTL_MODE_SETCRTC      0x23    /* 设置分辨率 + 绑定 FB */
#define DRM_IOCTL_MODE_PAGE_FLIP    0x24    /* 双缓冲翻页 */
#define DRM_IOCTL_MODE_GETCONNECTOR 0x25    /* 获取 Connector 信息 */

/* ======================== 参数结构 ======================== */

/*
 * DRM_IOCTL_GET_CAP 参数
 */
struct drm_get_cap {
    uint64_t capability;              /* 输入：能力 ID */
    uint64_t value;                   /* 输出：能力值 */
};

/* 能力 ID 定义 */
#define DRM_CAP_DUMB_BUFFER     0x01    /* 是否支持 dumb buffer */
#define DRM_CAP_VBLANK          0x02    /* 是否支持 VBlank 中断 */
#define DRM_CAP_PAGE_FLIP       0x03    /* 是否支持 Page Flip */

/*
 * DRM_IOCTL_VERSION 参数
 */
struct drm_version {
    int version_major;                /* 主版本号 */
    int version_minor;                /* 次版本号 */
    char name[32];                    /* 驱动名称 */
    char desc[64];                    /* 描述信息 */
};

/*
 * DRM_IOCTL_GEM_CREATE 参数
 */
struct drm_gem_create {
    uint64_t size;                    /* 输入：请求的字节数 */
    uint32_t handle;                  /* 输出：分配的 handle */
    uint32_t pad;                     /* 对齐填充 */
};

/*
 * DRM_IOCTL_GEM_MMAP 参数
 */
struct drm_gem_mmap {
    uint32_t handle;                  /* 输入：GEM handle */
    uint32_t pad;                     /* 对齐填充 */
    uint64_t offset;                  /* 输出：映射后的虚拟地址（内核选择） */
};

/*
 * DRM_IOCTL_GEM_CLOSE 参数
 */
struct drm_gem_close {
    uint32_t handle;                  /* 输入：GEM handle */
    uint32_t pad;                     /* 对齐填充 */
};

/*
 * DRM_IOCTL_MODE_GETRESOURCES 参数
 *
 * 用户传入数组指针，内核填充 ID 列表
 */
struct drm_mode_getresources {
    /* 输入：数组大小；输出：实际数量 */
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t count_fbs;

    /* 数组指针（用户分配，内核填充） */
    uint32_t *crtc_ids;
    uint32_t *connector_ids;
    uint32_t *encoder_ids;
    uint32_t *fb_ids;
};

/*
 * DRM_IOCTL_MODE_ADDFB2 参数
 */
struct drm_mode_fb_cmd2 {
    uint32_t fb_id;                   /* 输出：分配的 FB ID */
    uint32_t width;                   /* 输入：像素宽度 */
    uint32_t height;                  /* 输入：像素高度 */
    uint32_t pixel_format;            /* 输入：像素格式（DRM_FORMAT_*） */
    uint32_t handles[4];              /* 输入：GEM handle（每平面一个） */
    uint32_t pitches[4];              /* 输入：每行字节数（每平面一个） */
    uint32_t offsets[4];              /* 输入：偏移（每平面一个） */
};

/*
 * DRM_IOCTL_MODE_RMFB 参数
 */
struct drm_mode_rmfb {
    uint32_t fb_id;                   /* 输入：要删除的 FB ID */
    uint32_t pad;
};

/*
 * DRM_IOCTL_MODE_SETCRTC 参数
 */
struct drm_mode_crtc {
    uint32_t crtc_id;                 /* 输入：CRTC ID */
    uint32_t fb_id;                   /* 输入：FB ID（0 = 禁用 CRTC） */

    /* 分辨率（若 mode_valid=1，使用此模式；否则沿用当前模式） */
    uint32_t mode_valid;
    uint32_t hdisplay;
    uint32_t vdisplay;
    uint32_t vrefresh;

    /* 显示区域偏移（多显示器拼接时用） */
    uint32_t x;
    uint32_t y;

    /* Connector ID 数组（绑定到哪些 Connector） */
    uint32_t *connector_ids;
    uint32_t count_connectors;
};

/*
 * DRM_IOCTL_MODE_PAGE_FLIP 参数
 */
struct drm_mode_page_flip {
    uint32_t crtc_id;                 /* 输入：CRTC ID */
    uint32_t fb_id;                   /* 输入：新 FB ID */
    uint32_t flags;                   /* 输入：标志位 */
    uint32_t pad;
};

/* Page Flip 标志 */
#define DRM_MODE_PAGE_FLIP_EVENT    0x01    /* 完成后发送事件 */

/*
 * DRM_IOCTL_MODE_GETCONNECTOR 参数
 */
struct drm_mode_get_connector {
    uint32_t connector_id;            /* 输入：Connector ID */
    uint32_t connector_type;          /* 输出：接口类型 */
    uint32_t status;                  /* 输出：连接状态 */
    uint32_t count_modes;             /* 输出：支持的分辨率数量 */

    /* 模式数组（用户分配，内核填充） */
    struct drm_display_mode *modes;
};

#endif /* __DRM_IOCTL_H__ */
