#ifndef __STRING_H__
#define __STRING_H__

#include <stddef.h>
#include <stdint.h>

size_t strlen(const char * s);

size_t strnlen(const char * s, size_t count);

int strncmp(const char * cs,const char * ct,size_t count);

void * _memcpy(void *dest, const void *src, uint32_t n);

void * _memset(void *s, char c, uint32_t count);

#endif