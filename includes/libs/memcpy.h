
#ifndef __MEMCPY_H__
#define __MEMCPY_H__

#include <stdint.h>

void *memcpy(void *dest, const void *src, uint32_t n);

void *memset(void *s, int c, uint32_t count);

#endif