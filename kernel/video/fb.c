/*
 * Framebuffer 控制台驱动
 *
 * 通过 GRUB Multiboot VBE 获取 LFB 地址，
 * 用 ioremap 映射到 vmalloc 区域，实现像素级文字渲染。
 */
#include <video/fb.h>
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/highmem.h>
#include <mm/bootmem.h>
#include <printk.h>
#include <libs/memcpy.h>
#include <stdint.h>

/* 外部字体数据 */
extern const unsigned char font_8x16[256][16];

/* 外部 multiboot 信息（setup.c 定义） */
extern multiboot_info_t *multiboot_info_base;

/* 全局 framebuffer 信息 */
struct fb_info fb;

/* 控制台光标位置（字符坐标） */
static uint32_t fb_col;
static uint32_t fb_row;
static uint32_t fb_cols;   /* 屏幕列数（字符） */
static uint32_t fb_rows;   /* 屏幕行数（字符） */

/* 前景色/背景色 (0x00RRGGBB) */
#define FG_COLOR  0x00CCCCCC   /* 浅灰白 */
#define BG_COLOR  0x00000000   /* 黑色 */

/* ========== ioremap 实现 ========== */

static unsigned long ioremap_next_vaddr = FB_IOREMAP_BASE;

void *fb_ioremap(unsigned long phys_addr, unsigned long size)
{
    unsigned long vaddr_start = ioremap_next_vaddr;
    unsigned long vaddr = vaddr_start;
    unsigned long phys = phys_addr & PAGE_MASK;
    unsigned long end = phys_addr + size;

    while (phys < end) {
        pgd_t *pgd = swapper_pg_dir + pgd_index(vaddr);

        /* 若 PDE 不存在，分配一个新页表页 */
        if (pgd_none(*pgd)) {
            /* __alloc_bootmem 返回虚拟地址（已加 PAGE_OFFSET） */
            unsigned long pte_page_va = (unsigned long)__alloc_bootmem(PAGE_SIZE, PAGE_SIZE, 0);
            memset((void *)pte_page_va, 0, PAGE_SIZE);
            /* PGD 存储物理地址 */
            set_pgd(pgd, __pgd(__pa(pte_page_va) | _KERNPG_TABLE));
        }

        /* 设置 PTE（PTE 存储物理地址） */
        pte_t *pte = pte_offset(pgd, vaddr);
        set_pte(pte, __pte(phys | pgprot_val(PAGE_KERNEL_NOCACHE)));

        phys += PAGE_SIZE;
        vaddr += PAGE_SIZE;
    }

    ioremap_next_vaddr = vaddr;
    __flush_tlb_all();

    /* 返回包含页内偏移的虚拟地址 */
    return (void *)(vaddr_start + (phys_addr & ~PAGE_MASK));
}

/* ========== 像素操作 ========== */

static inline void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    uint32_t *pixel = (uint32_t *)(fb.virt_addr + y * fb.pitch + x * (fb.bpp / 8));
    *pixel = color;
}

/* 渲染单个字符到 framebuffer */
static void fb_render_char(char c, uint32_t col, uint32_t row)
{
    unsigned char ch = (unsigned char)c;
    const unsigned char *glyph = font_8x16[ch];
    uint32_t px = col * 8;
    uint32_t py = row * 16;

    for (uint32_t y = 0; y < 16; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? FG_COLOR : BG_COLOR;
            fb_put_pixel(px + x, py + y, color);
        }
    }
}

/* 清除单行（用背景色填充） */
static void fb_clear_row(uint32_t row)
{
    uint32_t py = row * 16;
    uint8_t *line = (uint8_t *)(fb.virt_addr + py * fb.pitch);
    uint32_t i;

    for (i = 0; i < 16; i++) {
        uint32_t *p = (uint32_t *)(line + i * fb.pitch);
        for (uint32_t x = 0; x < fb.width; x++) {
            p[x] = BG_COLOR;
        }
    }
}

/* ========== 控制台接口 ========== */

void fbcon_clear(void)
{
    if (!fb.active)
        return;

    uint8_t *p = (uint8_t *)fb.virt_addr;
    uint32_t total = fb.pitch * fb.height;

    for (uint32_t i = 0; i < total; i += 4) {
        *(uint32_t *)(p + i) = BG_COLOR;
    }

    fb_col = 0;
    fb_row = 0;
}

void fbcon_scroll_up(void)
{
    if (!fb.active)
        return;

    /* 将第 1 ~ (rows-1) 行上移到第 0 ~ (rows-2) 行 */
    uint32_t row_bytes = 16 * fb.pitch;
    uint8_t *dst = (uint8_t *)fb.virt_addr;
    uint8_t *src = dst + row_bytes;
    uint32_t move_size = (fb_rows - 1) * row_bytes;

    memcpy(dst, src, move_size);

    /* 清除最后一行 */
    fb_clear_row(fb_rows - 1);

    fb_col = 0;
    fb_row = fb_rows - 1;
}

void fbcon_put_char(char c)
{
    if (!fb.active)
        return;

    if (c == '\n') {
        fb_col = 0;
        fb_row++;
        if (fb_row >= fb_rows) {
            fbcon_scroll_up();
        }
        return;
    }

    if (c == '\r') {
        fb_col = 0;
        return;
    }

    if (c == '\t') {
        /* Tab = 前进到下一个 8 列对齐位置 */
        fb_col = (fb_col + 8) & ~7;
        if (fb_col >= fb_cols) {
            fb_col = 0;
            fb_row++;
            if (fb_row >= fb_rows)
                fbcon_scroll_up();
        }
        return;
    }

    fb_render_char(c, fb_col, fb_row);
    fb_col++;
    if (fb_col >= fb_cols) {
        fb_col = 0;
        fb_row++;
        if (fb_row >= fb_rows) {
            fbcon_scroll_up();
        }
    }
}

void fbcon_put_string(const char *str)
{
    while (*str) {
        fbcon_put_char(*str);
        str++;
    }
}

/* ========== 初始化 ========== */

void fbcon_init(void)
{
    multiboot_info_t *mbi = multiboot_info_base;

    /* 检查 framebuffer 信息是否可用 */
    if (!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO)) {
        printk("[fbcon] No framebuffer info in multiboot, stay VGA text\n");
        fb.active = 0;
        return;
    }

    /* 检查 framebuffer 类型 */
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
        printk("[fbcon] Framebuffer type=%d (not RGB), stay VGA text\n",
               mbi->framebuffer_type);
        fb.active = 0;
        return;
    }

    /* 读取 framebuffer 参数 */
    fb.phys_addr = (unsigned long)mbi->framebuffer_addr;
    fb.width     = mbi->framebuffer_width;
    fb.height    = mbi->framebuffer_height;
    fb.pitch     = mbi->framebuffer_pitch;
    fb.bpp       = mbi->framebuffer_bpp;
    fb.type      = mbi->framebuffer_type;

    fb.red_pos   = mbi->framebuffer_red_field_position;
    fb.red_size  = mbi->framebuffer_red_mask_size;
    fb.green_pos = mbi->framebuffer_green_field_position;
    fb.green_size= mbi->framebuffer_green_mask_size;
    fb.blue_pos  = mbi->framebuffer_blue_field_position;
    fb.blue_size = mbi->framebuffer_blue_mask_size;

    /* 基本校验 */
    if (fb.phys_addr == 0 || fb.width == 0 || fb.height == 0 || fb.bpp != 32) {
        printk("[fbcon] Invalid fb params: addr=0x%lx %dx%d bpp=%d, stay VGA\n",
               fb.phys_addr, fb.width, fb.height, fb.bpp);
        fb.active = 0;
        return;
    }

    printk("[fbcon] FB: phys=0x%lx, %dx%d, pitch=%d, bpp=%d\n",
           fb.phys_addr, fb.width, fb.height, fb.pitch, fb.bpp);

    /* ioremap 映射 framebuffer 显存 */
    unsigned long fb_size = (unsigned long)fb.pitch * fb.height;
    void *vaddr = fb_ioremap(fb.phys_addr, fb_size);
    if (!vaddr) {
        printk("[fbcon] ioremap failed, stay VGA text\n");
        fb.active = 0;
        return;
    }
    fb.virt_addr = (unsigned long)vaddr;

    /* 计算字符网格尺寸 */
    fb_cols = fb.width / 8;
    fb_rows = fb.height / 16;
    fb_col = 0;
    fb_row = 0;
    fb.active = 1;

    printk("[fbcon] Console: %dx%d chars, virt=0x%lx\n",
           fb_cols, fb_rows, fb.virt_addr);
}
