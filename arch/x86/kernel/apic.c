#include <arch/x86/apic.h>
#include <arch/x86/cpu.h>
#include <printk.h>
#include <arch/x86/highmem.h>
#include <arch/x86/msr.h>
#include <arch/x86/acpi.h>
#include <arch/x86/io.h>

unsigned long  ioapic_base;

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

void disable_8259_pic(){
    outb(0xff,0x21);
    outb(0xff,0xA1);
}

void local_apic_init(){
    remapping_apic();
    enable_hardware_apic();
    disable_8259_pic();
    
    unsigned int  apic_id =  GET_APIC_ID(apic_read(APIC_ID));
    printk("apic id id :%d\n",apic_id);

    apic_setup_lvts();

    unsigned int tpr =  apic_read(APIC_TPR);
    tpr &= ~APIC_TPR_MASK;
    tpr |=  APIC_TPR_VALUE(2,0); //accept above 32 vec
    apic_write(APIC_TPR,tpr);

    ///enbale apic
    unsigned int svr = apic_read(APIC_SPIV);
    svr &= ~APIC_VECTOR_MASK;
    svr |= APIC_SPIV_FOCUS_DISABLED;
    svr |= APIC_SPIV_APIC_ENABLED;
    svr |= SPURIOUS_APIC_VECTOR;
    apic_write(APIC_SPIV,svr);

}

void apic_setup_lvts(){
    unsigned val =  apic_read(APIC_LINT0);
    val |= APIC_LVT_MASKED | APIC_DM_EXTINT; 
    apic_write(APIC_LINT0,val);

    val =  apic_read(APIC_LINT1);
    val |= APIC_DM_NMI | APIC_LVT_MASKED;
    apic_write(APIC_LINT1,val);

    apic_write(APIC_ERR,ERROR_APIC_VECTOR | APIC_DM_FIXED);
}
 

void enable_hardware_apic(){
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    printk("Local APIC . %d , %x\n",MSR_IA32_APICBASE_ENABLE & l,l);
    if (!(l & MSR_IA32_APICBASE_ENABLE)) {
        printk("Local APIC disabled by BIOS -- reenabling.\n");
        l &= ~MSR_IA32_APICBASE_BASE;
        l |= MSR_IA32_APICBASE_ENABLE | APIC_DEFAULT_PHYS_BASE;
        wrmsr(MSR_IA32_APICBASE, l, h);
    }
}

  


//remapping apic base addr
void remapping_apic(){ 
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    unsigned long apic_base_addr = l & 0xFFFFF000;
    printk("Local APIC ADDR %x\n",apic_base_addr);
    if(!apic_base_addr){
        apic_base_addr = APIC_DEFAULT_PHYS_BASE;
    }
    set_fixmap_nocache(FIX_APIC_BASE, apic_base_addr); 
}

void remapping_ioapic(){
    unsigned long phy = acpi_context.ioapic.address;
    if(!phy){
        phy = IOAPIC_DEFAULT_PHYS_BASE;
    } 
    set_fixmap_nocache(FIX_IO_APIC_BASE_0,phy); 
}


void io_apic_init(){
    remapping_ioapic();

    unsigned int id = GET_IOAPIC_ID(ioapic_read(IOAPIC_ID)); 

    unsigned long version = ioapic_read(IOAPIC_VERSION);
    unsigned int version_code =  GET_IOAPIC_VERSION(version);
    unsigned int rte_count = GET_IOAPIC_RTE_COUNT(version) + 1;

    printk("ioapic id: %d-%d,version:%d,rte count:%d\n",id,acpi_context.ioapic.id,version_code,rte_count);

    /// mask all rte 
    struct ioapic_rte_entry entry = {0};
    entry.mask = 1;
    for (unsigned int i = 0; i < rte_count; i++)
    {
         ioapic_write(0x10 + 2 * i, *(unsigned int *)&entry);
         ioapic_write(0x11 + 2 * i, *(((unsigned int *)&entry) +1));
    }
    
    

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
    
    local_apic_init();
    io_apic_init();
}