/*
 * LulaOS DRM Framebuffer 图形 API 实现
 *
 * 直接操作 fb.virt_addr（由 DRM 驱动通过 fbcon_update_mode 更新）
 * 提供像素级图形绘制能力，绕过 fbcon 的文字限制。
 *
 * 显示链路：
 *   drm_fb_put_pixel() / fill_rect() / draw_image()
 *       → 计算 VRAM 偏移量: y * pitch + x * 4
 *       → *(uint32_t *)(vram + offset) = color
 *       → 显示器自动扫描 VRAM 输出到屏幕
 *
 * 初始化时机：
 *   kernel_main()
 *     → drm_core_init()
 *     → drm_bochs_init()       // BGA 设置分辨率，fbcon_update_mode 更新 fb.virt_addr
 *     → drm_fb_helper_init()   // 本模块缓存 fb 参数
 *     → drm_fb_test_image()    // 测试绘制
 */

#include <drm/drm_fb_helper.h>
#include <video/fb.h>
#include <printk.h>
#include <libs/memcpy.h>

/* 外部字体数据（font_8x16.c 定义，256 字符 x 16 行 x 1 字节/行） */
extern const unsigned char font_8x16[256][16];

/* ======================== 模块静态变量 ======================== */

/*
 * 缓存 fb_info 参数，避免每次画点都通过全局结构体间接访问。
 * drm_fb_helper_init() 时从 fb 结构体拷贝到此处。
 */
static void     *fb_vram;       /* VRAM 虚拟地址（ioremap 后） */
static uint32_t  fb_width;      /* 屏幕宽度（像素） */
static uint32_t  fb_height;     /* 屏幕高度（像素） */
static uint32_t  fb_pitch;      /* 每行字节数（= width * 4） */
static int       fb_ready;      /* 1=可用，0=未初始化或不可用 */

/* ======================== 初始化 ======================== */

void drm_fb_helper_init(void)
{
    if (!fb.active) {
        printk("[drm-fb] fb not active, graphics API disabled\n");
        fb_ready = 0;
        return;
    }

    fb_vram   = (void *)fb.virt_addr;
    fb_width  = fb.width;
    fb_height = fb.height;
    fb_pitch  = fb.pitch;
    fb_ready  = 1;

    printk("[drm-fb] Helper initialized: %dx%d, pitch=%d, vram=0x%lx\n",
           fb_width, fb_height, fb_pitch, (unsigned long)fb_vram);
}

/* ======================== 基础图形 API ======================== */

/*
 * drm_fb_put_pixel - 画单个像素点
 *
 * 32bpp 下每个像素占 4 字节，VRAM 布局为线性扫描：
 *   像素 (x, y) 的字节偏移 = y * pitch + x * 4
 *
 * 边界检查防止越界写入导致内存损坏。
 */
void drm_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!fb_ready)
        return;
    if (x >= fb_width || y >= fb_height)
        return;

    uint32_t *pixel = (uint32_t *)((unsigned long)fb_vram + y * fb_pitch + x * 4);
    *pixel = color;
}

/*
 * drm_fb_fill_rect - 填充矩形
 *
 * 对每行执行像素级填充，逐列写 4 字节。
 * 矩形超出屏幕部分通过裁剪 x2/y2 处理。
 */
void drm_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t color)
{
    if (!fb_ready)
        return;

    /* 裁剪到屏幕边界 */
    uint32_t x2 = x + w;
    uint32_t y2 = y + h;
    if (x >= fb_width || y >= fb_height)
        return;
    if (x2 > fb_width)  x2 = fb_width;
    if (y2 > fb_height) y2 = fb_height;

    for (uint32_t row = y; row < y2; row++) {
        uint32_t *line = (uint32_t *)((unsigned long)fb_vram +
                                       row * fb_pitch + x * 4);
        for (uint32_t col = x; col < x2; col++) {
            *line++ = color;
        }
    }
}

/*
 * drm_fb_clear - 清屏
 *
 * 将整块 VRAM（height * pitch 字节）全部填充为同一颜色。
 * 4 字节对齐写，效率较高。
 */
void drm_fb_clear(uint32_t color)
{
    if (!fb_ready)
        return;

    uint32_t total_pixels = fb_height * (fb_pitch / 4);
    uint32_t *p = (uint32_t *)fb_vram;

    for (uint32_t i = 0; i < total_pixels; i++) {
        p[i] = color;
    }
}

/* ======================== 位图 / 图片 API ======================== */

/*
 * drm_fb_draw_bitmap - 绘制单色位图（1bpp → 32bpp 展开）
 *
 * 位图数据格式（每字节描述 8 个水平像素）：
 *   bit7 bit6 bit5 ... bit0
 *   ← 先扫描方向（MSB 在左）
 *
 * bit=1 → 前景色 fg
 * bit=0 → 背景色 bg
 *
 * 字体渲染典型用法：font_8x16[ch] 是 16 字节，对应 8x16 像素的单色字形。
 */
void drm_fb_draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint8_t *bitmap, uint32_t fg, uint32_t bg)
{
    if (!fb_ready || !bitmap)
        return;

    uint32_t bytes_per_row = (w + 7) / 8;   /* 每行字节数（向上取整） */

    for (uint32_t row = 0; row < h; row++) {
        uint32_t py = y + row;
        if (py >= fb_height)
            break;

        const uint8_t *src = bitmap + row * bytes_per_row;

        for (uint32_t col = 0; col < w; col++) {
            uint32_t px = x + col;
            if (px >= fb_width)
                break;

            /* 取第 col 位：字节索引 = col/8，位索引 = 7 - (col%8) */
            uint8_t byte = src[col / 8];
            uint8_t bit  = (byte >> (7 - (col % 8))) & 1;

            uint32_t *pixel = (uint32_t *)((unsigned long)fb_vram +
                                            py * fb_pitch + px * 4);
            *pixel = bit ? fg : bg;
        }
    }
}

/*
 * drm_fb_draw_image - 绘制 XRGB8888 真彩图片
 *
 * pixels 是 uint32_t 数组，pitch 是每行字节数（通常 = w * 4）。
 * 支持裁剪：超出屏幕的部分不写入。
 *
 * 适合显示照片、图标、渐变图等任意真彩内容。
 */
void drm_fb_draw_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const uint32_t *pixels, uint32_t img_pitch)
{
    if (!fb_ready || !pixels)
        return;

    /* 计算裁剪范围 */
    uint32_t x_end = (x + w > fb_width)  ? fb_width  : (x + w);
    uint32_t y_end = (y + h > fb_height) ? fb_height : (y + h);
    uint32_t copy_w = x_end - x;   /* 实际拷贝的像素列数 */

    if (x >= fb_width || y >= fb_height || copy_w == 0)
        return;

    for (uint32_t row = y; row < y_end; row++) {
        uint32_t src_row = row - y;
        const uint32_t *src = (const uint32_t *)((unsigned long)pixels +
                                                   src_row * img_pitch);
        uint32_t *dst = (uint32_t *)((unsigned long)fb_vram +
                                      row * fb_pitch + x * 4);

        /* 逐像素拷贝（可用 memcpy 优化，但保持清晰性） */
        for (uint32_t col = 0; col < copy_w; col++) {
            dst[col] = src[col];
        }
    }
}

/* ======================== 文字渲染 ======================== */

/*
 * drm_fb_draw_char - 绘制单个 ASCII 字符
 *
 * font_8x16 格式：每字符 16 字节，每字节描述一行 8 个像素（MSB 在左）。
 * 直接委托给 draw_bitmap 渲染（8 像素宽，16 像素高）。
 */
void drm_fb_draw_char(char c, uint32_t x, uint32_t y,
                      uint32_t fg, uint32_t bg)
{
    unsigned char ch = (unsigned char)c;
    drm_fb_draw_bitmap(x, y, 8, 16, font_8x16[ch], fg, bg);
}

/*
 * drm_fb_draw_string - 绘制字符串
 *
 * 字符紧密排列，每字符 8 像素宽，不换行。
 * 超出屏幕右边界的字符自动被 draw_bitmap 裁剪。
 */
void drm_fb_draw_string(const char *str, uint32_t x, uint32_t y,
                        uint32_t fg, uint32_t bg)
{
    if (!str)
        return;

    uint32_t cx = x;
    while (*str) {
        drm_fb_draw_char(*str, cx, y, fg, bg);
        cx += 8;
        str++;
    }
}

/* ======================== 内部辅助：直线绘制 ======================== */

/*
 * draw_line - Bresenham 直线算法
 *
 * 在 (x0,y0) 和 (x1,y1) 之间画一条直线。
 * 支持任意方向（水平/垂直/斜线），用 put_pixel 逐点绘制。
 */
static void draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                      uint32_t color)
{
    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    int cx = (int)x0, cy = (int)y0;

    for (;;) {
        drm_fb_put_pixel((uint32_t)cx, (uint32_t)cy, color);

        if (cx == (int)x1 && cy == (int)y1)
            break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx  += sx;
        }
        if (e2 < dx) {
            err += dx;
            cy  += sy;
        }
    }
}

/* ======================== 测试图案 ======================== */

/*
 * SMPTE 标准色条颜色（8 条，从左到右）
 * 白/黄/青/绿/洋红/红/蓝/黑
 */
static const uint32_t smpte_colors[8] = {
    0x00FFFFFF,   /* 白   */
    0x00FFFF00,   /* 黄   */
    0x0000FFFF,   /* 青   */
    0x0000FF00,   /* 绿   */
    0x00FF00FF,   /* 洋红 */
    0x00FF0000,   /* 红   */
    0x000000FF,   /* 蓝   */
    0x00000000,   /* 黑   */
};

/*
 * drm_fb_test_image - 绘制综合测试图案
 *
 * 屏幕布局（以 1024x768 为例）：
 *
 *   [0,0]
 *    ┌─────────────────────────────────────┐
 *    │  RGB 渐变色块（256 x 64）           │  y=0..63
 *    ├─────────────────────────────────────┤
 *    │  SMPTE 8 色彩条（满宽 x 300 行）    │  y=64..363
 *    ├─────────────────────────────────────┤
 *    │  填充矩形（红/绿/蓝三块）           │  y=380..460
 *    ├─────────────────────────────────────┤
 *    │  菱形轮廓（中心 x=512,y=580）       │  y=500..660
 *    ├─────────────────────────────────────┤
 *    │  "LulaOS DRM Image Test" 文字       │  y=700
 *    └─────────────────────────────────────┘
 */
void drm_fb_test_image(void)
{
    if (!fb_ready) {
        printk("[drm-fb] test_image: skipped (not initialized)\n");
        return;
    }

    printk("[drm-fb] Drawing test image on %dx%d screen...\n",
           fb_width, fb_height);

    /* Step 1: 清屏（深蓝底色，区别于纯黑以便观察边界） */
    drm_fb_clear(0x00101030);

    /* =========================================================
     * Step 2: RGB 渐变色块（屏幕顶部，256x64）
     *
     * R 从 0→255 横向渐变（上排）
     * G 从 0→255 横向渐变（中排）
     * B 从 0→255 横向渐变（下排）
     * ========================================================= */
    {
        uint32_t gx = 0;
        uint32_t gy = 0;
        uint32_t gw = 256;
        uint32_t gh = 64;

        /* 居中放置渐变块 */
        if (fb_width > gw)
            gx = (fb_width - gw) / 2;

        for (uint32_t row = 0; row < gh && (gy + row) < fb_height; row++) {
            for (uint32_t col = 0; col < gw && (gx + col) < fb_width; col++) {
                uint32_t r, g, b;

                if (row < gh / 3) {
                    /* 上排：R 渐变 */
                    r = col; g = 0; b = 0;
                } else if (row < (gh * 2) / 3) {
                    /* 中排：G 渐变 */
                    r = 0; g = col; b = 0;
                } else {
                    /* 下排：B 渐变 */
                    r = 0; g = 0; b = col;
                }

                uint32_t color = (r << 16) | (g << 8) | b;
                drm_fb_put_pixel(gx + col, gy + row, color);
            }
        }

        printk("[drm-fb]   [1/4] RGB gradient block drawn at (%u,%u) %ux%u\n",
               gx, gy, gw, gh);
    }

    /* =========================================================
     * Step 3: SMPTE 8 色彩条（紧接渐变块下方，高度 = 屏幕高度 40%）
     *
     * 每条宽度 = screen_width / 8
     * ========================================================= */
    {
        uint32_t bar_top = 64;
        uint32_t bar_height = fb_height * 2 / 5;   /* 约 40% 屏高 */
        uint32_t bar_width  = fb_width / 8;

        for (uint32_t i = 0; i < 8; i++) {
            uint32_t x = i * bar_width;
            drm_fb_fill_rect(x, bar_top, bar_width, bar_height, smpte_colors[i]);
        }

        printk("[drm-fb]   [2/4] SMPTE color bars: %u bars, each %ux%u at y=%u\n",
               8, bar_width, bar_height, bar_top);
    }

    /* =========================================================
     * Step 4: 三个填充矩形（红/绿/蓝，水平排列，居中）
     *
     * 矩形大小: 100x60，间距 20 像素
     * ========================================================= */
    {
        uint32_t rect_w = 100;
        uint32_t rect_h = 60;
        uint32_t gap    = 20;
        uint32_t total_w = rect_w * 3 + gap * 2;
        uint32_t rx = (fb_width > total_w) ? (fb_width - total_w) / 2 : 0;
        uint32_t ry;

        /* 计算 ry：渐变块(64) + 色条(40%屏高) + 间距 */
        ry = 64 + (fb_height * 2 / 5) + 20;

        /* 红色矩形 */
        drm_fb_fill_rect(rx, ry, rect_w, rect_h, 0x00FF4444);
        /* 绿色矩形 */
        drm_fb_fill_rect(rx + rect_w + gap, ry, rect_w, rect_h, 0x0044FF44);
        /* 蓝色矩形 */
        drm_fb_fill_rect(rx + (rect_w + gap) * 2, ry, rect_w, rect_h, 0x004444FF);

        printk("[drm-fb]   [3/4] Filled rectangles at y=%u (R/G/B)\n", ry);
    }

    /* =========================================================
     * Step 5: 菱形轮廓（Bresenham 直线，4 条边）
     *
     * 菱形中心 (cx, cy)，水平/垂直半径各 80 像素
     * ========================================================= */
    {
        uint32_t cx = fb_width / 2;
        /* 菱形中心 Y：矩形下方再 +40 */
        uint32_t cy = 64 + (fb_height * 2 / 5) + 20 + 60 + 60;
        uint32_t rh = 80;   /* 垂直半径 */
        uint32_t rw = 120;  /* 水平半径 */

        /* 确保菱形不超出屏幕 */
        if (cy + rh >= fb_height)
            cy = fb_height - rh - 2;
        if (cx + rw >= fb_width)
            cx = fb_width - rw - 2;

        uint32_t color_diamond = 0x00FFAA00;   /* 橙色 */

        /* 4 条边：上→右，右→下，下→左，左→上 */
        draw_line(cx, cy - rh, cx + rw, cy,     color_diamond);   /* 上→右 */
        draw_line(cx + rw, cy, cx, cy + rh,      color_diamond);   /* 右→下 */
        draw_line(cx, cy + rh, cx - rw, cy,      color_diamond);   /* 下→左 */
        draw_line(cx - rw, cy, cx, cy - rh,      color_diamond);   /* 左→上 */

        printk("[drm-fb]   [3.5/4] Diamond outline at center (%u,%u) rw=%u rh=%u\n",
               cx, cy, rw, rh);
    }

    /* =========================================================
     * Step 6: LulaOS 文字（屏幕底部，居中）
     *
     * 字符串长 22 字符 × 8 像素 = 176 像素
     * ========================================================= */
    {
        const char *text = "LulaOS DRM Image Test";
        uint32_t text_w = 21 * 8;   /* 21 个字符（含空格） */
        uint32_t tx = (fb_width > text_w) ? (fb_width - text_w) / 2 : 0;
        uint32_t ty = fb_height - 40;   /* 距底边 40 像素 */

        /* 白色文字，黑色背景（形成清晰的文字框） */
        drm_fb_draw_string(text, tx, ty, 0x00FFFFFF, 0x00000000);

        /* 在文字下方加一行小字 */
        const char *sub = "Framebuffer Graphics API OK";
        uint32_t sub_w = 27 * 8;
        uint32_t sx = (fb_width > sub_w) ? (fb_width - sub_w) / 2 : 0;
        drm_fb_draw_string(sub, sx, ty + 18, 0x00AAAAAA, 0x00000000);

        printk("[drm-fb]   [4/4] Text drawn at y=%u: '%s'\n", ty, text);
    }

    printk("[drm-fb] Test image complete. DRM image display service verified.\n");
}
