#include <interrupts/interrupts.h>
#include <arch/x86/apic.h>

void _init_interrupts() 
{ 
    _init_apic();
}