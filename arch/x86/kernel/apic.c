#include <arch/x86/apic.h>
#include <arch/x86/cpu.h>
#include <printk.h>


int isSupportApic(){
    int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9));
}

int isSupportX2Apic(){
    int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 21));
}

void local_apic_init(){
    
    

}

void _init_apic(){

    int supportApic =  isSupportApic();
    int supportX2Apic =  isSupportX2Apic();
    if(!supportApic){
        printk("Not support apic\n");
        return;
    }  
    if(supportX2Apic){
        printk("support x2Apic\n"); 
    }else {
        printk("not support x2Apic\n");
    }

    local_apic_init();

}