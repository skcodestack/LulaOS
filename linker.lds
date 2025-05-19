ENTRY(start_)
SECTIONS
{
    . = 0X100000; //multleboot default load kernel to addr: 1MB

    .text BLOCK(4K):
    {
        * (.multleboot)
        * (.text) 
    }
    .bss BLOCK(4K):
    {
        *(COMMON)
        *(.bss)
    }
    .data BLOCK(4K): 
    {			 
	    *(.data) 
	} 
    .rodata BLOCK(4K):
    {
        *(.rodata)
    } 

}