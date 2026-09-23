# DRM 图形 API 层与图片显示测试

## 背景

当前 DRM 显示路径：
- **文字显示**：`fbcon_put_char()` → 直接写 `fb.virt_addr`（font_8x16 字体）
- **图片显示**：缺少对外 API，只有底层 GEM/KMS 机制，无"画点/画矩形/绘制位图"接口

`fb.virt_addr` 在 DRM 驱动加载后被 `fbcon_update_mode()` 替换为 `bochs->vram_virt`（16MB 完整映射），直接写 VRAM 即可立即显示，无需额外 page_flip。

## Task 1：创建 `includes/drm/drm_fb_helper.h`

新增图形 API 头文件，声明以下接口：

```c
/* 初始化：从全局 fb_info 获取 VRAM 指针和尺寸参数 */
void drm_fb_helper_init(void);

/* 基础图形 */
void drm_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void drm_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void drm_fb_clear(uint32_t color);

/* 位图/图片 */
void drm_fb_draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint8_t *bitmap, uint32_t fg, uint32_t bg);
void drm_fb_draw_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const uint32_t *pixels, uint32_t pitch);

/* 文字（复用 font_8x16，走 drm_fb_draw_bitmap） */
void drm_fb_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void drm_fb_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

/* 测试入口：在屏幕上绘制综合测试图案 */
void drm_fb_test_image(void);
```

## Task 2：创建 `kernel/drm/drm_fb_helper.c`

实现上述接口，核心逻辑：

**初始化**：
- 从全局 `fb`（`struct fb_info`）取 `virt_addr/width/height/pitch`，缓存为静态变量
- 若 `fb.active == 0` 则跳过所有绘制操作（防崩溃）

**画点 `put_pixel`**：
- 边界检查 → 计算偏移 `y * pitch + x * 4` → 写 `*(uint32_t *)(vram + offset) = color`

**填充矩形 `fill_rect`**：
- 对每行调用 `memset` 或循环写像素（pitch 对齐时可用 `memset32`，简化用循环）

**单色位图 `draw_bitmap`**：
- 逐字节解析 bitmap 数据，bit=1 用前景色，bit=0 用背景色

**真彩图片 `draw_image`**：
- `pixels` 是 XRGB8888 数组，逐行拷贝到 VRAM（带边界裁剪）

**文字 `draw_char/draw_string`**：
- 用 `font_8x16[ch]`（16字节，每字节 8bit = 一行 8 像素）作为 bitmap 调用 `draw_bitmap`

**测试图案 `drm_fb_test_image`**（重点）：
绘制一幅综合测试画面，包含：
1. 黑色背景清屏
2. **8 条竖色彩条纹**（白/黄/青/绿/洋红/红/蓝/黑，标准 SMPTE 色条）
3. **渐变色块**（左上角 RGB 渐变 256x64）
4. **几何图形**：填充矩形 + 菱形轮廓（用 put_pixel 画 Bresenham 线）
5. **LulaOS 文字**：用 `draw_string` 在色条下方显示 `"LulaOS DRM Image Test"` 字符串
6. 打印 printk 日志确认测试完成

## Task 3：修改 `kernel/kernel.c`

在 `drm_bochs_init()` 之后插入：

```c
#include <drm/drm_fb_helper.h>
...
drm_bochs_init();

/* DRM 图形 API 初始化（获取 VRAM 指针） */
drm_fb_helper_init();

/* 绘制测试图片，验证 DRM 图片显示服务 */
drm_fb_test_image();
```

## 文件变更汇总

| 文件 | 操作 |
|------|------|
| `includes/drm/drm_fb_helper.h` | 新建 |
| `kernel/drm/drm_fb_helper.c` | 新建 |
| `kernel/kernel.c` | 新增 3 行（include + 两个函数调用） |

无需修改 Makefile（自动扫描 `.c` 文件）。
