#ifndef __GDT_H__
#define __GDT_H__

#include <stdint.h>

#define SEG_TYPE(x) (x << 8)      // Segemnt type (readable/writable, expand down, conforming/expanding)
#define SEG_DESCTYPE(x) (x << 12) // Descriptor type (0 for system, 1 for code/data)
#define SEG_DPL(x) (x << 13)      // Descriptor Privilege Level (0-3)
#define SEG_PRES(x) (x << 14)     // Segement Present
#define SEG_AVL(x) (x << 20)      // Available for use by system software
#define SEG_LONG(x) (x << 21)     // 64-bit code segement(IA-32e mode only) long mode
#define SEG_DB(x) (x << 22)       // 32-bit mode (0 for 16-bit, 1 for 32-bit)
#define SEG_GRAN(x) (x << 23)     // Granularity (byte/page, 0 for 1B, 1 for 4B)

#define SEG_LIMIT_L(x) (x & 0x0000FFFF)
#define SEG_LIMIT_H(x) (x & 0x000F0000)
#define SEG_BASE_L(x) ((x & 0x0000FFFF) << 16)
#define SEG_BASE_M(x) ((x & 0x00FF0000) >> 16)
#define SEG_BASE_H(x) (x & 0xFF000000)

#define SEG_DATA_RD 0x00        // Read-Only
#define SEG_DATA_RDA 0x01       // Read-Only, accessed
#define SEG_DATA_RDWR 0x02      // Read/Write
#define SEG_DATA_RDWRA 0x03     // Read/Write, accessed
#define SEG_DATA_RDEXPD 0x04    // Read-Only, expand-down
#define SEG_DATA_RDEXPDA 0x05   // Read-Only, expand-down, accessed
#define SEG_DATA_RDWREXPD 0x06  // Read/Write, expand-down
#define SEG_DATA_RDWREXPDA 0x07 // Read/Write, expand-down, accessed
#define SEG_CODE_EX 0x08        // Execute-Only
#define SEG_CODE_EXA 0x09       // Execute-Only, accessed
#define SEG_CODE_EXRD 0x0A      // Execute/Read
#define SEG_CODE_EXRDA 0x0B     // Execute/Read, accessed
#define SEG_CODE_EXC 0x0C       // Execute-Only, conforming
#define SEG_CODE_EXCA 0x0D      // Execute-Only, conforming, accessed
#define SEG_CODE_EXRDC 0x0E     // Execute/Read, conforming
#define SEG_CODE_EXRDCA 0x0F    // Execute/Read, conforming, accessed

#define SEG_CODE_PL0 SEG_TYPE(SEG_CODE_EXRD) | SEG_DESCTYPE(1) | SEG_DPL(0) | SEG_PRES(1) | SEG_AVL(0) | SEG_LONG(0) | SEG_DB(1) | SEG_GRAN(1)
#define SEG_DATA_PL0 SEG_TYPE(SEG_DATA_RDWR) | SEG_DESCTYPE(1) | SEG_DPL(0) | SEG_PRES(1) | SEG_AVL(0) | SEG_LONG(0) | SEG_DB(1) | SEG_GRAN(1)
#define SEG_CODE_PL3 SEG_TYPE(SEG_CODE_EXRD) | SEG_DESCTYPE(1) | SEG_DPL(3) | SEG_PRES(1) | SEG_AVL(0) | SEG_LONG(0) | SEG_DB(1) | SEG_GRAN(1)
#define SEG_DATA_PL3 SEG_TYPE(SEG_DATA_RDWR) | SEG_DESCTYPE(1) | SEG_DPL(3) | SEG_PRES(1) | SEG_AVL(0) | SEG_LONG(0) | SEG_DB(1) | SEG_GRAN(1)

struct desc_struct 
{
	unsigned char x[8];
};

//boot.S 中的.globel _gdt
extern struct desc_struct _gdt[];

// initialize gdt
void _init_gdt();

#endif