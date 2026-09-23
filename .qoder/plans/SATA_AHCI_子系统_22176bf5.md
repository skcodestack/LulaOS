# SATA AHCI 子系统

## 架构对比：现有 IDE vs 新增 SATA

| | 现有 ATA (PIIX3 IDE) | 新增 SATA (AHCI) |
|---|---|---|
| PCI 类别 | class 01h/01h (IDE) | class 01h/06h (SATA) |
| 寄存器访问 | I/O 端口 (0x1F0/0x170) | MMIO (BAR5 映射) |
| 命令发送 | 写 I/O 寄存器 | 构建 H2D FIS → PxCI |
| 数据传输 | PIO/DMA PRD | Command Table PRDT |
| 设备数量 | 2 ch × 2 dev = 4 | 最多 32 端口 |

---

## Task 1：SATA 头文件 — `includes/sata/sata.h`

### 1.1 AHCI 全局寄存器偏移（相对 BAR5）

```c
#define AHCI_CAP        0x00   /* Host Capabilities (NP=bit4:0 端口数-1) */
#define AHCI_GHC        0x04   /* Global Host Control (AE=bit31 AHCI使能) */
#define AHCI_IS         0x08   /* Interrupt Status (per-port bit) */
#define AHCI_PI         0x0C   /* Ports Implemented (bit N = port N 存在) */
#define AHCI_VS         0x10   /* Version */

#define AHCI_GHC_AE     0x80000000  /* AHCI Enable */
#define AHCI_GHC_HR     0x00000001  /* HBA Reset */
```

### 1.2 AHCI 端口寄存器偏移（相对 BAR5 + 0x100 + port × 0x80）

```c
#define PORT_CLB        0x00   /* Command List Base (低32位，1K对齐) */
#define PORT_CLBU       0x04   /* Command List Base 高32位 */
#define PORT_FB         0x08   /* FIS Base (低32位，256字节对齐) */
#define PORT_FBU        0x0C   /* FIS Base 高32位 */
#define PORT_IS         0x10   /* Interrupt Status */
#define PORT_IE         0x14   /* Interrupt Enable */
#define PORT_CMD        0x18   /* Command & Status */
#define PORT_TFD        0x20   /* Task File Data (STS/ERR) */
#define PORT_SIG        0x24   /* Signature (0x00000101=SATA disk) */
#define PORT_SSTS       0x28   /* SATA Status (DET=bit3:0) */
#define PORT_SCTL       0x2C   /* SATA Control */
#define PORT_SERR       0x30   /* SATA Error */
#define PORT_CI         0x38   /* Command Issue (写1触发) */

/* PORT_CMD 位 */
#define PORT_CMD_ST     0x01   /* Start (启动端口) */
#define PORT_CMD_FRE    0x10   /* FIS Receive Enable */
#define PORT_CMD_FR     0x4000 /* FIS Receive Running (只读) */
#define PORT_CMD_CR     0x8000 /* Command List Running (只读) */

/* PORT_SSTS.DET 值 */
#define SSTS_DET_NODEV  0x0    /* 无设备 */
#define SSTS_DET_PRESENT 0x1   /* 设备存在但PHY未建立 */
#define SSTS_DET_PHYUP  0x3    /* 设备存在且PHY已建立 */
```

### 1.3 AHCI FIS 与 Command List 结构

```c
/*
 * ahci_cmd_list - 命令列表条目（32字节，每端口最多32个slot）
 * 必须 1024 字节对齐（每端口32条共 1024 字节）
 */
struct ahci_cmd_list {
    unsigned short  cfl_flags;   /* CFL(bit4:0)=FIS字数; bit6=W写方向; bit8=P */
    unsigned short  prdtl;       /* PRDT 条目数 */
    unsigned int    prdbc;       /* 已传输字节（控制器写回） */
    unsigned int    ctba;        /* Command Table 物理地址（128字节对齐） */
    unsigned int    ctbau;       /* 高32位（LulaOS 32位系统=0） */
    unsigned int    reserved[4];
};

/*
 * ahci_prdt - PRD Table 条目（16字节）
 */
struct ahci_prdt {
    unsigned int    dba;         /* 数据缓冲区物理地址 */
    unsigned int    dbau;        /* 高32位（=0） */
    unsigned int    reserved;
    unsigned int    dbc;         /* 字节数-1（bit31=IOC 中断完成） */
};

/*
 * ahci_cmd_table - 命令表（包含 CFIS + PRDT）
 * 必须 128 字节对齐
 */
struct ahci_cmd_table {
    unsigned char   cfis[64];    /* H2D Register FIS (type 0x27) */
    unsigned char   acmd[16];    /* ATAPI 命令（磁盘不用） */
    unsigned char   reserved[48];
    struct ahci_prdt prdt[1];    /* 可变长 PRDT（实际按需扩展） */
};
```

### 1.4 核心结构体

```c
#define SATA_MAX_PORTS  32
#define SATA_MAX_SLOTS  32

struct sata_port {
    struct sata_host   *host;        /* 所属 AHCI 控制器 */
    unsigned char       port_no;     /* 端口号 0~31 */
    unsigned int        mmio;        /* 端口 MMIO 虚拟基址 */
    unsigned char       present;     /* 设备在线 */
    unsigned char       type;        /* 0=磁盘 1=ATAPI（从PxSIG判断） */
    unsigned int        sectors;     /* LBA28 总扇区数 */
    unsigned int        sectors48;   /* LBA48 总扇区数（高32位） */
    char                model[41];
    char                serial[21];
    unsigned short      identify[256]; /* IDENTIFY 原始数据 */
    /* DMA 缓冲区（每端口独立，避免并发冲突） */
    struct ahci_cmd_list *cmd_list;     /* 虚拟地址 */
    unsigned int          cmd_list_phys; /* 物理地址 */
    unsigned char        *fis_recv;      /* 虚拟地址 (256字节) */
    unsigned int          fis_recv_phys;
    struct ahci_cmd_table *cmd_table[SATA_MAX_SLOTS]; /* 虚拟地址 */
    unsigned int           cmd_table_phys[SATA_MAX_SLOTS];
    wait_queue_head_t     wait_queue;   /* 命令完成等待 */
};

struct sata_host {
    struct pci_dev   *pdev;
    unsigned int      mmio_base_phys;   /* BAR5 物理地址 */
    void             *mmio_base_virt;   /* BAR5 映射后虚拟地址 */
    unsigned char     num_ports;        /* CAP.NP + 1 */
    unsigned int      ports_implemented;/* PI 寄存器值 */
    struct sata_port  ports[SATA_MAX_PORTS];
};

/* 公共 API */
void sata_init(void);
int  sata_read_sectors(struct sata_port *port, unsigned int lba,
                       unsigned char count, void *buf);
int  sata_write_sectors(struct sata_port *port, unsigned int lba,
                        unsigned char count, const void *buf);
```

---

## Task 2：SATA 核心实现 — `kernel/sata/sata.c`

### 2.1 PCI 驱动表

```c
static const struct pci_device_id sata_pci_id_table[] = {
    { .vendor = 0x8086, .device = 0x2922, .class = 0 }, /* ICH9 AHCI (QEMU) */
    { .vendor = 0x8086, .device = 0x2923, .class = 0 }, /* ICH9 AHCI alt */
    { .vendor = 0,      .device = 0,      .class = 0x010601 }, /* 任意 AHCI */
    { 0 },
};
```

### 2.2 sata_pci_probe — 控制器初始化

```
pci_enable_device(pdev)
pci_command |= PCI_COMMAND_MASTER | PCI_COMMAND_MEM   ← 使能 MEM 响应
bar5 = pci_config_read32(BAR5) & 0xFFFFFFF0
mmio_virt = ioremap(bar5, 4096)                       ← MMIO 映射
GHC |= AHCI_GHC_AE                                     ← 全局 AHCI 使能
num_ports = (CAP & 0x1F) + 1
ports_implemented = readl(mmio + AHCI_PI)
for (port = 0; port < num_ports; port++)
    if (ports_implemented & (1 << port))
        sata_port_init(&host->ports[port], port)       ← 初始化端口
sata_scan_host(host)                                    ← 扫描设备
```

### 2.3 sata_port_init — 单端口初始化

```
1. 停止端口：PxCMD &= ~PORT_CMD_ST，等 PxCR 清零
2. 停止 FIS 接收：PxCMD &= ~PORT_CMD_FRE，等 PxFR 清零
3. 清除 PxSERR（写全1）
4. kmalloc 分配 cmd_list（1024字节，1024对齐）→ 写 PORT_CLB
5. kmalloc 分配 fis_recv（256字节，256对齐）→ 写 PORT_FB
6. 为 slot 0 分配 cmd_table（512字节，128对齐）→ 记录物理地址
7. 启用 FIS 接收：PxCMD |= PORT_CMD_FRE
8. 启动端口：PxCMD |= PORT_CMD_ST
```

> 物理地址获取：LulaOS 高半区内核，`phys = virt - PAGE_OFFSET`，使用现有 `__pa()` 宏（见 `includes/arch/x86/page.h`）

### 2.4 sata_scan_host — 总线扫描（核心）

```c
void sata_scan_host(struct sata_host *host)
{
    unsigned int pi, port_no;

    printk("SATA: scanning %d ports on AHCI controller\n", host->num_ports);

    for (port_no = 0; port_no < host->num_ports; port_no++) {
        struct sata_port *port = &host->ports[port_no];
        unsigned int ssts, sig;

        if (!(host->ports_implemented & (1 << port_no)))
            continue;   /* 该端口未实现 */

        /* 读 PxSSTS，检查 DET 字段（bit3:0） */
        ssts = readl(port->mmio + PORT_SSTS);
        if ((ssts & 0x0F) != SSTS_DET_PHYUP) {
            printk("SATA: port%d: no device (ssts=0x%08x)\n", port_no, ssts);
            continue;
        }

        /* 读 PxSIG 判断设备类型 */
        sig = readl(port->mmio + PORT_SIG);
        port->type = (sig == 0x00000101) ? 0 : 1;  /* 0=disk, 1=ATAPI */

        printk("SATA: port%d: device present sig=0x%08x type=%s\n",
               port_no, sig, port->type ? "ATAPI" : "DISK");

        port->present = 1;

        /* 发送 IDENTIFY DEVICE */
        sata_identify(port);
    }
}
```

### 2.5 sata_identify — 通过 H2D FIS 发送 IDENTIFY

```
构建 H2D Register FIS (type=0x27, PMPort=0, C=1, command=0xEC)
填入 cmd_table[0].cfis
构造 cmd_list[0]：cfl=5(words), prdtl=1
构造 prdt[0]：dba=__pa(identify_buf), dbc=511(=512字节-1), IOC=1
写 PORT_CI = (1 << 0) 触发 slot 0
轮询 PORT_CI 清零（或等中断唤醒）
读取 256 words → 解析 model/serial/LBA48扇区数
```

### 2.6 sata_read_sectors — DMA 读（H2D FIS + READ DMA EXT）

```
1. 等 PORT_TFD.BSY=0, DRQ=0
2. 构建 H2D FIS：command=0x25(READ DMA EXT), LBA48地址, count
3. prdt[0].dba=__pa(buf), dbc=count*512-1, IOC=1
4. cmd_list[slot]: W=0(读方向), prdtl=1
5. PORT_CI = (1 << slot)
6. 轮询 PORT_CI 该位清零（或 wait_queue 等中断）
7. 检查 PORT_IS 有无错误
```

### 2.7 sata_write_sectors — DMA 写

与读相同，差异为：
- `cmd_list.cfl_flags` 设置 `W=1`（写方向）
- FIS command = `0x35`（WRITE DMA EXT）

---

## Task 3：集成到内核启动 — `kernel/kernel.c`

```c
/* 在 thread_init_func 中，ata_init() 之后追加： */
ata_init();   /* 现有：Legacy IDE */
sata_init();  /* 新增：AHCI SATA */
```

`sata_init()` 内部注册 PCI 驱动，匹配 class=0x010601 的 AHCI 控制器，与现有 ata_piix 互不干扰（各自匹配不同 PCI class）。

---

## Task 4：Makefile 无需手动添加

Makefile 使用 `$(shell find -name "*.[cS]")` 自动发现所有 `.c` 文件，`kernel/sata/sata.c` 放好目录后自动编译，无需修改 Makefile。

---

## 文件清单

| 操作 | 文件 | 说明 |
|------|------|------|
| 新建 | `includes/sata/sata.h` | AHCI 寄存器定义、结构体、公共 API |
| 新建 | `kernel/sata/sata.c` | AHCI 控制器初始化、端口初始化、总线扫描、IDENTIFY、DMA 读写 |
| 修改 | `kernel/kernel.c` | 在 `ata_init()` 后追加 `sata_init()` 调用 |

---

## 关键实现细节

**内存对齐要求（AHCI 规范强制）：**
- Command List：1024 字节对齐（`kmalloc_align(1024)` 或手动对齐）
- FIS Receive Area：256 字节对齐
- Command Table：128 字节对齐

**对齐分配辅助**：在 sata.c 内实现 `ahci_alloc_aligned(size, align)`，使用 `kmalloc(size + align)` + 手动对齐（参考 `arch/x86/page.h` 的 `__pa()`）。

**QEMU 测试参数**：
```
qemu-system-i386 -drive file=disk.img,if=none,id=hd0 \
                 -device ich9-ahci,id=ahci \
                 -device ide-hd,drive=hd0,bus=ahci.0
```
