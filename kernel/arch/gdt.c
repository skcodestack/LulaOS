#include <arch/gdt.h>
 
// create segment descriptor
void _set_gdt_entry(uint32_t index,uint32_t base, uint32_t limit, uint32_t access)
{
    uint64_t descriptor; 
    descriptor = SEG_BASE_H(base) | SEG_LIMIT_H(limit) | access | SEG_BASE_M(base);
    descriptor <<= 32;
    descriptor |= SEG_LIMIT_L(limit) | SEG_BASE_L(base);
    _gdt[index] = descriptor;
}
 
void _init_gdt()
{

    //mov init to boot.S
    _set_gdt_entry(0,0, 0, 0);
    _set_gdt_entry(1,0, 0x000FFFFF, SEG_CODE_PL0);
    _set_gdt_entry(2,0, 0x000FFFFF, SEG_DATA_PL0);
    _set_gdt_entry(3,0, 0x000FFFFF, SEG_CODE_PL3); 
    _set_gdt_entry(4,0, 0x000FFFFF, SEG_DATA_PL3);   

}
