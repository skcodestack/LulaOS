
/* LOAD_OFFSET=0xC0000000 */
ENTRY(phy_start_)
SECTIONS
{   

    /* multleboot default load kernel to addr  1MB */
    /* . = 0x100000
    .boot_text BLOCK(4K):
    {
        * (.multiboot)
        * (.boot_text)
    }
    .boot_bss BLOCK(4K):
    {
        * (.boot_bss)
    } */

    /* . += LOAD_OFFSET;  */
    phy_start_ = start_ - 0xC0000000;
    . = 0xC0000000 + 0x100000;
    .text BLOCK(4K): AT(ADDR(.text) - 0xC0000000)
    {   
        * (.multiboot)
        * (.text) 
    }
    .data BLOCK(4K) : AT(ADDR(.data) - 0xC0000000)
    {			 
	    *(.data) 
	} 
    .rodata BLOCK(4K): AT(ADDR(.rodata) - 0xC0000000)
    {
        *(.rodata)
    }
    .bss BLOCK(4K): AT(ADDR(.bss) - 0xC0000000)
    {
        *(COMMON)
        *(.bss)
        * (.bss.page_table)
        . = ALIGN(4);
        pg0 = .;
    }
    
}