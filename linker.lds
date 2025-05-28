
ENTRY(start_)
SECTIONS
{ 
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
        *(.bss.page_table)
        *(.bss.mb_params)
        *(COMMON)
        *(.bss)  
        . = ALIGN(4096);
        pg0 = .;
    }
    
}