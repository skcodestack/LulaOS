#include <tty/tty.h>
#include <video/fb.h>
#include <stdint.h>

#define TTY_HEIGHT 25
#define TTY_WIDTH 80

/* 早期 printk 缓冲区（fbcon 初始化前记录所有输出） */
#define EARLY_BUF_SIZE 8192
static char early_buf[EARLY_BUF_SIZE];
static unsigned int early_buf_len = 0;

/* 当前显示模式：默认 VGA 文本 */
static int tty_mode = TTY_MODE_VGA_TEXT;

vga_attribute *vga_buffer = (vga_attribute *)0xB8000;

vga_attribute theme_color = (VGA_COLOR_BLACK << 4 | VGA_COLOR_WHITE) << 8;

uint32_t TTY_COLOUMN = 0;
uint32_t TTY_ROW = 0;


void tty_set_buffer_base(unsigned long base){
    vga_buffer = (vga_attribute *)(0xB8000 + base);
}

void tty_set_theme(vga_attribute fg, vga_attribute bg)
{
    theme_color = (bg << 4 | fg) << 8;
}

void tty_put_char(char c)
{
    /* 记录到早期缓冲区（用于 fbcon 切换后重放） */
    if (early_buf_len < EARLY_BUF_SIZE) {
        early_buf[early_buf_len++] = c;
    }

    /* 根据当前模式分发 */
    if (tty_mode == TTY_MODE_FB) {
        fbcon_put_char(c);
        return;
    }

    /* VGA 文本模式 */
    if(c == '\n'){
        TTY_COLOUMN = 0;
        TTY_ROW++;
        if (TTY_ROW >= TTY_HEIGHT)
        {
            tty_scroll_up();
        }
        return;
    }
    *(vga_buffer + TTY_COLOUMN + TTY_ROW * TTY_WIDTH) = (theme_color | c);
    TTY_COLOUMN++;
    if (TTY_COLOUMN >= TTY_WIDTH)
    {
        TTY_COLOUMN = 0;
        TTY_ROW++;
        if (TTY_ROW >= TTY_HEIGHT)
        {
            tty_scroll_up();
        }
    }
}

void tty_put_string(char *str)
{
    while (*str != '\0')
    {
        /* code */
        tty_put_char(*str);
        str++;
    }
}

void tty_scroll_up()
{
    tty_clear();
}

void tty_clear()
{
    if (tty_mode == TTY_MODE_FB) {
        fbcon_clear();
        return;
    }
    for (uint32_t x = 0; x < TTY_WIDTH; x++)
    {
        for (uint32_t y = 0; y < TTY_HEIGHT; y++)
        {
            *(vga_buffer + x + y * TTY_WIDTH) = theme_color;
        }
    }
    TTY_COLOUMN = 0;
    TTY_ROW = 0;
}

/*
 * 切换显示模式
 * 切到 FB 模式时，自动重放早期缓冲区内容
 */
void tty_set_mode(int mode)
{
    tty_mode = mode;
    if (mode == TTY_MODE_FB) {
        tty_replay_buffer();
    }
}

/*
 * 重放早期缓冲区内容到 framebuffer
 */
void tty_replay_buffer(void)
{
    unsigned int i;
    fbcon_clear();
    for (i = 0; i < early_buf_len; i++) {
        fbcon_put_char(early_buf[i]);
    }
}