#include <stddef.h>
#include <libs/string.h>

size_t strnlen(const char *s, size_t count)
{
	const char *sc;

	for (sc = s; count-- && *sc != '\0'; ++sc)
		/* nothing */;
	return sc - s;
}

int strncmp(const char * cs,const char * ct,size_t count)
{
	register signed char __res = 0;

	while (count) {
		if ((__res = *cs - *ct++) != 0 || !*cs++)
			break;
		count--;
	}

	return __res;
}

void *_memcpy(void *dest, const void *src, uint32_t n)
{
	int d0, d1, d2;
	__asm__ __volatile__(
		"rep; movsl\n\t"
		"movl %4,%%ecx\n\t"
		"andl $3,%%ecx\n\t"
		"jz 1f\n\t"
		"rep ; movsb\n\t"
		"1:"
		: "=&c"(d0), "=&D"(d1), "=&S"(d2)
		: "0"(n / 4), "g"(n), "1"((long)dest), "2"((long)src)
		: "memory");

	return dest;
}

void * _memset(void *s, char c, uint32_t count){
	int d0, d1;
	__asm__ __volatile__(
		"rep ; stosb\n\t"    /// al write to es:edi
		: "=&c"(d0), "=&D"(d1)
		: "a"((unsigned char)c), "1"((long)s), "0"(count)
		: "memory");

	return s;
} 
 