#include <arch/x86/acpi.h>
#include <stddef.h>
#include <printk.h>
#include <arch/x86/page.h>
#include <libs/string.h>
 

acpi_table_handler handles[ACPI_TABLE_COUNT] = {NULL};


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


static int __init acpi_parse_madt(acpi_table_header * header, unsigned long phys)
{
    printk("madt process func /n");
    struct acpi_table_madt * madt;
    
    madt = (struct acpi_table_madt *)phys;
    

}

__init void  acpi_tables_init(){

    //set apic process func
    handles[ACPI_APIC] = acpi_parse_madt;


    struct acpi_table_rsdp *rsdp = NULL;
    struct acpi_table_rsdt * rsdt;
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

    rsdt = (struct acpi_table_rsdt *)rsdp->rsdt_address; 
    if(!rsdt){
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

    int entries  = (header->length - sizeof(acpi_table_header) ) / 4;
    
    for (int i = 0; i < entries; i++)
    {
        int type = 0;
        header = (acpi_table_header *) rsdt->entry[i];

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
        func(header,(unsigned long)rsdt->entry[i]);
           
    } 
}