/*
 * Bochs/QEMU VBE DRM 驱动
 *
 * 参考 Linux bochs-drm 驱动（drivers/gpu/drm/bochs/）：
 *   - 支持 QEMU std-vga (PCI 0x1234:0x1111) 和 Bochs VBE (BGA 寄存器)
 *   - 通过 BGA 寄存器控制分辨率和显示模式
 *   - 实现 CRTC / Plane / Connector 回调
 *
 * BGA (Bochs Graphics Array) 寄存器：
 *   索引端口: 0x01CE
 *   数据端口: 0x01CF
 *
 * VRAM 来源：
 *   - QEMU: PCI BAR0 提供 VRAM 物理地址
 *   - Bochs: 固定在 0xE0000000（ISA 设备，无 PCI BAR）
 */

#include <drm/drm_core.h>
#include <drm/drm_gem.h>
#include <drm/drm_kms.h>
#include <pci/pci.h>
#include <arch/x86/io.h>
#include <arch/x86/page.h>
#include <mm/slab.h>
#include <video/fb.h>     /* fb_ioremap */
#include <printk.h>
#include <libs/memcpy.h>

/* ======================== BGA 寄存器定义 ======================== */

#define BGA_IO_INDEX        0x01CE      /* 索引端口 */
#define BGA_IO_DATA         0x01CF      /* 数据端口 */

#define BGA_REG_ID          0x00        /* 版本 ID */
#define BGA_REG_XRES        0x01        /* 水平分辨率 */
#define BGA_REG_YRES        0x02        /* 垂直分辨率 */
#define BGA_REG_BPP         0x03        /* 每像素位数 */
#define BGA_REG_ENABLE      0x04        /* 启用/禁用 */
#define BGA_REG_BANK        0x05        /* 银行切换 */
#define BGA_REG_VIRT_WIDTH  0x06        /* 虚拟宽度 */
#define BGA_REG_VIRT_HEIGHT 0x07        /* 虚拟高度 */
#define BGA_REG_X_OFFSET    0x08        /* X 偏移 */
#define BGA_REG_Y_OFFSET    0x09        /* Y 偏移 */

/* BGA 启用标志 */
#define BGA_ENABLED         0x01
#define BGA_LINEAR_FB       0x40        /* 线性帧缓冲 */

/* ======================== 预设分辨率 ======================== */

struct bochs_mode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh;
};

static const struct bochs_mode bochs_modes[] = {
    { 640,  480,  60 },
    { 800,  600,  60 },
    { 1024, 768,  60 },
    { 1280, 1024, 60 },
    { 1600, 1200, 60 },
    { 1920, 1080, 60 },
};

#define BOCHS_MODE_COUNT    (sizeof(bochs_modes) / sizeof(bochs_modes[0]))

/* ======================== 驱动私有数据 ======================== */

struct bochs_device {
    struct drm_device *drm;             /* DRM 设备 */
    struct pci_dev *pci_dev;            /* PCI 设备（可为 NULL） */

    /* VRAM 信息 */
    unsigned long vram_phys;            /* VRAM 物理地址 */
    unsigned long vram_size;            /* VRAM 大小 */
    void *vram_virt;                    /* VRAM 虚拟地址（ioremap 后） */

    /* KMS 对象 */
    struct drm_crtc crtc;
    struct drm_plane primary_plane;
    struct drm_connector connector;
    struct drm_encoder encoder;

    /* 当前显示参数 */
    uint32_t current_width;
    uint32_t current_height;
    uint32_t current_bpp;
    uint32_t current_pitch;

    /* 双缓冲支持 */
    uint32_t virt_height;               /* 虚拟高度（2x 实际高度） */
    uint32_t current_yoffset;           /* 当前显示的 Y 偏移 */
};

/* ======================== BGA 寄存器操作 ======================== */

static void bochs_write_reg(uint16_t index, uint16_t value)
{
    outw(index, BGA_IO_INDEX);
    outw(value, BGA_IO_DATA);
}

static uint16_t bochs_read_reg(uint16_t index)
{
    outw(index, BGA_IO_INDEX);
    return inw(BGA_IO_DATA);
}

/*
 * bochs_set_resolution - 设置 BGA 分辨率
 *
 * 流程：
 *   1. 禁用显示
 *   2. 设置 X/Y 分辨率、BPP
 *   3. 设置虚拟尺寸（支持双缓冲）
 *   4. 重新启用显示
 */
static void bochs_set_resolution(struct bochs_device *bochs,
                                 uint32_t width, uint32_t height,
                                 uint32_t bpp)
{
    /* 禁用显示 */
    bochs_write_reg(BGA_REG_ENABLE, 0);

    /* 设置分辨率 */
    bochs_write_reg(BGA_REG_XRES, (uint16_t)width);
    bochs_write_reg(BGA_REG_YRES, (uint16_t)height);
    bochs_write_reg(BGA_REG_BPP, (uint16_t)bpp);

    /* 设置虚拟尺寸（双缓冲：虚拟高度 = 实际高度 * 2） */
    bochs_write_reg(BGA_REG_VIRT_WIDTH, (uint16_t)width);
    bochs_write_reg(BGA_REG_VIRT_HEIGHT, (uint16_t)(height * 2));

    /* 设置偏移为 0（显示第一帧） */
    bochs_write_reg(BGA_REG_X_OFFSET, 0);
    bochs_write_reg(BGA_REG_Y_OFFSET, 0);

    /* 启用显示（线性帧缓冲模式） */
    bochs_write_reg(BGA_REG_ENABLE, BGA_ENABLED | BGA_LINEAR_FB);

    bochs->current_width  = width;
    bochs->current_height = height;
    bochs->current_bpp    = bpp;
    bochs->current_pitch  = width * (bpp / 8);
    bochs->virt_height    = height * 2;
    bochs->current_yoffset = 0;

    printk("[bochs-drm] Set mode: %dx%d@%dbpp, pitch=%d, vram_virt=0x%lx\n",
           width, height, bpp, bochs->current_pitch,
           (unsigned long)bochs->vram_virt);
}

/* ======================== CRTC 回调 ======================== */

static int bochs_crtc_set_mode(struct drm_crtc *crtc, struct drm_display_mode *mode)
{
    struct bochs_device *bochs = (struct bochs_device *)crtc->driver_private;

    bochs_set_resolution(bochs, mode->hdisplay, mode->vdisplay, 32);
    return 0;
}

static int bochs_crtc_set_fb(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct bochs_device *bochs = (struct bochs_device *)crtc->driver_private;

    if (!fb || !fb->obj)
        return DRM_ERR_INVAL;

    /*
     * 将 FB 内容拷贝到 VRAM（简化实现）
     * 真实驱动通过 DMA 或 GPU 命令缓冲区更新 VRAM
     */
    unsigned long copy_size = fb->pitch * fb->height;
    if (copy_size > bochs->vram_size)
        copy_size = bochs->vram_size;

    if (fb->obj->vaddr && bochs->vram_virt) {
        memcpy(bochs->vram_virt, fb->obj->vaddr, copy_size);
    }

    return 0;
}

static void bochs_crtc_enable(struct drm_crtc *crtc)
{
    /* BGA 已在 set_mode 中启用 */
    (void)crtc;
}

static void bochs_crtc_disable(struct drm_crtc *crtc)
{
    struct bochs_device *bochs = (struct bochs_device *)crtc->driver_private;
    bochs_write_reg(BGA_REG_ENABLE, 0);
    (void)bochs;
}

/*
 * bochs_crtc_page_flip - 双缓冲翻页
 *
 * 原理：
 *   VRAM 虚拟高度 = 实际高度 * 2
 *   前缓冲: Y=0         ~ Y=height-1
 *   后缓冲: Y=height    ~ Y=2*height-1
 *
 *   新帧写入后缓冲 → 切换 Y_OFFSET → 显示新帧
 */
static int bochs_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb)
{
    struct bochs_device *bochs = (struct bochs_device *)crtc->driver_private;

    if (!fb || !fb->obj)
        return DRM_ERR_INVAL;

    /* 确定当前显示位置 */
    uint32_t back_yoffset;
    if (bochs->current_yoffset == 0) {
        back_yoffset = bochs->current_height;   /* 后缓冲在 height 位置 */
    } else {
        back_yoffset = 0;                        /* 后缓冲在 0 位置 */
    }

    /* 将新帧内容写入后缓冲 */
    unsigned long dst_offset = back_yoffset * bochs->current_pitch;
    unsigned long copy_size = fb->pitch * fb->height;

    if (fb->obj->vaddr && bochs->vram_virt) {
        if (dst_offset + copy_size <= bochs->vram_size) {
            memcpy((void *)((unsigned long)bochs->vram_virt + dst_offset),
                   fb->obj->vaddr, copy_size);
        }
    }

    /* 切换显示到后缓冲（更新 Y_OFFSET） */
    bochs_write_reg(BGA_REG_Y_OFFSET, (uint16_t)back_yoffset);
    bochs->current_yoffset = back_yoffset;

    /* 更新 CRTC 的 FB 指针 */
    crtc->fb = fb;

    return 0;
}

static const struct drm_crtc_funcs bochs_crtc_funcs = {
    .set_mode  = bochs_crtc_set_mode,
    .set_fb    = bochs_crtc_set_fb,
    .enable    = bochs_crtc_enable,
    .disable   = bochs_crtc_disable,
    .page_flip = bochs_crtc_page_flip,
};

/* ======================== Plane 回调 ======================== */

static int bochs_plane_update(struct drm_plane *plane,
                              struct drm_crtc *crtc,
                              struct drm_framebuffer *fb,
                              int32_t crtc_x, int32_t crtc_y,
                              uint32_t crtc_w, uint32_t crtc_h)
{
    plane->crtc = crtc;
    plane->fb = fb;
    plane->crtc_x = crtc_x;
    plane->crtc_y = crtc_y;
    plane->crtc_w = crtc_w;
    plane->crtc_h = crtc_h;

    /* Primary Plane 更新时，将 FB 内容拷贝到 VRAM */
    if (fb && fb->obj && crtc) {
        struct bochs_device *bochs = (struct bochs_device *)crtc->driver_private;
        if (bochs->vram_virt && fb->obj->vaddr) {
            unsigned long copy_size = fb->pitch * fb->height;
            if (copy_size > bochs->vram_size)
                copy_size = bochs->vram_size;
            memcpy(bochs->vram_virt, fb->obj->vaddr, copy_size);
        }
    }

    return 0;
}

static int bochs_plane_disable(struct drm_plane *plane)
{
    plane->fb = (void *)0;
    return 0;
}

static const struct drm_plane_funcs bochs_plane_funcs = {
    .update  = bochs_plane_update,
    .disable = bochs_plane_disable,
};

/* ======================== Connector 回调 ======================== */

static enum drm_connector_status bochs_connector_detect(struct drm_connector *connector)
{
    /* 虚拟显示器始终连接 */
    (void)connector;
    return DRM_CONNECTOR_CONNECTED;
}

static int bochs_connector_get_modes(struct drm_connector *connector)
{
    int i;

    /* 填充预设分辨率列表 */
    connector->num_modes = BOCHS_MODE_COUNT;
    for (i = 0; i < (int)BOCHS_MODE_COUNT; i++) {
        drm_mode_make_default(&connector->modes[i],
                              bochs_modes[i].width,
                              bochs_modes[i].height,
                              bochs_modes[i].refresh);
    }

    return connector->num_modes;
}

static const struct drm_connector_funcs bochs_connector_funcs = {
    .detect    = bochs_connector_detect,
    .get_modes = bochs_connector_get_modes,
};

/* ======================== 驱动 load/unload ======================== */

static int bochs_drm_load(struct drm_device *dev)
{
    struct bochs_device *bochs;
    struct pci_dev *pci;

    printk("[bochs-drm] Loading driver...\n");

    /* 分配驱动私有数据 */
    bochs = (struct bochs_device *)kmalloc(sizeof(*bochs), GFP_KERNEL);
    if (!bochs)
        return DRM_ERR_NOMEM;

    memset(bochs, 0, sizeof(*bochs));
    bochs->drm = dev;
    dev->dev_private = bochs;

    /* 查找 VGA 设备 */
    pci = pci_find_device(0x1234, 0x1111);  /* QEMU std-vga */
    if (!pci) {
        /* 尝试查找 Bochs VBE（ISA 设备，无 PCI） */
        printk("[bochs-drm] QEMU std-vga not found, trying Bochs VBE...\n");
        bochs->vram_phys = 0xE0000000;      /* Bochs VBE 固定地址 */
        bochs->vram_size = 16 * 1024 * 1024; /* 16MB 默认 */
    } else {
        bochs->pci_dev = pci;

        /* 从 PCI BAR0 获取 VRAM 地址 */
        bochs->vram_phys = pci->resource[0].start;
        bochs->vram_size = pci->resource[0].end - pci->resource[0].start + 1;

        printk("[bochs-drm] Found QEMU std-vga: BAR0=0x%lx, size=%luMB\n",
               bochs->vram_phys, bochs->vram_size / (1024 * 1024));

        /* 使能 PCI 设备（开启内存空间响应） */
        pci_enable_device(pci);
    }

    if (bochs->vram_phys == 0) {
        printk("[bochs-drm] ERROR: No VRAM found!\n");
        kfree(bochs);
        return DRM_ERR_NODEV;
    }

    /* ioremap VRAM */
    bochs->vram_virt = fb_ioremap(bochs->vram_phys, bochs->vram_size);
    if (!bochs->vram_virt) {
        printk("[bochs-drm] ERROR: Failed to ioremap VRAM\n");
        kfree(bochs);
        return DRM_ERR_NOMEM;
    }

    printk("[bochs-drm] VRAM: phys=0x%lx, virt=0x%lx, size=%luMB\n",
           bochs->vram_phys, (unsigned long)bochs->vram_virt,
           bochs->vram_size / (1024 * 1024));

    /* 初始化 BGA 寄存器，设置默认分辨率 */
    bochs_set_resolution(bochs, 1024, 768, 32);

    /* 创建 KMS 对象 */
    bochs->crtc.driver_private = bochs;
    drm_crtc_init(dev, &bochs->crtc, &bochs_crtc_funcs);

    bochs->primary_plane.driver_private = bochs;
    drm_plane_init(dev, &bochs->primary_plane,
                   DRM_PLANE_TYPE_PRIMARY, &bochs_plane_funcs);

    bochs->connector.driver_private = bochs;
    drm_connector_init(dev, &bochs->connector,
                       DRM_CONNECTOR_TYPE_VIRTUAL, &bochs_connector_funcs);
    bochs->connector.status = DRM_CONNECTOR_CONNECTED;
    bochs_connector_get_modes(&bochs->connector);

    drm_encoder_init(dev, &bochs->encoder);
    bochs->encoder.crtc = &bochs->crtc;
    bochs->encoder.connector = &bochs->connector;

    /* 打印当前显卡支持的分辨率列表 */
    printk("[bochs-drm] Supported resolutions (%d modes):\n", (int)BOCHS_MODE_COUNT);
    for (int i = 0; i < (int)BOCHS_MODE_COUNT; i++) {
        uint32_t pitch_bytes = bochs_modes[i].width * 4; /* 32bpp = 4 bytes/pixel */
        uint32_t vram_required = pitch_bytes * bochs_modes[i].height;
        printk("[bochs-drm]   [%d] %4dx%-4d @ %dHz  pitch=%u bytes  vram_needed=%u KB%s\n",
               i, bochs_modes[i].width, bochs_modes[i].height,
               bochs_modes[i].refresh, pitch_bytes,
               vram_required / 1024,
               vram_required > bochs->vram_size ? " [EXCEEDS VRAM!]" : "");
    }

    /* 打印 DRM 设备配置信息汇总 */
    printk("[bochs-drm] ========== DRM Device Configuration ==========\n");
    printk("[bochs-drm] Driver       : %s (%s)\n",
           dev->driver->name, dev->driver->desc);
    printk("[bochs-drm] Device type  : %s\n",
           bochs->pci_dev ? "QEMU std-vga (PCI 0x1234:0x1111)"
                          : "Bochs VBE (ISA, fixed 0xE0000000)");
    if (bochs->pci_dev) {
        printk("[bochs-drm] PCI bus      : %02x:%02x.%x\n",
               bochs->pci_dev->bus,
               bochs->pci_dev->devfn >> 3,
               bochs->pci_dev->devfn & 0x07);
    }
    printk("[bochs-drm] VRAM phys    : 0x%lx\n", bochs->vram_phys);
    printk("[bochs-drm] VRAM virt    : 0x%lx\n", (unsigned long)bochs->vram_virt);
    printk("[bochs-drm] VRAM size    : %lu MB (%lu bytes)\n",
           bochs->vram_size / (1024 * 1024), bochs->vram_size);
    printk("[bochs-drm] Mode         : %dx%d @ %dbpp, pitch=%d bytes\n",
           bochs->current_width, bochs->current_height,
           bochs->current_bpp, bochs->current_pitch);
    printk("[bochs-drm] Virt size    : %dx%d (double buffering)\n",
           bochs->current_width, bochs->virt_height);
    printk("[bochs-drm] Connector    : type=VIRTUAL, status=CONNECTED\n");
    printk("[bochs-drm] Modes count  : %d supported\n", (int)BOCHS_MODE_COUNT);
    printk("[bochs-drm] Supported modes:\n");
    for (int i = 0; i < (int)BOCHS_MODE_COUNT; i++) {
        printk("[bochs-drm]   [%d] %dx%d @ %dHz\n",
               i, bochs_modes[i].width, bochs_modes[i].height,
               bochs_modes[i].refresh);
    }
    printk("[bochs-drm] GEM handles  : next=%u\n", dev->next_handle);
    printk("[bochs-drm] Registered   : %s\n",
           dev->registered ? "yes" : "no");
    printk("[bochs-drm] ==============================================\n");

    printk("[bochs-drm] Driver loaded successfully\n");
    return 0;
}

static void bochs_drm_unload(struct drm_device *dev)
{
    struct bochs_device *bochs = (struct bochs_device *)dev->dev_private;

    if (!bochs)
        return;

    /* 禁用 BGA 显示 */
    bochs_write_reg(BGA_REG_ENABLE, 0);

    kfree(bochs);
    dev->dev_private = (void *)0;

    printk("[bochs-drm] Driver unloaded\n");
}

/* ======================== 驱动注册 ======================== */

static struct drm_driver bochs_drm_driver = {
    .name   = "bochs-drm",
    .desc   = "Bochs/QEMU VBE DRM Driver",
    .load   = bochs_drm_load,
    .unload = bochs_drm_unload,
    .ioctls = (void *)0,       /* 使用核心 ioctl 表即可 */
    .num_ioctls = 0,
};

/*
 * drm_bochs_init - Bochs DRM 驱动入口
 *
 * 在 kernel.c 中 drm_core_init() 之后调用：
 *   drm_core_init();
 *   drm_bochs_init();
 */
void drm_bochs_init(void)
{
    int ret;
    struct drm_device *dev;

    printk("[bochs-drm] Initializing...\n");

    /* 注册驱动 */
    ret = drm_register_driver(&bochs_drm_driver);
    if (ret) {
        printk("[bochs-drm] Failed to register driver: %d\n", ret);
        return;
    }

    /* 创建 DRM 设备并调用 load() */
    dev = drm_device_alloc(&bochs_drm_driver);
    if (!dev) {
        printk("[bochs-drm] Failed to allocate device\n");
        return;
    }

    ret = bochs_drm_driver.load(dev);
    if (ret) {
        printk("[bochs-drm] Failed to load driver: %d\n", ret);
        drm_device_free(dev);
        return;
    }

    dev->registered = 1;
    printk("[bochs-drm] Initialization complete\n");
}
