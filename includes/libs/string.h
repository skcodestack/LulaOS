#ifndef __STRING_H__
#define __STRING_H__

#include <stddef.h>
#include <stdint.h>

size_t strnlen(const char * s, size_t count);

void * _memcpy(void *dest, const void *src, uint32_t n);

void * _memset(void *s, int c, uint32_t count);

#endif