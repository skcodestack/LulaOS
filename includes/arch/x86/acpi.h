#ifndef __ACPI_H__
#define __ACPI_H__

#include <stdint.h>
#include <arch/linkage.h>


#define LO_RSDP_WINDOW_BASE         	0	/* Physical Address */
#define HI_RSDP_WINDOW_BASE         	0xE0000	/* Physical Address */
#define LO_RSDP_WINDOW_SIZE         	0x400
#define HI_RSDP_WINDOW_SIZE         	0x20000


#define RSDP_SIG			"RSD PTR "
#define RSDT_SIG 			"RSDT"
#define RSDP_SCAN_STEP			16
#define RSDP_CHECKSUM_LENGTH		20



enum {
	ACPI_APIC = 0,  
	ACPI_BOOT,
	ACPI_DBGP,
	ACPI_DSDT,
	ACPI_ECDT,
	ACPI_ETDT,
	ACPI_FACP,
	ACPI_FACS,
	ACPI_OEMX,
	ACPI_PSDT,
	ACPI_SBST,
	ACPI_SLIT,
	ACPI_SPCR,
	ACPI_SRAT,
	ACPI_SSDT,
	ACPI_SPMI,
	ACPI_XSDT,
	ACPI_TABLE_COUNT
};

static char *acpi_table_signatures[ACPI_TABLE_COUNT] = {
	"APIC",
	"BOOT",
	"DBGP",
	"DSDT",
	"ECDT",
	"ETDT",
	"FACP",
	"FACS",
	"OEM",
	"PSDT",
	"SBST",
	"SLIT",
	"SPCR",
	"SRAT",
	"SSDT",
	"SPMI",
	"XSDT"
};

typedef struct {		 
	char signature[4];	 
	uint32_t length;	 
	uint8_t revision;		 
	uint8_t checksum;		 
	char oem_id[6];		 
	char oem_table_id[8];	 
	uint32_t oem_revision;	 
	uint32_t creator_id;	 
	uint32_t creator_revision;	 
} acpi_table_header __attribute__ ((packed));


struct acpi_table_rsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_address;
} __attribute__ ((packed));

struct acpi_table_rsdt {
	acpi_table_header header;
	uint32_t entry[ACPI_TABLE_COUNT]; //各种表
} __attribute__ ((packed));




//apic 
struct acpi_table_madt {
	acpi_table_header header;
	uint32_t lapic_address;//lapic地址
	struct {
		uint32_t pcat_compat:1;//enable apic，8259必须disable
		uint32_t reserved:31;
	} flags __attribute__ ((packed));
} __attribute__ ((packed));;



__init void  acpi_tables_init();

#endif