#ifndef __IO__
#define __IO__


static inline void outb(unsigned char value, unsigned short port) {
    asm volatile("outb %0, %1" : : "a" (value), "Nd" (port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    asm volatile("inb %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

#endif
