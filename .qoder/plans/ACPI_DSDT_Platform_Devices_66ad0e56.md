# ACPI DSDT/SSDT → Platform 设备

## 设计思路

ACPI 中真正的平台设备（PS/2、串口、HPET、电池、按钮等）都描述在 DSDT/SSDT 的 AML 字节码中。

实现路线：
1. 先解析 FADT 拿到 DSDT 物理地址
2. 实现最小 AML 遍历器：只解析命名空间结构（Scope/Device/Name），不执行控制流
3. 扫描所有 `Device()` 节点，提取 `_HID` / `_CID` / `_CRS`
4. 将 `_CRS` Buffer 中的 ACPI Resource Descriptor 转成 `platform_resource`
5. 注册为 Platform 设备，名称用 `_HID` 或 Linux 风格的 `PNPxxxx`

> 注：完整的 AML 解释器非常庞大，本计划采用"只读扫描"策略，只解析设备声明和资源，不执行 Method。

---

## Task 1: 扩展 `includes/arch/x86/acpi.h` — FADT 与 DSDT 结构

**文件**: `includes/arch/x86/acpi.h`

新增：
- `ACPI_FACP`（FADT）已在枚举中，需添加处理函数
- `struct acpi_table_fadt` 结构体（关键字段）
- DSDT 物理地址保存到 `acpi_context`

```c
struct acpi_table_fadt {
    acpi_table_header header;
    uint32_t facs;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    /* ... 后续字段按需扩展 ... */
} __attribute__ ((packed));

#define ACPI_MAX_DSDT_DEVICES  32

struct acpi_dsdt_device {
    char name[8];              /* 设备名，如 "PS2K" */
    char hid[16];              /* _HID 字符串 */
    char cid[16];              /* _CID 字符串 */
    unsigned int flags;
};

/* acpi_context 扩展 */
typedef struct {
    /* ... 原有字段 ... */
    uint32_t dsdt_address;
    uint32_t fadt_address;
    uint32_t dsdt_device_count;
    struct acpi_dsdt_device dsdt_devices[ACPI_MAX_DSDT_DEVICES];
} acpi_table_context;
```

---

## Task 2: 解析 FADT 表 — 获取 DSDT 地址

**文件**: `arch/x86/kernel/acpi.c`

新增 `acpi_parse_fadt`：

```c
static int __init acpi_parse_fadt(unsigned long len, unsigned long phys)
{
    struct acpi_table_fadt *fadt;

    fadt = (struct acpi_table_fadt *)_rang_mapping(phys, len);
    acpi_context.fadt_address = phys;
    acpi_context.dsdt_address = fadt->dsdt;

    printk("ACPI FADT: dsdt=%x sci_int=%d pm_tmr=%x\n",
           fadt->dsdt, fadt->sci_int, fadt->pm_tmr_blk);
    return 0;
}
```

并在 `acpi_tables_init()` 中注册：

```c
handles[ACPI_FACP] = acpi_parse_fadt;
```

---

## Task 3: 创建 `kernel/device/acpi_dev.c` — DSDT 设备扫描

**文件**: `kernel/device/acpi_dev.c`（新建）

实现最小 AML 扫描器：

```c
#include <device/platform.h>
#include <arch/x86/acpi.h>
#include <arch/x86/page.h>
#include <arch/x86/highmem.h>
#include <printk.h>
#include <libs/string.h>
#include <libs/memcpy.h>

/* AML opcode 子集 */
#define AML_ZERO_OP          0x00
#define AML_ONE_OP           0x01
#define AML_ALIAS_OP         0x06
#define AML_NAME_OP          0x08
#define AML_SCOPE_OP         0x10
#define AML_BUFFER_OP        0x11
#define AML_PACKAGE_OP       0x12
#define AML_DEVICE_OP        0x82   /* 扩展opcode：0x5B 0x82 */
#define AML_EXT_PREFIX       0x5B

/* 遍历 AML，收集 Device() 节点 */
static void acpi_dsdt_scan_devices(unsigned long dsdt_phys);

/* 注册扫描到的设备为 Platform 设备 */
void acpi_register_platform_devices(void);
```

核心逻辑：
- 用 `_rang_mapping()` 或早期映射将 DSDT 映射到内核地址
- 字节级遍历，识别 `Device()`、`Scope()`、`Name()`
- 维护当前路径（如 `\_SB.PCI0.LPCB.PS2K`）
- 对每个 Device，查找其内部的 `_HID`、`_CID`、`_CRS` Name
- 将结果保存到 `acpi_context.dsdt_devices[]`

---

## Task 4: 最小 AML 名称/资源解析

**文件**: `kernel/device/acpi_dev.c`

### 4.1 AML NameString 解析

ACPI 名称编码规则：
- `'\'` 根前缀
- `'^'` 父级前缀
- 多名字前缀：`0x2E`（2 名）、`0x2F`（MultiNamePrefix，后跟名字数）
- 每个名字 4 字节大写字符

实现 `acpi_aml_parse_name()` 返回完整路径字符串。

### 4.2 `_CRS` Buffer 解析

`_CRS` 通常是 `Name(_CRS, Buffer() { ... })`。

Buffer 中包含 ACPI Resource Descriptor：
- Small Resource：bit7=0，类型在 bits6:3
- Large Resource：byte0=0x79/0x7A...，后跟 2 字节长度

当前只需支持：
- IRQ Descriptor（small type 0x4）
- I/O Port Descriptor（large type 0x47）
- Memory32 Descriptor（large type 0x81）

实现 `acpi_parse_crs()` 将 descriptor 转成 `platform_resource`。

---

## Task 5: 将 DSDT 设备注册为 Platform 设备

**文件**: `kernel/device/acpi_dev.c`

```c
void acpi_register_platform_devices(void)
{
    int i;

    if (!acpi_context.dsdt_address)
        return;

    acpi_dsdt_scan_devices(acpi_context.dsdt_address);

    for (i = 0; i < acpi_context.dsdt_device_count; i++) {
        struct acpi_dsdt_device *adev = &acpi_context.dsdt_devices[i];
        struct platform_device *pdev;
        /* 分配/静态定义 platform_device，填充资源 */
        ...
        platform_device_register(pdev);
    }
}
```

---

## Task 6: 初始化时调用 ACPI 设备注册

**文件**: `kernel/kernel.c`

在 `platform_bus_init()` 之后调用：

```c
/* 注册 Platform 总线 */
platform_bus_init();

/* 从 ACPI DSDT 注册 Platform 设备 */
acpi_register_platform_devices();

/* 注册 PS/2 键盘/鼠标（如果 ACPI 没提供，则保留静态定义） */
keyboard_init();
mouse_init();
```

---

## Task 7: 处理静态 fallback

**文件**: `kernel/keyboard.c`, `kernel/mouse.c`

当前键盘/鼠标是静态 `platform_device`。若 ACPI DSDT 中已存在 `PNP0303`（键盘）或 `PNP0F13`（鼠标），应避免重复注册。

方案：
- 在 `keyboard_init()` / `mouse_init()` 中先调用 `platform_find_device("ps2-keyboard")` / `"ps2-mouse"`
- 如果 ACPI 已注册，则不注册静态设备
- 但保留静态 `platform_driver`，等待匹配

或者更简单：
- 静态 `keyboard_device` 改名为 `PNP0303`
- 与 ACPI 发现的设备同名， whichever registers first gets matched by the driver

推荐方案：
- 保留静态 device，但名称改为 ACPI 标准 HID：`"PNP0303"` 键盘，`"PNP0F13"` 鼠标
- 这样无论设备来自 ACPI 还是静态 fallback，驱动都能匹配

---

## 文件变更总结

| 文件 | 操作 |
|------|------|
| `includes/arch/x86/acpi.h` | 扩展 FADT/DSDT 结构体与 acpi_context |
| `arch/x86/kernel/acpi.c` | 新增 FADT 解析，保存 DSDT 地址 |
| `kernel/device/acpi_dev.c` | 新建，AML 扫描器 + CRS 解析 + Platform 注册 |
| `kernel/kernel.c` | 在 platform_bus_init 后调用 acpi_register_platform_devices |
| `kernel/keyboard.c` | 静态设备名改为 `PNP0303` |
| `kernel/mouse.c` | 静态设备名改为 `PNP0F13` |

---

## 验证方法

启动后应能看到类似输出：

```
ACPI FADT: dsdt=xxxx sci_int=9 pm_tmr=xxxx
ACPI DSDT: found 8 devices
platform: device 'PNP0303' registered
platform: device 'PNP0F13' registered
platform: driver 'PNP0303' bound to 'PNP0303'
platform: driver 'PNP0F13' bound to 'PNP0F13'
```

---

## 后续扩展

- 支持 SSDT（Secondary SSDT）解析
- 支持 `_STA`（Status）判断设备是否启用
- 支持更复杂的 `_CRS` Large Resource（如 Memory32Fixed、ExtendedIRQ）
- 支持 `_PRW`（唤醒）和 `_PRT`（PCI IRQ 路由）
