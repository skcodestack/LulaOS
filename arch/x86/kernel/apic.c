#include <arch/x86/apic.h>
#include <arch/x86/cpu.h>
#include <printk.h>
#include <arch/x86/highmem.h>
#include <arch/x86/msr.h>

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
    
    unsigned int  apic_id =  GET_APIC_ID(apic_read(APIC_ID));
    printk("apic id id :%d/n",apic_id);
}


void enable_apic(){
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    printk("Local APIC . %d , %x\n",MSR_IA32_APICBASE_ENABLE,l);
    if (!(l & MSR_IA32_APICBASE_ENABLE)) {
        printk("Local APIC disabled by BIOS -- reenabling.\n");
        l &= ~MSR_IA32_APICBASE_BASE;
        l |= MSR_IA32_APICBASE_ENABLE | APIC_DEFAULT_PHYS_BASE;
        wrmsr(MSR_IA32_APICBASE, l, h);
    }
}


//remapping apic base addr
void remapping_apic(){ 
    set_fixmap_nocache(FIX_APIC_BASE, APIC_DEFAULT_PHYS_BASE);
    
}

void remapping_ioapic(){

}

void _init_apic(){

    int supportApic =  isSupportApic();
    int supportX2Apic =  isSupportX2Apic();
    if(!supportApic){
        printk("Not support apic\n");
        return;
    }
    printk("support apic\n");  
    if(supportX2Apic){
        printk("support x2Apic\n"); 
    }else {
        printk("not support x2Apic\n");
    }

    remapping_apic();
    remapping_ioapic();
    
    local_apic_init();

}