#include <tty/tty.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <linkage.h>
#include <interrupts/interrupts.h>
void _kernel_init()
{
    _init_gdt();
    _init_idt();
    _init_interrupts();
}

asmlinkage _kernel_main(void *info_table)
{
    tty_set_theme(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    tty_put_string("Hello World!\nThis is LulaOS");
}