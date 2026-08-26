# LulaOS Framebuffer 图形显示切换

## 整体架构

```
printk()
  |
  v
tty_put_string() -- detects mode
  |                    |
  v                    v
VGA text mode     Framebuffer mode
(0xB8000)         (LFB via ioremap)
fallback           primary
```

核心流程：GRUB 设置图形模式 -> 内核读取 multiboot framebuffer 信息 -> ioremap 映射显存 -> fbcon 渲染文字 -> 重放早期 printk 缓冲区。

---

## Task 1: 扩展 Multiboot Header 请求图形模式

**文件**: `arch/x86/boot/boot.S`

- 修改 `MB_FLAGS` 添加 `MULTIBOOT_VIDEO_MODE` 标志
- 在 multiboot header 尾部追加 video 模式字段（mode_type/width/height/depth）
- 更新 checksum 计算

```asm
#define MB_FLAGS (MULTIBOOT_MEMORY_INFO | MULTIBOOT_PAGE_ALIGN | MULTIBOOT_VIDEO_MODE)
```

Header 追加（紧跟 checksum 之后）:
```asm
.long 0          /* header_addr (unused without AOUT_KLUDGE) */
.long 0          /* load_addr */
.long 0          /* load_end_addr */
.long 0          /* bss_end_addr */
.long 0          /* entry_addr */
.long 0          /* mode_type = 0 (linear graphics) */
.long 1024       /* width */
.long 768        /* height */
.long 32         /* depth */
```

---

## Task 2: 更新 GRUB 配置

**文件**: `GRUB_TEMPLATE`

在 menuentry 中添加 insmod 和 gfxpayload，确保 GRUB 进入图形模式:

```
insmod all_video
set gfxpayload=1024x768x32
menuentry "$_OS_NAME" {
    multiboot /boot/$_OS_NAME.bin
}
```

---

## Task 3: 创建 Framebuffer 驱动核心

**新文件**: `includes/video/fb.h`

定义 framebuffer 信息和 fbcon 接口:

```c
struct fb_info {
    unsigned long phys_addr;
    unsigned long virt_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  type;      /* RGB=1, EGA_TEXT=2 */
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
    int active;         /* 1=fb可用, 0=回退VGA */
};

extern struct fb_info fb;
void fbcon_init(void);
void fbcon_put_char(char c);
void fbcon_put_string(const char *str);
void fbcon_clear(void);
void fbcon_scroll_up(void);
```

**新文件**: `kernel/video/fb.c`

- `fbcon_init()`: 从 `multiboot_info_base` 读取 framebuffer 字段，校验 type==RGB，调用 `fb_ioremap()` 映射显存
- `fbcon_put_char()`: 将字符用 8x16 点阵字体渲染到 framebuffer
- `fbcon_clear()`: memset 显存为黑色，重置光标
- `fbcon_scroll_up()`: memmove 上移 + 清底部行
- 内部 `fb_put_pixel(x, y, color)` 写单个像素

显存地址通过 ioremap 映射到虚拟地址 0xF0000000 区域。

---

## Task 4: 实现 ioremap

**文件**: `includes/video/fb.h`（声明）+ `kernel/video/fb.c`（实现）

```c
void *fb_ioremap(unsigned long phys_addr, unsigned long size);
```

实现逻辑:
1. 从 VMALLOC_START (0xF8000000) 开始分配虚拟地址
2. 对每个 4K 页：在 `swapper_pg_dir` 中找到/创建 PDE，创建 PTE
3. PTE = phys | PAGE_KERNEL_NOCACHE（帧缓冲禁用缓存）
4. 分配 PTE 页使用 `alloc_bootmem`（在 bootmem 阶段调用）
5. 最后 `__flush_tlb_all()` 刷新 TLB

注：fb_ioremap 在 `paging_init()` 之后调用，此时 swapper_pg_dir 已有 vmalloc 区域的框架。

---

## Task 5: 内嵌 8x16 VGA 点阵字体

**新文件**: `kernel/video/font_8x16.c`

标准 VGA 8x16 点阵字体数据（256 字符 x 16 字节/字符 = 4096 字节）:
```c
const unsigned char font_8x16[256][16] = { ... };
```

每个字符 8 像素宽 x 16 像素高，每行 1 字节（8 bits = 8 pixels，MSB 在左）。

---

## Task 6: 修改 TTY 层支持双模

**文件**: `includes/tty/tty.h` + `kernel/tty/tty.c`

tty.h 添加:
```c
#define TTY_MODE_VGA_TEXT  0
#define TTY_MODE_FB        1
void tty_set_mode(int mode);
void tty_replay_buffer(void);
```

tty.c 修改:
- 添加 `static int tty_mode = TTY_MODE_VGA_TEXT;`
- 添加环形缓冲区（约 4KB），记录所有输出字符
- `tty_put_char()` 根据 `tty_mode` 分发到 VGA 或 fbcon
- `tty_set_mode()` 切换模式，若切到 FB 则调用 `tty_replay_buffer()` 重放
- `tty_replay_buffer()` 遍历缓冲区重写到 fbcon

---

## Task 7: 早期 printk 缓冲

**文件**: `kernel/tty/tty.c`

在 `tty_put_char()` 中，无论哪种模式都同时将字符追加到 `early_buf[]`（固定大小 4096 字节的环形缓冲）。当 `tty_set_mode(TTY_MODE_FB)` 被调用时:
1. `fbcon_clear()`
2. 遍历 `early_buf` 调用 `fbcon_put_char()` 重放

这样内核启动早期的 printk 消息不会丢失。

---

## Task 8: 集成到启动流程

**文件**: `arch/x86/kernel/setup.c`

在 `setup_arch()` 中，`paging_init()` 之后:
```c
void __init setup_arch(){
    copy_multiboot_info();
    setup_memery();           // 含 paging_init()

    // 初始化 framebuffer 控制台（需要页表就绪）
    fbcon_init();

    acpi_tables_init();
    tty_set_buffer_base(PAGE_OFFSET);
}
```

`fbcon_init()` 流程:
1. 从 `multiboot_info_base` 读取 framebuffer 信息
2. 检查 `MULTIBOOT_INFO_FRAMEBUFFER_INFO` 标志位
3. 检查 `framebuffer_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB`
4. 调用 `fb_ioremap()` 映射显存
5. 若成功，调用 `tty_set_mode(TTY_MODE_FB)` 切换到图形模式

**文件**: `kernel/Makefile` 或确认自动编译（当前 Makefile 使用 `find -name "*.[cS]"`，新 .c 文件会自动编译）

---

## Task 9: 编译验证

1. `make all` 确认编译通过
2. 检查新增文件是否被 Makefile 的 `find` 自动包含
3. 确认链接无未定义符号

---

## 文件变更总结

| 文件 | 操作 |
|------|------|
| `arch/x86/boot/boot.S` | 修改 MB_FLAGS，追加 video 字段 |
| `GRUB_TEMPLATE` | 添加 insmod + gfxpayload |
| `includes/video/fb.h` | 新建，fb_info 结构和 fbcon 接口 |
| `kernel/video/fb.c` | 新建，fbcon 实现 + ioremap |
| `kernel/video/font_8x16.c` | 新建，8x16 点阵字体数据 |
| `includes/tty/tty.h` | 添加 tty_set_mode / tty_replay_buffer |
| `kernel/tty/tty.c` | 双模分发 + 早期缓冲区 + 重放 |
| `arch/x86/kernel/setup.c` | 在 paging_init 后调用 fbcon_init |
