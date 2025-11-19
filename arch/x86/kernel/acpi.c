#include <arch/x86/acpi.h>
#include <stddef.h>
#include <printk.h>
#include <arch/x86/page.h>
#include <libs/string.h>
#include <libs/memcpy.h>
#include <arch/x86/highmem.h>

acpi_table_context acpi_context= {0};

typedef int (*acpi_table_handler) (unsigned long , unsigned long);

acpi_table_handler handles[ACPI_TABLE_COUNT] = {NULL};


static char * __init _rang_mapping (unsigned long addr,unsigned long size){
    unsigned long map_phys = addr;
    unsigned long map_size;
    int idx = FIX_ACPI_BEGIN;

    unsigned long offset =  addr & (PAGE_SIZE -1);
    map_size = PAGE_SIZE - offset;

    set_fixmap(idx,addr);
    unsigned long base = fix_to_virt(idx);

    if(map_size >= size){
        return ((char * )base + offset);
    }
    
    while (map_size < size)
    {
        if(idx++ == FIX_ACPI_END){
            return 0;
        }
        map_phys = map_phys + PAGE_SIZE;
        set_fixmap(idx,map_phys);
        map_size = map_size + PAGE_SIZE;
    }
    
    return ((char * )base + offset);
}

static unsigned char __init acpi_checksum(void *buffer, int length)
{
	int i;
	unsigned char *bytebuffer;
	unsigned char sum = 0;

	if (!buffer || length <= 0)
		return 0;

	bytebuffer = (unsigned char *) buffer;

	for (i = 0; i < length; i++)
		sum += *(bytebuffer++);

	return sum;
}


static struct acpi_table_rsdp* scan_memeory_for_rsdp(void * addr,unsigned long size){
    unsigned long offset = 0;

    while(offset < size){
        if(strncmp(addr,RSDP_SIG,sizeof(RSDP_SIG)-1) == 0 && acpi_checksum(addr, RSDP_CHECKSUM_LENGTH) == 0){ 

            return addr;
        }
        offset += RSDP_SCAN_STEP;
		addr += RSDP_SCAN_STEP;
    }
    return NULL;
}

static struct acpi_table_rsdp* find_rsdp_entry()
{
    struct acpi_table_rsdp * rsdp;
    /// search rsdp from 0x0000 - 0x400
    rsdp = scan_memeory_for_rsdp(__va(LO_RSDP_WINDOW_BASE),LO_RSDP_WINDOW_SIZE);
    if(rsdp){
        return rsdp;
    } 
    /// search rsdp from E0000h-F0000h
    rsdp = scan_memeory_for_rsdp(__va(HI_RSDP_WINDOW_BASE),HI_RSDP_WINDOW_SIZE);
    if(rsdp){
        return rsdp;
    } 

    return NULL;
};


static void __init apic_parse_lapic(void * addr){
    struct acpi_table_lapic *entry = NULL;
    entry = (struct acpi_table_lapic *)addr;

    memcpy(&acpi_context.lapic,entry,sizeof(struct acpi_table_lapic));
    printk("apic struct is %d,%d\n",acpi_context.lapic.id,acpi_context.lapic.acpi_id);
}

static void __init apic_parse_ioapic(void * addr){
    struct acpi_table_ioapic *entry = NULL;
    entry = (struct acpi_table_ioapic *)addr;

    memcpy(&acpi_context.ioapic,entry,sizeof(struct acpi_table_ioapic));
    printk("ioapic struct is %x,%d,%d\n",acpi_context.ioapic.address,acpi_context.ioapic.header.length,sizeof(struct acpi_table_ioapic));
}

static void __init apic_parse_int_src_ovr(void * addr){
    struct acpi_table_int_src_ovr *entry = NULL;
    entry = (struct acpi_table_int_src_ovr *)addr;

    memcpy(&acpi_context.ioapic_ovr,entry,sizeof(struct acpi_table_int_src_ovr));
    printk("ioapic_src_ovr struct is %d,%d\n",acpi_context.ioapic_ovr.header.length,sizeof(struct acpi_table_int_src_ovr));
    printk("ioapic_src_ovr struct is %d,%d,%d\n",
        acpi_context.ioapic_ovr.bus,
        acpi_context.ioapic_ovr.bus_irq
        ,acpi_context.ioapic_ovr.global_irq);
}


static int __init acpi_parse_madt(unsigned long len, unsigned long phys)
{ 
    struct acpi_table_madt * madt;
    
    madt = (struct acpi_table_madt *)_rang_mapping(phys,len);
    unsigned long table_size = len - sizeof(struct acpi_table_madt);
    acpi_madt_entry_header * entry_header =  (acpi_madt_entry_header *)((void * )madt + sizeof(struct acpi_table_madt));

    acpi_context.lapic_address =  madt->lapic_address;

    while (entry_header && table_size > 0)
    {
        switch (entry_header->type)
        {
            case ACPI_MADT_LAPIC:
                apic_parse_lapic((void *)entry_header);
                break;
            case ACPI_MADT_IOAPIC:
                apic_parse_ioapic((void *)entry_header);
                break;
            case ACPI_MADT_INT_SRC_OVR:
                apic_parse_int_src_ovr((void *)entry_header);
            break;
            default:
                break;
        }

        table_size = table_size - entry_header->length;
        entry_header = (acpi_madt_entry_header *)((void *)entry_header + entry_header->length);
    } 

    return 0;
}

__init void  acpi_tables_init(){

    //set apic process func
    handles[ACPI_APIC] = acpi_parse_madt;


    struct acpi_table_rsdp *rsdp = NULL;
    struct acpi_table_rsdt * rsdt;
    struct acpi_table_rsdt copy_rsdt;
    acpi_table_header * header;

    rsdp =  find_rsdp_entry();
    if(!rsdp){
        printk("not find rsdp\n");
        return;
    } 
    printk("find rsdp :%.8s v%d [%.6s]\n", rsdp->signature, rsdp->revision, rsdp->oem_id);

    if (strncmp(rsdp->signature, RSDP_SIG,strlen(RSDP_SIG))) {
		printk( "RSDP table signature incorrect\n");
		return;
	}

    rsdt = (struct acpi_table_rsdt *) _rang_mapping(rsdp->rsdt_address,sizeof(struct acpi_table_rsdt));
    if(!rsdp){
        printk("rsdt table is not found\n");
    }
    
    header =  &rsdt->header; 
    
    if (strncmp(header->signature, RSDT_SIG,strlen(RSDT_SIG))) {
		printk( "RSDT table signature incorrect\n");
		return;
	} 

    if(header->length > sizeof(struct acpi_table_rsdt)){
        printk( "RSDT table too many entrys\n");
		return;
    }

    memcpy(&copy_rsdt,rsdt,sizeof(struct acpi_table_rsdt));

    header =  &copy_rsdt.header;

    int entries  = (header->length - sizeof(acpi_table_header) ) / 4;
    
    for (int i = 0; i < entries; i++)
    {
        int type = 0;
        header = (acpi_table_header *) _rang_mapping(copy_rsdt.entry[i],sizeof(acpi_table_header));
        
        if (acpi_checksum(header,header->length)) { 
			continue;
		}

        for (type = 0; type < ACPI_TABLE_COUNT; type++)
        {
            if (!strncmp(header->signature, acpi_table_signatures[type],strlen(acpi_table_signatures[type]))) {
                break;
	        } 
        }
        
        if(type >= ACPI_TABLE_COUNT){
            continue;
        } 

        acpi_table_handler func =  handles[type];
        if(!func){
            continue;
        }
        func(header->length,(unsigned long)copy_rsdt.entry[i]);
           
    } 
}