#ifndef __FB_H__
#define __FB_H__

#include <stdint.h>

/* Framebuffer 类型常量（与 multiboot 规范一致） */
#define FB_TYPE_INDEXED     0
#define FB_TYPE_RGB         1
#define FB_TYPE_EGA_TEXT    2

struct fb_info {
    unsigned long phys_addr;    /* LFB 物理地址 */
    unsigned long virt_addr;    /* ioremap 后的虚拟地址 */
    uint32_t width;             /* 水平像素数 */
    uint32_t height;            /* 垂直像素数 */
    uint32_t pitch;             /* 每行字节数 */
    uint8_t  bpp;               /* 每像素位数 */
    uint8_t  type;              /* FB_TYPE_* */
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
    int active;                 /* 1=fb 可用, 0=回退 VGA */
};

extern struct fb_info fb;

/* Framebuffer 控制台接口 */
void fbcon_init(void);
void fbcon_put_char(char c);
void fbcon_put_string(const char *str);
void fbcon_clear(void);
void fbcon_scroll_up(void);

/* BGA 分辨率改变后同步 fb 参数（由 DRM 驱动调用）
 * new_virt: DRM 完整映射的虚拟地址（覆盖全部 VRAM），传 0 则不切换 */
void fbcon_update_mode(uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                       unsigned long new_virt);


/* ioremap: 将物理地址映射到 vmalloc 区域的虚拟地址 */
void *fb_ioremap(unsigned long phys_addr, unsigned long size);

/* TLB 刷新 */
static inline void __flush_tlb_all(void)
{
    __asm__ __volatile__(
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3\n\t"
        ::: "eax", "memory"
    );
}

/* Framebuffer 映射起始虚拟地址（vmalloc 区域内） */
#define FB_IOREMAP_BASE  0xF0000000UL

#endif
