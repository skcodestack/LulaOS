
ENTRY(start_)
SECTIONS
{ 
    . = 0xC0000000 + 0x100000;

    .text BLOCK(4K): AT(ADDR(.text) - 0xC0000000)
    {   
        _text = .;
        * (.multiboot)
        * (.text)
        _etext = .; 
    }

    /* trampoline 放在 .text 之后，避免将 .multiboot 推出映像前 8KB */
    .trampoline BLOCK(4K): AT(ADDR(.trampoline) - 0xC0000000)
    {
        _trampoline_start = .;
        *(.trampoline)
        _trampoline_end = .;
    }

    .init.text BLOCK(4K): AT(ADDR(.init.text) - 0xC0000000)
    {   
        _init_text = .;
        *(.text.init)
        _einit_text = .;
    }

    .exit.text BLOCK(4K): AT(ADDR(.exit.text) - 0xC0000000)
    {   
        _exit_text = .;
        *(.text.exit)
        _eexit_text = .;
    } 


    .init.setup BLOCK(4K): AT(ADDR(.init.setup) - 0xC0000000)
    {   
        _init_setup = .;
        *(.setup.init)
        _einit_setup = .;
    } 

    .init.initcall BLOCK(4K): AT(ADDR(.init.initcall) - 0xC0000000)
    {
        _init_initcall = .;
        *(.initcall.init)
        _einit_initcall = .;
    }
    
    .exit.exitcall BLOCK(4K): AT(ADDR(.exit.exitcall) - 0xC0000000)
    {
        _exit_exitcall = .;
        *(.exitcall.exit)
        _eexit_exitcall = .;
    } 

    .init.data BLOCK(4K): AT(ADDR(.init.data) - 0xC0000000)
    {
        _init_data = .;
        *(.data.init)
        _einit_data = .;
    }

    .exit.data BLOCK(4K): AT(ADDR(.exit.data) - 0xC0000000)
    {
        _exit_data = .;
        *(.data.exit)
        _eexit_data = .;
    } 

    .data.init_task BLOCK(8K) : AT(ADDR(.data.init_task) - 0xC0000000)
    {
        *(.data.init_task)
    }

    .data BLOCK(4K) : AT(ADDR(.data) - 0xC0000000)
    {			 
        _data = .;
	    *(.data) 
        _edata = .;
	} 
    .rodata BLOCK(4K): AT(ADDR(.rodata) - 0xC0000000)
    {
        _rodata = .;
        *(.rodata)
        _erodata = .;
    }
    .bss BLOCK(4K): AT(ADDR(.bss) - 0xC0000000)
    {
        _bss = .;
        *(.bss.page_table)
        . = ALIGN(4096);
        *(.bss.mb_params)
        *(COMMON)
        *(.bss)  
        . = ALIGN(4096);
        pg0 = .;
        _end = .;
    }
    .end = .;
}