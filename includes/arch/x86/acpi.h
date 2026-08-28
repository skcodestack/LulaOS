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

enum {
	ACPI_MADT_LAPIC = 0,
	ACPI_MADT_IOAPIC,
	ACPI_MADT_INT_SRC_OVR,
	ACPI_MADT_NMI_SRC,
	ACPI_MADT_LAPIC_NMI,
	ACPI_MADT_LAPIC_ADDR_OVR,
	ACPI_MADT_IOSAPIC,
	ACPI_MADT_LSAPIC,
	ACPI_MADT_PLAT_INT_SRC,
	ACPI_MADT_LX2APIC,
	ACPI_MADT_ENTRY_COUNT
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


/* FADT (Fixed ACPI Description Table) */
struct acpi_table_fadt {
	acpi_table_header header;
	uint32_t facs;                    /* Firmware ACPI Control Structure 物理地址 */
	uint32_t dsdt;                    /* Differentiated System Description Table 物理地址 */
	uint8_t  reserved1;               /* INT_MODEL */
	uint8_t  preferred_pm_profile;    /* Preferred Power Management Profile */
	uint16_t sci_int;                 /* SCI 中断号（GSI） */
	uint32_t smi_cmd;                 /* SMI 命令端口 */
	uint8_t  acpi_enable;
	uint8_t  acpi_disable;
	uint8_t  s4bios_req;
	uint8_t  pstate_cnt;
	uint32_t pm1a_evt_blk;            /* PM1a Event Block I/O 端口 */
	uint32_t pm1b_evt_blk;
	uint32_t pm1a_cnt_blk;            /* PM1a Control Block I/O 端口 */
	uint32_t pm1b_cnt_blk;
	uint32_t pm2_cnt_blk;
	uint32_t pm_tmr_blk;              /* PM Timer I/O 端口 */
	uint32_t gpe0_blk;
	uint32_t gpe1_blk;
	uint8_t  pm1_evt_len;
	uint8_t  pm1_cnt_len;
	uint8_t  pm2_cnt_len;
	uint8_t  pm_tmr_len;
	uint8_t  gpe0_blk_len;
	uint8_t  gpe1_blk_len;
	uint8_t  gpe1_base;
	uint8_t  cst_cnt;
	uint16_t p_lvl2_lat;
	uint16_t p_lvl3_lat;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t  duty_offset;
	uint8_t  duty_width;
	uint8_t  day_alrm;
	uint8_t  mon_alrm;
	uint8_t  century;
	/* ACPI 2.0+ 扩展字段省略 */
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


typedef struct {
	uint8_t type;
	uint8_t length;
} acpi_madt_entry_header __attribute__ ((packed));

struct acpi_table_lapic {
	acpi_madt_entry_header	header;
	uint8_t			acpi_id;
	uint8_t			id;
	struct {
		uint32_t			enabled:1;
		uint32_t			reserved:31;
	}			flags;
} __attribute__ ((packed));

struct acpi_table_ioapic {
	acpi_madt_entry_header	header;
	uint8_t			id;
	uint8_t			reserved;
	uint32_t			address;
	uint32_t			global_irq_base;
} __attribute__ ((packed));

typedef struct {
	uint16_t			polarity:2;
	uint16_t			trigger:2;
	uint16_t			reserved:12;
} __attribute__ ((packed)) acpi_interrupt_flags;

struct acpi_table_int_src_ovr {
	acpi_madt_entry_header	header;
	uint8_t			bus;
	uint8_t			bus_irq;
	uint32_t			global_irq;
	acpi_interrupt_flags	flags;
} __attribute__ ((packed));





#define ACPI_MAX_LAPIC        32
#define ACPI_MAX_IOAPIC        8
#define ACPI_MAX_INT_SRC_OVR  32

/* ACPI DSDT 设备描述 */
#define ACPI_MAX_DSDT_DEVICES   32
#define ACPI_DEV_NAME_SIZE      8
#define ACPI_HID_SIZE           16
#define ACPI_MAX_CRS_RESOURCES  6

struct acpi_resource_info {
    uint32_t start;
    uint32_t end;
    uint32_t flags;    /* IORESOURCE_IO / IORESOURCE_MEM / IORESOURCE_IRQ */
};

struct acpi_dsdt_device {
    char name[ACPI_DEV_NAME_SIZE];              /* AML 设备名，如 "PS2K" */
    char hid[ACPI_HID_SIZE];                    /* _HID 字符串，如 "PNP0303" */
    char cid[ACPI_HID_SIZE];                    /* _CID 字符串 */
    int num_resources;
    struct acpi_resource_info resource[ACPI_MAX_CRS_RESOURCES];
};

typedef struct {
 uint32_t lapic_address;
 uint32_t lapic_count;
 uint32_t ioapic_count;
 uint32_t int_src_ovr_count;
 struct acpi_table_lapic       lapics[ACPI_MAX_LAPIC];
 struct acpi_table_ioapic      ioapics[ACPI_MAX_IOAPIC];
 struct acpi_table_int_src_ovr int_src_ovrs[ACPI_MAX_INT_SRC_OVR];

 /* FADT 信息 */
 uint32_t dsdt_address;
 uint32_t fadt_address;
 uint16_t sci_int;             /* SCI 中断号 */
 uint32_t pm_tmr_blk;          /* PM Timer I/O 端口 */

 /* DSDT 枚举到的设备 */
 uint32_t dsdt_device_count;
 struct acpi_dsdt_device dsdt_devices[ACPI_MAX_DSDT_DEVICES];
} acpi_table_context;

extern acpi_table_context acpi_context;

/* ACPI 物理地址映射（arch/x86/kernel/acpi.c） */
char *_rang_mapping(unsigned long addr, unsigned long size);

__init void  acpi_tables_init();

/* ACPI Platform 设备注册（kernel/device/acpi_dev.c） */
void acpi_register_platform_devices(void);

// 根据 GSI 查找对应的 IOAPIC 索引
// 返回 -1 表示未找到
static inline int acpi_find_ioapic_by_gsi(uint32_t gsi) {
    for (uint32_t i = 0; i < acpi_context.ioapic_count; i++) {
        // 每个 IOAPIC 的 GSI 范围：[global_irq_base, global_irq_base + rte_count - 1]
        // rte_count 需要从硬件读取，这里简单用下一个 IOAPIC 的 base 作为边界
        uint32_t base = acpi_context.ioapics[i].global_irq_base;
        uint32_t next_base = (i + 1 < acpi_context.ioapic_count) ?
                             acpi_context.ioapics[i + 1].global_irq_base : 0xFFFFFFFF;
        if (gsi >= base && gsi < next_base) {
            return (int)i;
        }
    }
    return -1;
}

#endif