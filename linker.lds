
LOAD_OFFSET=0xC0000000
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
    phy_start_ = start_ - LOAD_OFFSET;
    . = LOAD_OFFSET + 0x100000;
    .text BLOCK(4K): AT(ADDR(.text) - LOAD_OFFSET)
    {   
        * (.multiboot)
        * (.text) 
    }
    .data BLOCK(4K) : AT(ADDR(.data) - LOAD_OFFSET)
    {			 
	    *(.data) 
	} 
    .rodata BLOCK(4K): AT(ADDR(.rodata) - LOAD_OFFSET)
    {
        *(.rodata)
    }
    .bss BLOCK(4K): AT(ADDR(.bss) - LOAD_OFFSET)
    {
        *(COMMON)
        *(.bss)
        * (.bss.page_table)
        . = ALIGN(4);
        pg0 = .;
    }
    
}