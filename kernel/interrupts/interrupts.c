#include <interrupts/interrupts.h>


extern void _init_apic();

void _init_interrupts() 
{ 
    //init apic 
    _init_apic();
}