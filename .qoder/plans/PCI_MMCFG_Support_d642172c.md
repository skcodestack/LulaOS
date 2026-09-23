# PCI MMCFG Support

## Task 1: 在 acpi.h 中添加 MCFG 表结构和枚举

**文件**: `includes/arch/x86/acpi.h`

1.1 在 `enum` 中添加 `ACPI_MCFG` 条目（插入到 `ACPI_OEMX` 之前，并将 `acpi_table_signatures` 数组同步更新，signature 为 `"MCFG"`）

1.2 定义 MCFG 表结构体：
```c
/* MCFG 表 - MMCFG 配置空间基址 */
struct acpi_table_mcfg {
    acpi_table_header header;
    uint8_t  reserved[8];
    /* 后续紧跟 allocation 数组（可变长） */
} __attribute__ ((packed));

struct acpi_mcfg_allocation {
    uint64_t address;           /* MMCFG 物理基址 */
    uint16_t pci_segment;       /* PCI Segment 号 */
    uint8_t  start_bus_number;  /* 起始总线号 */
    uint8_t  end_bus_number;    /* 结束总线号 */
    uint32_t reserved;
} __attribute__ ((packed));
```

1.3 在 `acpi_table_context` 中添加 MCFG 字段：
```c
/* MCFG 信息 */
uint64_t mcfg_base;             /* MMCFG 物理基址（0=不可用） */
uint8_t  mcfg_start_bus;
uint8_t  mcfg_end_bus;
uint16_t mcfg_segment;
```

---

## Task 2: 在 acpi.c 中添加 MCFG 表解析处理函数

**文件**: `arch/x86/kernel/acpi.c`

2.1 添加 `acpi_parse_mcfg()` 函数：
```c
static int __init acpi_parse_mcfg(unsigned long len, unsigned long phys)
{
    struct acpi_table_mcfg *mcfg;
    struct acpi_mcfg_allocation *alloc;

    mcfg = (struct acpi_table_mcfg *)_rang_mapping(phys, len);
    if (!mcfg) return -1;

    /* 第一个 allocation 紧跟在 reserved[8] 之后 */
    alloc = (struct acpi_mcfg_allocation *)((void *)mcfg + sizeof(*mcfg));

    acpi_context.mcfg_base      = alloc->address;
    acpi_context.mcfg_start_bus = alloc->start_bus_number;
    acpi_context.mcfg_end_bus   = alloc->end_bus_number;
    acpi_context.mcfg_segment   = alloc->pci_segment;

    printk("ACPI MCFG: base=%x bus[%d-%d] seg=%d\n",
           (uint32_t)alloc->address,
           alloc->start_bus_number,
           alloc->end_bus_number,
           alloc->pci_segment);
    return 0;
}
```

2.2 在 `acpi_tables_init()` 中注册 handler（在 `handles[ACPI_FACP]` 之后添加）：
```c
handles[ACPI_MCFG] = acpi_parse_mcfg;
```

---

## Task 3: 在 pci.c 中添加 MMCFG 访问逻辑

**文件**: `kernel/pci/pci.c`

3.1 在文件顶部添加 `#include <arch/x86/highmem.h>` 和 `#include <arch/x86/acpi.h>`

3.2 添加 MMCFG 全局状态：
```c
/* MMCFG 状态（0=未启用，非0=MMCFG 虚拟基址） */
static unsigned long pci_mmcfg_virt_base = 0;
static unsigned char pci_mmcfg_start_bus = 0;
static unsigned char pci_mmcfg_end_bus   = 0;

/* MMCFG fixmap 槽位映射缓存（记录每个槽当前映射的总线号，0xFF=未映射） */
static unsigned char mcfg_slot_bus[PCI_MMCFG_SLOTS];
```

3.3 定义 MMCFG 访问常量（放在 pci.h 中或 pci.c 顶部）：
```c
#define PCI_MMCFG_SLOTS   8   /* fixmap 槽位数 */
#define PCI_MMCFG_SLOT_START FIX_ACPI_BEGIN  /* 复用 ACPI fixmap 区域 */
```

3.4 添加 MMCFG 地址计算 + 按需映射函数：
```c
/*
 * pci_mmcfg_vaddr - 获取指定 bus/devfn/reg 的 MMCFG 虚拟地址
 *
 * 每次访问前检查对应槽位是否已映射目标总线，未映射则 remap。
 * 每条总线占用 1MB（32dev x 8func x 4KB），fixmap 一次映射 4KB 页。
 */
static volatile void *pci_mmcfg_vaddr(unsigned char bus, unsigned char devfn,
                                       unsigned char offset)
{
    unsigned long bus_offset = ((unsigned long)bus << 20)
                             | ((unsigned long)devfn << 12)
                             | (offset & 0xFF);
    unsigned long page_phys = pci_mmcfg_virt_base + bus_offset;
    unsigned long page_off  = page_phys & (PAGE_SIZE - 1);
    unsigned long frame     = page_phys & ~(PAGE_SIZE - 1);
    int slot = bus & (PCI_MMCFG_SLOTS - 1);
    int fix_idx = PCI_MMCFG_SLOT_START + slot;

    if (mcfg_slot_bus[slot] != bus) {
        set_fixmap(fix_idx, frame);
        mcfg_slot_bus[slot] = bus;
    }

    return (volatile void *)(fix_to_virt(fix_idx) + page_off);
}
```

3.5 添加 MMCFG 读写函数：
```c
static unsigned int pci_mmcfg_read32(unsigned char bus, unsigned char devfn,
                                      unsigned char offset)
{
    volatile void *addr = pci_mmcfg_vaddr(bus, devfn, offset);
    return *(volatile unsigned int *)addr;
}

static void pci_mmcfg_write32(unsigned char bus, unsigned char devfn,
                                unsigned char offset, unsigned int val)
{
    volatile void *addr = pci_mmcfg_vaddr(bus, devfn, offset);
    *(volatile unsigned int *)addr = val;
}
```

3.6 修改现有 `pci_config_read32` 和 `pci_config_write32`，添加 MMCFG 优先分支：
```c
unsigned int pci_config_read32(unsigned char bus, unsigned char devfn,
                               unsigned char offset)
{
    if (pci_mmcfg_virt_base)
        return pci_mmcfg_read32(bus, devfn, offset);

    /* Type 1 回退 */
    unsigned int address = (1U << 31) | ((unsigned int)bus << 16)
                         | ((unsigned int)devfn << 8) | (offset & 0xFC);
    outl(address, PCI_CONFIG_ADDRESS);
    return inl(PCI_CONFIG_DATA);
}
```

3.7 添加 `pci_mmcfg_init()` 函数，在 `pci_init()` 中调用：
```c
static void pci_mmcfg_init(void)
{
    int i;

    if (!acpi_context.mcfg_base) {
        printk("PCI: MCFG not found, using Type 1 IO\n");
        return;
    }

    /* 验证 MMCFG 地址有效 */
    if (acpi_context.mcfg_base == 0 ||
        acpi_context.mcfg_start_bus > acpi_context.mcfg_end_bus) {
        printk("PCI: invalid MCFG table, using Type 1 IO\n");
        return;
    }

    /* 初始化槽位缓存 */
    for (i = 0; i < PCI_MMCFG_SLOTS; i++)
        mcfg_slot_bus[i] = 0xFF;

    /* MMCFG 物理基址直接作为"虚拟基址"使用（fixmap 按需映射页） */
    pci_mmcfg_virt_base = (unsigned long)acpi_context.mcfg_base;
    pci_mmcfg_start_bus = acpi_context.mcfg_start_bus;
    pci_mmcfg_end_bus   = acpi_context.mcfg_end_bus;

    printk("PCI: using MMCFG at phys %x, bus [%d-%d]\n",
           (uint32_t)acpi_context.mcfg_base,
           pci_mmcfg_start_bus, pci_mmcfg_end_bus);
}
```

3.8 修改 `pci_init()`，在 `pci_bus_init()` 之后、`pci_scan_bus()` 之前调用：
```c
void pci_init(void)
{
    printk("PCI: scanning buses...\n");
    pci_bus_init();
    pci_mmcfg_init();       /* <-- 新增：尝试初始化 MMCFG */
    pci_scan_bus(0);
    ...
}
```

---

## Task 4: 在 pci.h 中添加 MMCFG 常量声明

**文件**: `includes/pci/pci.h`

在头文件头部注释和常量区添加 MMCFG 相关说明，无需新增公共 API（MMCFG 对上层透明，外部仍通过 `pci_config_read32/write32` 访问）。

---

## Task 5: 验证

1. **QEMU i440fx（传统，无 MCFG）**：确认日志输出 `PCI: MCFG not found, using Type 1 IO`，设备枚举与之前一致
2. **QEMU q35（PCIe，有 MCFG）**：使用 `-machine q35 -usb` 运行，确认日志输出 `PCI: using MMCFG at phys ...` 且设备枚举结果相同或更多

---

## 关键设计说明

| 决策 | 说明 |
|------|------|
| 复用 `FIX_ACPI_BEGIN` 槽位 | ACPI 表解析在 `acpi_tables_init()` 中完成，之后槽位可复用给 MMCFG |
| 8 个槽位 + bus%8 哈希 | 每条总线扫描期间连续访问同一槽，不会频繁换槽 |
| 物理地址作"虚拟基址" | MMCFG 地址计算公式用物理地址算偏移，再传给 fixmap 做实际映射 |
| Type 1 自动回退 | `pci_mmcfg_virt_base == 0` 时走旧路径，无需修改任何调用方 |
