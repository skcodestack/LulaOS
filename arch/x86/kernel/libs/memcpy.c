#include <libs/memcpy.h>
#include <libs/string.h>
void *memcpy(void *dest, const void *src, uint32_t n){
    return _memcpy(dest, src, n);
}

void *memset(void *s, int c, uint32_t count){
    return _memset(s, c, count);
}