/*
 * LulaOS DRM Framebuffer 图形 API 辅助层
 *
 * 在 DRM 驱动加载后，直接操作 fb.virt_addr（VRAM 虚拟地址）
 * 提供像素级图形绘制接口，供内核或用户态显示服务使用。
 *
 * 使用前提：
 *   1. drm_bochs_init() 已完成（VRAM 已 ioremap）
 *   2. 调用 drm_fb_helper_init() 缓存显示参数
 *
 * 显示链路：
 *   图形 API 调用 → 直接写 VRAM → 显示器自动扫描输出
 */

#ifndef __DRM_FB_HELPER_H__
#define __DRM_FB_HELPER_H__

#include <stdint.h>

/*
 * drm_fb_helper_init - 初始化图形 API 层
 *
 * 从全局 fb_info 结构读取 VRAM 虚拟地址、分辨率、pitch 等参数，
 * 缓存到本模块静态变量。必须在 drm_bochs_init() 之后调用。
 */
void drm_fb_helper_init(void);

/*
 * drm_fb_put_pixel - 画一个像素点
 *
 * @x, @y: 像素坐标（0-based，左上角为原点）
 * @color: XRGB8888 格式颜色值（0x00RRGGBB）
 *
 * 越界像素自动忽略，不会崩溃
 */
void drm_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/*
 * drm_fb_fill_rect - 填充矩形区域
 *
 * @x, @y: 矩形左上角坐标
 * @w, @h: 矩形宽高（像素）
 * @color: XRGB8888 格式填充颜色
 *
 * 自动裁剪超出屏幕的部分
 */
void drm_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t color);

/*
 * drm_fb_clear - 清屏（整屏填充指定颜色）
 *
 * @color: XRGB8888 格式颜色（通常传 0x00000000 黑色）
 */
void drm_fb_clear(uint32_t color);

/*
 * drm_fb_draw_bitmap - 绘制单色位图
 *
 * 每字节描述 8 个像素（高位在左），bit=1 用前景色，bit=0 用背景色。
 * 适用于字体渲染、单色图标等。
 *
 * @x, @y: 位图左上角坐标
 * @w, @h: 位图像素宽高（w 必须 <= 8 的倍数）
 * @bitmap: 位图数据，共 (w/8)*h 字节，逐行存储
 * @fg: 前景色（bit=1 时填充）
 * @bg: 背景色（bit=0 时填充）
 */
void drm_fb_draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint8_t *bitmap, uint32_t fg, uint32_t bg);

/*
 * drm_fb_draw_image - 绘制真彩色图片（XRGB8888）
 *
 * @x, @y: 图片左上角坐标
 * @w, @h: 图片像素宽高
 * @pixels: 像素数据数组，XRGB8888 格式，逐行存储
 * @pitch: 每行字节数（= w * 4，或含对齐填充）
 *
 * 自动裁剪超出屏幕的部分
 */
void drm_fb_draw_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const uint32_t *pixels, uint32_t pitch);

/*
 * drm_fb_draw_char - 绘制单个字符
 *
 * 使用内置 font_8x16 字体（8x16像素），通过 draw_bitmap 渲染。
 *
 * @c: ASCII 字符
 * @x, @y: 字符左上角像素坐标
 * @fg: 前景色
 * @bg: 背景色
 */
void drm_fb_draw_char(char c, uint32_t x, uint32_t y,
                      uint32_t fg, uint32_t bg);

/*
 * drm_fb_draw_string - 绘制字符串
 *
 * 连续渲染多个字符，字符间距无间隔（8px/字）。
 * 不换行，超出屏幕右边界的字符自动裁剪。
 *
 * @str: C 字符串（以 '\0' 结尾）
 * @x, @y: 字符串起始像素坐标
 * @fg: 前景色
 * @bg: 背景色
 */
void drm_fb_draw_string(const char *str, uint32_t x, uint32_t y,
                        uint32_t fg, uint32_t bg);

/*
 * drm_fb_test_image - 绘制综合测试图案
 *
 * 在屏幕上绘制一幅包含以下内容的测试画面：
 *   - SMPTE 标准 8 色竖条纹
 *   - RGB 渐变色块
 *   - 填充矩形 + 菱形轮廓
 *   - LulaOS 标志文字
 *
 * 用于验证 DRM 图片显示服务是否正常工作。
 */
void drm_fb_test_image(void);

#endif /* __DRM_FB_HELPER_H__ */
