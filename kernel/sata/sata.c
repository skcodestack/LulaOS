/*
 * LulaOS SATA/AHCI 子系统实现（传输层 + PCI 探测）
 *
 * 参考：
 *   Intel AHCI 1.3 Specification
 *   Linux drivers/ata/ahci.c         — ahci_init_one()
 *   Linux drivers/ata/libahci.c      — ahci_port_start(), ahci_qc_issue()
 *   Linux drivers/ata/libata-core.c  — ata_dev_read_id()
 *
 * 职责（与 kernel/sata/sata-blk.c 分离）：
 *   - 全局状态：sata_host_instance（单控制器）
 *   - 端口控制：停止/启动、Command List / FIS Receive / Command Table 分配
 *   - 命令发送：中断驱动等待（prepare_to_wait → PORT_CI → schedule）
 *   - IDENTIFY DEVICE（sata_identify，导出供扫描层调用）
 *   - DMA 读写（sata_read/write_sectors，LBA48）
 *   - PCI probe：BAR5 ioremap、全局使能、端口初始化
 *   - 扫描与块设备接入在 sata-blk.c（sata_scan_host + add_disk）
 *
 * 实现功能：
 *   - AHCI 控制器发现（PCI class 01h/06h）
 *   - BAR5 MMIO 映射（ioremap）
 *   - 全局 AHCI 使能（GHC.AE = 1）
 *   - 端口初始化（Command List / FIS Receive 分配，PxSSTS 检测）
 *   - 总线扫描（遍历所有端口，发送 IDENTIFY DEVICE）
 *   - DMA 读写（H2D Register FIS + READ/WRITE DMA EXT + PRDT）
 *
 * 初始化时机：须在 kmem_cache_init() + pci_init() 之后调用（需要 kmalloc + PCI 枚举）
 */

#include <sata/sata.h>
#include <pci/pci.h>
#include <arch/x86/io.h>
#include <arch/x86/page.h>
#include <arch/x86/highmem.h>
#include <interrupts/interrupts.h>   /* request_irq, FIRST_DEVICE_VECTOR */
#include <kernel/sched.h>            /* schedule(), current */
#include <wait.h>                    /* prepare_to_wait / finish_wait */
#include <printk.h>
#include <libs/string.h>
#include <libs/memcpy.h>
#include <mm/slab.h>
#include <stddef.h>

/* ======================== 全局状态 ======================== */

/* 单个 AHCI 控制器（LulaOS 简化为只绑定第一个发现的 AHCI 控制器） */
static struct sata_host sata_host_instance;
static int sata_initialized = 0;

/* ======================== 对齐内存分配辅助 ======================== */

/*
 * ahci_alloc_aligned - 分配对齐的物理内存块
 *
 * AHCI 规范要求：
 *   Command List  : 1024 字节对齐
 *   FIS Receive   : 256  字节对齐
 *   Command Table : 128  字节对齐
 *
 * 实现方式：多分配 align 字节的余量，将返回指针向前对齐。
 * 由于 AHCI 驱动生命周期与系统相同，不需要释放，无需记录原始指针。
 *
 * @size:  请求的字节数
 * @align: 对齐边界（必须是 2 的幂）
 * 返回：对齐后的虚拟地址，失败返回 NULL
 */
static void *ahci_alloc_aligned(unsigned int size, unsigned int align)
{
    void *raw, *aligned;
    unsigned long addr;

    raw = kmalloc(size + align, GFP_KERNEL);
    if (!raw)
        return NULL;

    addr = ((unsigned long)raw + align - 1) & ~(unsigned long)(align - 1);
    aligned = (void *)addr;

    /* 清零对齐后的内存块（AHCI 要求未使用字段为 0） */
    memset(aligned, 0, size);
    return aligned;
}

/* ======================== 端口控制辅助 ======================== */

/*
 * sata_port_stop - 停止端口命令处理
 *
 * AHCI 规范要求：修改 PORT_CLB / PORT_FB 之前，
 * 必须先停止端口（ST=0 且 CR=0）和 FIS 接收（FRE=0 且 FR=0）。
 *
 * 顺序：先停 ST，等 CR 清零；再停 FRE，等 FR 清零。
 */
static int sata_port_stop(struct sata_port *port)
{
    volatile unsigned int *cmd_reg = port->mmio + (PORT_CMD / 4);
    unsigned int val;
    unsigned int timeout;

    /* 停命令处理：清 ST 位 */
    val = sata_readl(cmd_reg);
    if (val & PORT_CMD_ST) {
        sata_writel(cmd_reg, val & ~PORT_CMD_ST);
        /* 等 CR 位清零（命令列表停止运行） */
        timeout = 500000;
        while (timeout--) {
            val = sata_readl(cmd_reg);
            if (!(val & PORT_CMD_CR))
                break;
        }
        if (val & PORT_CMD_CR) {
            printk("SATA: port%u: timeout waiting for CR clear\n", port->port_no);
            return -1;
        }
    }

    /* 停 FIS 接收：清 FRE 位 */
    val = sata_readl(cmd_reg);
    if (val & PORT_CMD_FRE) {
        sata_writel(cmd_reg, val & ~PORT_CMD_FRE);
        /* 等 FR 位清零 */
        timeout = 500000;
        while (timeout--) {
            val = sata_readl(cmd_reg);
            if (!(val & PORT_CMD_FR))
                break;
        }
        if (val & PORT_CMD_FR) {
            printk("SATA: port%u: timeout waiting for FR clear\n", port->port_no);
            return -1;
        }
    }

    return 0;
}

/*
 * sata_port_start - 启动端口（先启用 FIS 接收，再启动命令处理）
 *
 * 顺序：先设 FRE=1（启用 FIS 接收），再设 ST=1（启动命令处理）。
 * 顺序不可颠倒：必须先有 FIS 接收，才能正确接收设备响应。
 */
static void sata_port_start(struct sata_port *port)
{
    volatile unsigned int *cmd_reg = port->mmio + (PORT_CMD / 4);
    unsigned int val;

    /* 启用 FIS 接收 */
    val = sata_readl(cmd_reg);
    sata_writel(cmd_reg, val | PORT_CMD_FRE);

    /* 启动命令处理 */
    val = sata_readl(cmd_reg);
    sata_writel(cmd_reg, val | PORT_CMD_ST);
}

/* ======================== 命令发送（中断驱动） ======================== */

/*
 * sata_wait_cmd - 中断驱动等待命令完成
 *
 * 三段式等待（参考 LulaOS ATA DMA 路径）：
 *   prepare_to_wait  —— 原子地入队 + 设 TASK_UNINTERRUPTIBLE
 *   PORT_CI 触发      —— 启动事件源（此刻起 IRQ 才可能触发）
 *   schedule          —— 让出 CPU，等 IRQ 唤醒
 *   finish_wait       —— 醒来后出队 + 置回 RUNNING
 *
 * ISR（sata_irq_handler）在命令完成时：
 *   1. 读 PORT_IS，保存到 port->cmd_status
 *   2. 写1清零 PORT_IS
 *   3. wake_up(&port->wait_queue)
 *
 * 醒来后：
 *   - 检查 port->cmd_status 有无错误位
 *   - 检查 PORT_CI 确认 slot 已清零
 *
 * @port:  端口
 * @slot:  命令 slot 号（0~31）
 * 返回：0 成功，-1 超时或错误
 */
static int sata_wait_cmd(struct sata_port *port, unsigned int slot)
{
    wait_queue_t wait;
    unsigned int ci_val;

    init_waitqueue_entry(&wait, current);

    /* 原子入队 + 设 TASK_UNINTERRUPTIBLE（此后 ISR 的 wake_up 才能找到我们） */
    prepare_to_wait(&port->wait_queue, &wait);

    /*
     * 触发命令：写 PORT_CI 对应位
     *
     * 必须在 prepare_to_wait 之后：
     *   若先触发命令再入队，IRQ 可能在入队前到达 →
     *   wake_up 遍历空队列 → 唤醒丢失 → 永久睡眠
     */
    sata_writel(port->mmio + (PORT_CI / 4), (1U << slot));

    /* 让出 CPU，等 ISR 唤醒（TASK_UNINTERRUPTIBLE 状态，不被信号打断） */
    schedule();

    /* 醒来后清理：置回 RUNNING + 出队 */
    finish_wait(&port->wait_queue, &wait);

    /*
     * ISR 已将 PORT_IS 快照保存到 port->cmd_status 并清零了 PORT_IS。
     * 在此检查快照中的错误位。
     */
    if (port->cmd_status & PORT_IS_ERR) {
        unsigned int tfd = sata_readl(port->mmio + (PORT_TFD / 4));
        printk("SATA: port%u: command error (is=0x%08x tfd=0x%08x)\n",
               port->port_no, port->cmd_status, tfd);
        return -1;
    }

    /* 确认 PORT_CI slot 位已清零（命令完成） */
    ci_val = sata_readl(port->mmio + (PORT_CI / 4));
    if (ci_val & (1U << slot)) {
        printk("SATA: port%u: command still pending after wake (ci=0x%08x)\n",
               port->port_no, ci_val);
        return -1;
    }

    return 0;
}

/* ======================== AHCI 中断处理函数 ======================== */

/*
 * sata_irq_handler - AHCI 共享中断处理函数
 *
 * AHCI 所有端口共享一个 PCI IRQ（pdev->irq）。当任意端口命令完成
 * 或发生错误时，HBA 拉高 PCI 中断线，内核路由到本函数。
 *
 * 处理流程：
 *   1. 读 GHC_IS（全局中断状态）：bit N = 1 表示 port N 有待处理事件
 *   2. 遍历所有有事件的端口：
 *      a. 读 PORT_IS 保存到 port->cmd_status（被唤醒进程读取）
 *      b. 写1清零 PORT_IS（解除端口级中断挂起）
 *      c. wake_up(&port->wait_queue)（唤醒等待该端口的进程）
 *   3. 写1清零 GHC_IS（解除全局中断挂起）
 *
 * 共享 IRQ 说明：
 *   若 GHC_IS=0（不是我们的中断，可能是共享 IRQ 的其他设备），
 *   直接返回，不做任何操作。
 */
static void sata_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    struct sata_host *host = (struct sata_host *)dev_id;
    volatile unsigned int *ghc_is_reg = host->mmio_base_virt + (AHCI_IS / 4);
    unsigned int ghc_is = sata_readl(ghc_is_reg);
    unsigned int port_no;

    /* GHC_IS=0：不是本控制器的中断（共享 IRQ 情况） */
    if (ghc_is == 0)
        return;

    for (port_no = 0; port_no < host->num_ports && port_no < SATA_MAX_PORTS;
         port_no++) {
        struct sata_port *port;
        unsigned int pis;

        if (!(ghc_is & (1U << port_no)))
            continue;  /* 该端口无待处理事件 */

        port = &host->ports[port_no];
        if (!port->mmio)
            continue;  /* 端口未初始化 */

        /* 读 PORT_IS 快照（唤醒后的进程通过 port->cmd_status 读取） */
        pis = sata_readl(port->mmio + (PORT_IS / 4));
        port->cmd_status = pis;

        /* 写1清零 PORT_IS（解除端口级中断挂起，允许下次中断） */
        sata_writel(port->mmio + (PORT_IS / 4), pis);

        /* 唤醒等待该端口命令完成的进程 */
        wake_up(&port->wait_queue);
    }

    /* 写1清零 GHC_IS（解除全局中断挂起） */
    sata_writel(ghc_is_reg, ghc_is);
}

/*
 * sata_issue_cmd - 准备并发送一条 AHCI 命令
 *
 * 流程：
 *   1. 等待端口 TFD BSY=0, DRQ=0（端口空闲）
 *   2. 将 Command Header 写入 cmd_list[slot]
 *   3. 清除端口中断状态
 *   4. 调用 sata_wait_cmd()：内部执行三段式中断等待
 *      （入队 → 触发 PORT_CI → schedule → 被 ISR 唤醒）
 *
 * @port:        目标端口
 * @slot:        命令 slot 号（0~31，本实现固定用 slot 0）
 * @cmd_header:  命令头（已构建好 CFL/PRDTL/CTBA）
 * 返回：0 成功，-1 失败
 */
static int sata_issue_cmd(struct sata_port *port, unsigned int slot,
                          struct ahci_cmd_header *cmd_header)
{
    volatile unsigned int *tfd_reg = port->mmio + (PORT_TFD / 4);
    volatile unsigned int *is_reg  = port->mmio + (PORT_IS  / 4);
    unsigned int tfd_timeout;

    /* 1. 等待端口空闲（BSY=0, DRQ=0） */
    tfd_timeout = 1000000;
    while (tfd_timeout--) {
        unsigned int tfd = sata_readl(tfd_reg);
        if (!(tfd & (PORT_TFD_BSY | PORT_TFD_DRQ)))
            break;
    }
    if (tfd_timeout == 0) {
        printk("SATA: port%u: port busy before command (tfd=0x%08x)\n",
               port->port_no, sata_readl(tfd_reg));
        return -1;
    }

    /* 2. 将命令头写入 Command List[slot] */
    port->cmd_list[slot] = *cmd_header;

    /* 3. 清除端口中断状态（写1清零） */
    sata_writel(is_reg, 0xFFFFFFFF);

    /*
     * 4. PORT_CI 触发已移入 sata_wait_cmd()（在 prepare_to_wait 之后执行），
     *    确保入队 → 触发顺序，避免唤醒丢失。
     */
    return sata_wait_cmd(port, slot);
}

/* ======================== IDENTIFY DEVICE ======================== */

/*
 * sata_identify - 发送 IDENTIFY DEVICE 获取设备信息
 *
 * 通过 H2D Register FIS 封装 ATA IDENTIFY (0xEC) 命令，
 * PRDT 指向端口内部的 identify 缓冲区。
 *
 * H2D Register FIS 格式（5 DWORDs = 20 bytes）：
 *   DW0: [31:24]=PMPort [23:16]=0x27(FIS type) [15:8]=0 [7:0]=0x27
 *        → byte0=FIS_TYPE, byte1=C(bit7=1), byte2=command, byte3=features
 *   DW1: LBA0/1/2 + Device
 *   DW2: LBA3/4/5 + Features_hi
 *   DW3: Count + ICC + Control
 *   DW4: reserved
 *
 * 命令完成后解析：
 *   - word 49  : LBA28 能力（bit9）
 *   - word 83  : LBA48 支持（bit10）
 *   - word 100-101: LBA48 扇区数
 *   - word 10-19 : 序列号（20 字符）
 *   - word 27-46 : 型号（40 字符）
 */
int sata_identify(struct sata_port *port)
{
    struct ahci_cmd_header hdr;
    struct ahci_cmd_table *ct = port->cmd_table;
    unsigned char *cfis;
    unsigned int i;
    unsigned long long lba48_sectors;

    /* 清空 Command Table */
    memset(ct, 0, sizeof(*ct));
    cfis = ct->cfis;

    /*
     * 构建 H2D Register FIS
     * byte0: FIS Type = 0x27 (Register H2D)
     * byte1: C=1 (Command bit, 表示这是命令而非控制更新)
     * byte2: Command = 0xEC (IDENTIFY DEVICE)
     */
    cfis[0] = FIS_TYPE_REG_H2D;     /* FIS Type */
    cfis[1] = 0x80;                  /* C=1（命令，非控制） */
    cfis[2] = AHCI_CMD_IDENTIFY;     /* Command: IDENTIFY DEVICE */
    cfis[3] = 0;                     /* Features low */
    /* LBA 和 Device 字段全 0（IDENTIFY 不使用） */
    /* byte4-10 = 0（已 memset） */
    cfis[11] = 0;                    /* Features high */
    cfis[12] = 0;                    /* Count low */
    cfis[13] = 0;                    /* Count high */

    /* 构建 PRDT[0]：指向 identify 数据缓冲区（512 字节） */
    ct->prdt[0].dba  = (unsigned int)__pa(port->identify);
    ct->prdt[0].dbau = 0;
    ct->prdt[0].reserved = 0;
    /* DBC = 字节数-1 = 511；IOC=1（完成时产生中断标志） */
    ct->prdt[0].dbc  = 511 | AHCI_PRDT_IOC;

    /* 构建 Command Header */
    memset(&hdr, 0, sizeof(hdr));
    hdr.dw0_cfl_flags = 5;          /* CFL=5 DWORDs（H2D Register FIS） */
                                     /* W=0：数据方向 D2H（设备→主机） */
    hdr.dw0_prdtl     = 1;          /* 1 个 PRDT 条目 */
    hdr.dw1_prdbc     = 0;          /* 传输前清零 */
    hdr.dw2_ctba      = port->cmd_table_phys;
    hdr.dw3_ctbau     = 0;

    /* 发送命令，等待完成 */
    if (sata_issue_cmd(port, 0, &hdr) != 0) {
        printk("SATA: port%u: IDENTIFY failed\n", port->port_no);
        return -1;
    }

    /* 验证 IDENTIFY 数据有效性：word 0 不应为全 0 或全 1 */
    if (port->identify[0] == 0x0000 || port->identify[0] == 0xFFFF) {
        printk("SATA: port%u: IDENTIFY data invalid (word0=0x%04x)\n",
               port->port_no, port->identify[0]);
        return -1;
    }

    /* 检查 LBA 支持（word 49 bit9） */
    if (!(port->identify[SATA_ID_CAPS] & 0x0200)) {
        printk("SATA: port%u: LBA not supported (CHS only, unsupported)\n",
               port->port_no);
        return -1;
    }

    /* LBA28 扇区数（word 60-61，小端序） */
    port->sectors = ((unsigned int)port->identify[SATA_ID_LBA_HI] << 16)
                  | (unsigned int)port->identify[SATA_ID_LBA_LO];

    /* LBA48 扇区数（word 100-101，小端序；仅在 word83 bit10=1 时有效） */
    if (port->identify[SATA_ID_CMD_SET2] & 0x0400) {
        unsigned int lo = (unsigned int)port->identify[SATA_ID_LBA48_LO];
        unsigned int hi = (unsigned int)port->identify[SATA_ID_LBA48_MID];
        port->sectors48 = ((unsigned long long)hi << 16) | lo;
    } else {
        port->sectors48 = (unsigned long long)port->sectors;
    }

    /* 提取序列号（word 10-19，每 word 高字节在前低字节在后） */
    for (i = 0; i < 10; i++) {
        unsigned short w = port->identify[SATA_ID_SERIAL + i];
        port->serial[i * 2]     = (unsigned char)(w >> 8);   /* 高字节 */
        port->serial[i * 2 + 1] = (unsigned char)(w & 0xFF); /* 低字节 */
    }
    port->serial[20] = '\0';
    /* 去除尾部空格 */
    for (i = 19; i > 0 && port->serial[i] == ' '; i--)
        port->serial[i] = '\0';

    /* 提取型号（word 27-46，同上字节序） */
    for (i = 0; i < 20; i++) {
        unsigned short w = port->identify[SATA_ID_MODEL + i];
        port->model[i * 2]     = (unsigned char)(w >> 8);
        port->model[i * 2 + 1] = (unsigned char)(w & 0xFF);
    }
    port->model[40] = '\0';
    for (i = 39; i > 0 && port->model[i] == ' '; i--)
        port->model[i] = '\0';

    lba48_sectors = port->sectors48;
    printk("SATA: port%u: model='%s' serial='%s'\n",
           port->port_no, port->model, port->serial);
    printk("SATA: port%u: LBA28=%u sectors (%u MB), "
           "LBA48=%llu sectors (%llu GB)\n",
           port->port_no,
           port->sectors, port->sectors / 2048,
           lba48_sectors, lba48_sectors / (1024ULL * 1024 * 2));

    return 0;
}

/* ======================== DMA 读/写扇区 ======================== */

/*
 * sata_read_sectors - DMA 方式读取扇区（LBA48）
 *
 * 使用 READ DMA EXT (0x25) 命令，通过 H2D Register FIS 下发，
 * PRDT 指向 buf 的物理地址。LBA48 支持最大 128PB 寻址。
 *
 * 流程：
 *   1. 构建 H2D FIS：command=0x25，LBA48 地址拆分到 LBA0~5，count 拆分
 *   2. 构建 PRDT：buf 物理地址，字节数-1，IOC=1
 *   3. 构建 Command Header：CFL=5，W=0（读），PRDTL=1
 *   4. 发送并等待完成
 */
int sata_read_sectors(struct sata_port *port, unsigned long long lba,
                      unsigned int count, void *buf)
{
    struct ahci_cmd_header hdr;
    struct ahci_cmd_table *ct = port->cmd_table;
    unsigned char *cfis;
    unsigned int total_bytes;

    if (!port->present || !count)
        return -1;

    total_bytes = count * 512;

    /* 单次 PRDT 条目最大 4MB（DBC 为 22 位，最大 2^22-1 = 4MB-1） */
    if (total_bytes > (4 * 1024 * 1024)) {
        printk("SATA: read: transfer too large (%u bytes, max 4MB)\n",
               total_bytes);
        return -1;
    }

    /* 清空 Command Table */
    memset(ct, 0, sizeof(*ct));
    cfis = ct->cfis;

    /* 构建 H2D Register FIS：READ DMA EXT (0x25) */
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;                  /* C=1 */
    cfis[2] = AHCI_CMD_READ_DMA_EXT;
    cfis[3] = 0;                     /* Features low */

    /* LBA bits 0-23 */
    cfis[4] = (unsigned char)(lba & 0xFF);          /* LBA0 */
    cfis[5] = (unsigned char)((lba >> 8) & 0xFF);   /* LBA1 */
    cfis[6] = (unsigned char)((lba >> 16) & 0xFF);  /* LBA2 */
    cfis[7] = 0x40;                  /* Device: bit6=1 (LBA 模式) */

    /* LBA bits 24-47 */
    cfis[8]  = (unsigned char)((lba >> 24) & 0xFF); /* LBA3 */
    cfis[9]  = (unsigned char)((lba >> 32) & 0xFF); /* LBA4 */
    cfis[10] = (unsigned char)((lba >> 40) & 0xFF); /* LBA5 */
    cfis[11] = 0;                    /* Features high */

    /* Count（LBA48 使用 16 位计数，0 = 65536 扇区） */
    cfis[12] = (unsigned char)(count & 0xFF);        /* Count low */
    cfis[13] = (unsigned char)((count >> 8) & 0xFF); /* Count high */

    /* PRDT[0]：指向 buf */
    ct->prdt[0].dba  = (unsigned int)__pa(buf);
    ct->prdt[0].dbau = 0;
    ct->prdt[0].reserved = 0;
    ct->prdt[0].dbc  = (total_bytes - 1) | AHCI_PRDT_IOC;

    /* Command Header */
    memset(&hdr, 0, sizeof(hdr));
    hdr.dw0_cfl_flags = 5;          /* CFL=5，W=0（读方向） */
    hdr.dw0_prdtl     = 1;
    hdr.dw1_prdbc     = 0;
    hdr.dw2_ctba      = port->cmd_table_phys;
    hdr.dw3_ctbau     = 0;

    return sata_issue_cmd(port, 0, &hdr);
}

/*
 * sata_write_sectors - DMA 方式写入扇区（LBA48）
 *
 * 与读操作的差异：
 *   - FIS command = WRITE DMA EXT (0x35)
 *   - Command Header flag W=1（写方向：主机→设备）
 *   - 写完成后自动追加 CACHE FLUSH 确保数据落盘
 */
int sata_write_sectors(struct sata_port *port, unsigned long long lba,
                       unsigned int count, const void *buf)
{
    struct ahci_cmd_header hdr;
    struct ahci_cmd_table *ct = port->cmd_table;
    unsigned char *cfis;
    unsigned int total_bytes;

    if (!port->present || !count)
        return -1;

    total_bytes = count * 512;

    if (total_bytes > (4 * 1024 * 1024)) {
        printk("SATA: write: transfer too large (%u bytes, max 4MB)\n",
               total_bytes);
        return -1;
    }

    /* 清空 Command Table */
    memset(ct, 0, sizeof(*ct));
    cfis = ct->cfis;

    /* 构建 H2D Register FIS：WRITE DMA EXT (0x35) */
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;                  /* C=1 */
    cfis[2] = AHCI_CMD_WRITE_DMA_EXT;
    cfis[3] = 0;                     /* Features low */

    cfis[4]  = (unsigned char)(lba & 0xFF);
    cfis[5]  = (unsigned char)((lba >> 8) & 0xFF);
    cfis[6]  = (unsigned char)((lba >> 16) & 0xFF);
    cfis[7]  = 0x40;                 /* Device: LBA 模式 */
    cfis[8]  = (unsigned char)((lba >> 24) & 0xFF);
    cfis[9]  = (unsigned char)((lba >> 32) & 0xFF);
    cfis[10] = (unsigned char)((lba >> 40) & 0xFF);
    cfis[11] = 0;

    cfis[12] = (unsigned char)(count & 0xFF);
    cfis[13] = (unsigned char)((count >> 8) & 0xFF);

    /* PRDT[0] */
    ct->prdt[0].dba  = (unsigned int)__pa(buf);
    ct->prdt[0].dbau = 0;
    ct->prdt[0].reserved = 0;
    ct->prdt[0].dbc  = (total_bytes - 1) | AHCI_PRDT_IOC;

    /* Command Header：W=1（写方向） */
    memset(&hdr, 0, sizeof(hdr));
    hdr.dw0_cfl_flags = 5 | AHCI_CMD_FLAG_W;   /* CFL=5, W=1 */
    hdr.dw0_prdtl     = 1;
    hdr.dw1_prdbc     = 0;
    hdr.dw2_ctba      = port->cmd_table_phys;
    hdr.dw3_ctbau     = 0;

    if (sata_issue_cmd(port, 0, &hdr) != 0)
        return -1;

    /*
     * 写完成后发送 CACHE FLUSH (0xE7)
     * 确保写缓存中的数据实际写入磁盘介质。
     * 不刷新则断电可能丢失数据。
     */
    memset(ct, 0, sizeof(*ct));
    cfis = ct->cfis;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;
    cfis[2] = AHCI_CMD_FLUSH_CACHE;

    memset(&hdr, 0, sizeof(hdr));
    hdr.dw0_cfl_flags = 5;
    hdr.dw0_prdtl     = 0;          /* CACHE FLUSH 无数据传输 */
    hdr.dw2_ctba      = port->cmd_table_phys;

    return sata_issue_cmd(port, 0, &hdr);
}

/* ======================== 端口初始化 ======================== */

/*
 * sata_port_init - 初始化单个 SATA 端口
 *
 * 流程（AHCI 1.3 spec 10.1.2）：
 *   1. 停止端口（ST=0 等 CR=0，FRE=0 等 FR=0）
 *   2. 清除 PxSERR（写全1，解锁端口状态机）
 *   3. 分配 Command List（1024字节，1024对齐）→ 写 PORT_CLB
 *   4. 分配 FIS Receive（256字节，256对齐）→ 写 PORT_FB
 *   5. 分配 Command Table（256字节，128对齐）→ 记录物理地址
 *   6. 启用 FIS 接收（FRE=1）
 *   7. 启动端口（ST=1）
 */
static int sata_port_init(struct sata_host *host, unsigned int port_no)
{
    struct sata_port *port = &host->ports[port_no];
    volatile unsigned int *base = (volatile unsigned int *)
        ((unsigned long)host->mmio_base_virt +
         AHCI_PORT_BASE + port_no * AHCI_PORT_SIZE);

    port->host    = host;
    port->port_no = port_no;
    port->mmio    = base;
    port->present = 0;

    /* 停止端口 */
    if (sata_port_stop(port) != 0)
        return -1;

    /* 清除 PxSERR（写全1，解锁端口状态机，否则 PxCI 写操作被阻塞） */
    sata_writel(port->mmio + (PORT_SERR / 4), 0xFFFFFFFF);

    /* 分配 Command List：1024 字节，1024 字节对齐 */
    port->cmd_list = (struct ahci_cmd_header *)
        ahci_alloc_aligned(1024, 1024);
    if (!port->cmd_list) {
        printk("SATA: port%u: failed to allocate Command List\n", port_no);
        return -1;
    }
    port->cmd_list_phys = (unsigned int)__pa(port->cmd_list);

    /* 写入 PORT_CLB / PORT_CLBU */
    sata_writel(port->mmio + (PORT_CLB  / 4), port->cmd_list_phys);
    sata_writel(port->mmio + (PORT_CLBU / 4), 0);

    /* 分配 FIS Receive：256 字节，256 字节对齐 */
    port->fis_recv = (unsigned char *)ahci_alloc_aligned(256, 256);
    if (!port->fis_recv) {
        printk("SATA: port%u: failed to allocate FIS Receive\n", port_no);
        return -1;
    }
    port->fis_recv_phys = (unsigned int)__pa(port->fis_recv);

    /* 写入 PORT_FB / PORT_FBU */
    sata_writel(port->mmio + (PORT_FB  / 4), port->fis_recv_phys);
    sata_writel(port->mmio + (PORT_FBU / 4), 0);

    /* 分配 Command Table：256 字节，128 字节对齐 */
    port->cmd_table = (struct ahci_cmd_table *)
        ahci_alloc_aligned(sizeof(struct ahci_cmd_table), 128);
    if (!port->cmd_table) {
        printk("SATA: port%u: failed to allocate Command Table\n", port_no);
        return -1;
    }
    port->cmd_table_phys = (unsigned int)__pa(port->cmd_table);

    /* 初始化命令完成等待队列（中断驱动必须，sata_wait_cmd 在此睡眠） */
    init_waitqueue_head(&port->wait_queue);
    port->cmd_status = 0;

    /*
     * 使能端口中断：DPS（命令完成）+ 所有错误位
     *
     * PORT_IE 位掩码与 PORT_IS 完全相同。
     * 必须使能 DPS (bit15)：命令完成时 HBA 才会触发中断。
     * 同时使能错误位：TFE/IF/HBF 等发生时也触发中断唤醒等待进程。
     */
    sata_writel(port->mmio + (PORT_IE / 4),
                PORT_IS_DPS | PORT_IS_TFE | PORT_IS_IF |
                PORT_IS_HBF | PORT_IS_HBD | PORT_IS_PCE);

    /* 启用 FIS 接收，启动端口 */
    sata_port_start(port);

    printk("SATA: port%u: initialized (cmd_list=0x%08x fis=0x%08x ct=0x%08x)\n",
           port_no, port->cmd_list_phys, port->fis_recv_phys,
           port->cmd_table_phys);

    return 0;
}

/* ======================== PCI 驱动注册 ======================== */

/*
 * sata_pci_id_table - 支持的 PCI 设备 ID 表
 *
 * 匹配规则：vendor+device 精确匹配，或 class=0x010601（任意 AHCI 控制器）
 *
 * PCI class code 0x010601 = Mass Storage (01h) / SATA (06h) / AHCI (01h)
 */
static const struct pci_device_id sata_pci_id_table[] = {
    { .vendor = 0x8086, .device = 0x2922, .class = 0 }, /* ICH9 AHCI (QEMU) */
    { .vendor = 0x8086, .device = 0x2923, .class = 0 }, /* ICH9 AHCI alt  */
    { .vendor = 0,      .device = 0,      .class = 0x010601 }, /* 任意 AHCI 1.x */
    { 0 },
};

/*
 * sata_pci_probe - AHCI 控制器 PCI probe 回调
 *
 * 流程（参考 Linux ahci_init_one()）：
 *   1. 使能 PCI 设备（MEM 响应 + Bus Master）
 *   2. 读取 BAR5（AHCI MMIO 基址）
 *   3. ioremap BAR5 映射到虚拟地址
 *   4. 全局 AHCI 使能（GHC.AE = 1）
 *   5. 读取 CAP（端口数）和 PI（已实现端口位图）
 *   6. 初始化每个已实现端口
 *   7. 扫描总线，发现并识别设备（sata_scan_host，实现在 sata-blk.c）
 */
static int sata_pci_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id)
{
    struct sata_host *host = &sata_host_instance;
    unsigned int bar5_raw, cap, pi, vs;
    volatile unsigned int *mmio;
    unsigned int port_no;

    (void)id;

    /* 防重入：只绑定第一个 AHCI 控制器 */
    if (sata_initialized) {
        printk("SATA: ignoring additional AHCI controller at %02x:%02x.%x\n",
               pdev->bus, pdev->devfn >> 3, pdev->devfn & 7);
        return -1;
    }
    sata_initialized = 1;

    printk("SATA: found AHCI controller at %02x:%02x.%x [%04x:%04x]\n",
           pdev->bus, pdev->devfn >> 3, pdev->devfn & 7,
           pdev->vendor, pdev->device);

    /*
     * 使能 MEM 空间响应 + Bus Master
     *
     * AHCI 使用 MMIO（非 I/O 端口），必须开启 PCI_COMMAND_MEM。
     * DMA 传输需要 PCI_COMMAND_MASTER（总线控制权）。
     */
    pci_enable_device(pdev);
    {
        unsigned int cmd;
        cmd = pci_config_read32(pdev->bus, pdev->devfn, PCI_COMMAND);
        cmd |= PCI_COMMAND_MASTER | PCI_COMMAND_MEM;
        pci_config_write32(pdev->bus, pdev->devfn, PCI_COMMAND, cmd);
        printk("SATA: PCI command = 0x%04x (MEM+Master enabled)\n",
               cmd & 0xFFFF);
    }

    /*
     * 读取 BAR5（AHCI MMIO 基址）
     *
     * BAR5 位于 PCI config offset 0x24，类型为 Memory（bit0=0）。
     * 实际基址 = bar5 & 0xFFFFFFF0（低 4 位为标志位）。
     */
    bar5_raw = pci_config_read32(pdev->bus, pdev->devfn, 0x24);
    bar5_raw &= 0xFFFFFFF0;

    if (bar5_raw == 0) {
        printk("SATA: BAR5 is zero, AHCI not properly configured\n");
        sata_initialized = 0;
        return -1;
    }
    printk("SATA: BAR5 phys = 0x%08x\n", bar5_raw);

    /* ioremap BAR5（AHCI 寄存器区域，映射 4096 字节足够覆盖全局+端口） */
    mmio = (volatile unsigned int *)ioremap(bar5_raw, 4096);
    if (!mmio) {
        printk("SATA: failed to ioremap BAR5\n");
        sata_initialized = 0;
        return -1;
    }

    /* 填充 host 结构 */
    memset(host, 0, sizeof(*host));
    host->pdev           = pdev;
    host->mmio_base_phys = bar5_raw;
    host->mmio_base_virt = mmio;

    /*
     * 全局 AHCI 使能（GHC.AE = 1）
     *
     * 必须在访问任何 AHCI 寄存器前使能，否则控制器处于 Legacy 兼容模式。
     * 部分 BIOS/QEMU 已预置 AE=1，这里写保险。
     */
    {
        unsigned int ghc = sata_readl(mmio + (AHCI_GHC / 4));
        if (!(ghc & AHCI_GHC_AE)) {
            sata_writel(mmio + (AHCI_GHC / 4), ghc | AHCI_GHC_AE);
            printk("SATA: AHCI mode enabled (GHC.AE=1)\n");
        }
    }

    /* 读取 AHCI 版本号 */
    vs = sata_readl(mmio + (AHCI_VS / 4));
    host->version = vs;
    printk("SATA: AHCI version %u.%u (VS=0x%08x)\n",
           (vs >> 16) & 0xFFFF, vs & 0xFFFF, vs);

    /* 读取 CAP 获取端口数（bit4:0 = NP，实际端口数 = NP + 1） */
    cap = sata_readl(mmio + (AHCI_CAP / 4));
    host->num_ports = (cap & AHCI_CAP_NP) + 1;
    if (host->num_ports > SATA_MAX_PORTS)
        host->num_ports = SATA_MAX_PORTS;
    printk("SATA: CAP=0x%08x, num_ports=%u\n", cap, host->num_ports);

    /* 读取 PI（Ports Implemented）位图 */
    pi = sata_readl(mmio + (AHCI_PI / 4));
    host->ports_implemented = pi;
    printk("SATA: PI=0x%08x (implemented ports bitmap)\n", pi);

    /* 初始化所有已实现的端口 */
    for (port_no = 0; port_no < host->num_ports && port_no < SATA_MAX_PORTS;
         port_no++) {
        if (!(pi & (1U << port_no)))
            continue;

        if (sata_port_init(host, port_no) != 0) {
            printk("SATA: port%u: init failed, skipping\n", port_no);
            continue;
        }
    }

    /*
     * 注册 AHCI 中断处理函数
     *
     * AHCI 所有端口共享一个 PCI IRQ（pdev->irq）。
     * 中断向量 = FIRST_DEVICE_VECTOR + irq（LulaOS 统一映射规则）。
     * request_irq 内部自动调用 ioapic_enable_irq() 解除 IOAPIC RTE 屏蔽。
     *
     * 必须在 sata_scan_host 之前注册：
     *   扫描时 IDENTIFY 命令通过 schedule() 等中断唤醒，
     *   若 IRQ 未注册则永远不会被唤醒 → 永久挂死。
     */
    {
        unsigned int irq = pdev->irq;
        unsigned int vector = FIRST_DEVICE_VECTOR + irq;
        int ret;

        ret = request_irq(vector, sata_irq_handler, "ahci", host);
        if (ret == 0) {
            printk("SATA: IRQ%u registered (vector=0x%02x)\n", irq, vector);
        } else {
            printk("SATA: IRQ%u registration FAILED (ret=%d)\n", irq, ret);
        }
    }

    /*
     * 全局 AHCI 中断使能（GHC.IE = 1）
     *
     * 与 GHC.AE 独立：AE 使能寄存器访问，IE 使能中断触发。
     * 必须在 request_irq 之后设置，否则 handler 还未注册就有中断到达。
     */
    {
        unsigned int ghc = sata_readl(mmio + (AHCI_GHC / 4));
        sata_writel(mmio + (AHCI_GHC / 4), ghc | AHCI_GHC_IE);
        printk("SATA: global interrupt enabled (GHC.IE=1)\n");
    }

    /* 扫描总线，发现并识别设备（实现位于 sata-blk.c，扫描后接入块设备层） */
    sata_scan_host(host);

    return 0;
}

/* PCI 驱动描述符 */
static struct pci_driver sata_pci_driver = {
    .driver = {
        .name = "ahci",
    },
    .id_table = sata_pci_id_table,
    .probe    = sata_pci_probe,
    .remove   = NULL,
};

/* ======================== 初始化入口 ======================== */

/*
 * sata_init - SATA/AHCI 子系统初始化
 *
 * 注册 PCI 驱动，匹配 class=0x010601 的 AHCI 控制器。
 * 与现有 ata_piix（匹配 class 0x010100 IDE 控制器）互不干扰。
 *
 * 须在 pci_init() 之后调用（需要 PCI 设备已枚举）。
 */
void sata_init(void)
{
    printk("SATA: initializing AHCI subsystem\n");
    pci_register_driver(&sata_pci_driver);
}
