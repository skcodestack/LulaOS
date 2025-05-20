#include <tty/tty.h>
#include <stdint.h>

#define TTY_HEIGHT 25
#define TTY_WIDTH 80

vga_attribute *vga_buffer = (vga_attribute *)0xB8000;

vga_attribute theme_color = VGA_COLOR_BLACK;

uint32_t TTY_COLOUMN = 0;
uint32_t TTY_ROW = 0;


void tty_set_theme(vga_attribute fg, vga_attribute bg)
{
    theme_color = (bg << 4 | fg) << 8;
}

void tty_put_char(char c)
{   
     if(c == '\n'){
        TTY_COLOUMN = 0;
        TTY_ROW++;
        if (TTY_ROW > TTY_HEIGHT)
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
        if (TTY_ROW > TTY_HEIGHT)
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
}

void tty_clear()
{
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