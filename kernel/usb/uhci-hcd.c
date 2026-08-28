/*
 * LulaOS UHCI 主机控制器 PCI 驱动
 *
 * 参考 linux-2.6.20 drivers/usb/host/uhci-hcd.c
 *
 * 实现功能：
 *   - 作为 PCI 驱动发现 class=0x0C0300 的 UHCI 控制器
 *   - uhci_pci_probe()：从 BAR4 获取 I/O 基址，创建 HCD
 *   - uhci_start()：初始化帧列表、启动控制器
 *   - uhci_stop()：停止控制器、释放资源
 *
 * UHCI 寄存器访问方式：I/O 端口（BAR4）
 * 帧列表：1024 项，每项 4 字节，必须 4KB 对齐（物理地址）
 */

#include <usb/uhci.h>
#include <usb/hcd.h>
#include <usb/usb.h>
#include <pci/pci.h>
#include <arch/x86/io.h>
#include <arch/x86/page.h>
#include <mm/slab.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <stddef.h>

/* ======================== 常量 ======================== */

#define UHCI_PCI_CLASS          0x0C0300  /* USB UHCI 控制器类别码 */
#define UHCI_QH_POOL_SIZE       16        /* QH 池大小（简化版） */
#define UHCI_TD_POOL_SIZE       64        /* TD 池大小（简化版） */

/* ======================== 辅助函数 ======================== */

/*
 * 清零内存
 */
static void uhci_memzero(void *p, unsigned int size)
{
    unsigned char *c = (unsigned char *)p;
    unsigned int i;
    for (i = 0; i < size; i++)
        c[i] = 0;
}

/*
 * uhci_read16 - 读取 UHCI 16 位 I/O 寄存器
 */
static inline unsigned short uhci_read16(struct uhci_hcd *uhci, unsigned int reg)
{
    return inw((unsigned short)(uhci->io_base + reg));
}

/*
 * uhci_write16 - 写入 UHCI 16 位 I/O 寄存器
 */
static inline void uhci_write16(struct uhci_hcd *uhci, unsigned int reg,
                                 unsigned short val)
{
    outw(val, (unsigned short)(uhci->io_base + reg));
}

/*
 * uhci_read32 - 读取 UHCI 32 位 I/O 寄存器
 */
static inline unsigned int uhci_read32(struct uhci_hcd *uhci, unsigned int reg)
{
    return inl((unsigned short)(uhci->io_base + reg));
}

/*
 * uhci_write32 - 写入 UHCI 32 位 I/O 寄存器
 */
static inline void uhci_write32(struct uhci_hcd *uhci, unsigned int reg,
                                 unsigned int val)
{
    outl(val, (unsigned short)(uhci->io_base + reg));
}

/* ======================== 控制器生命周期 ======================== */

/*
 * uhci_reset - 复位 UHCI 控制器
 *
 * 设置 USBCMD.HCRESET，等待复位完成
 */
static void uhci_reset(struct uhci_hcd *uhci)
{
    int timeout;

    /* 设置 HCRESET 位 */
    uhci_write16(uhci, UHCI_USBCMD, USBCMD_HCRESET);

    /* 等待复位完成（最多 100 次循环） */
    for (timeout = 100; timeout > 0; timeout--) {
        if (!(uhci_read16(uhci, UHCI_USBCMD) & USBCMD_HCRESET))
            break;
    }

    if (timeout == 0)
        printk("UHCI: reset timeout!\n");
}

/*
 * uhci_init_frame_list - 初始化帧列表
 *
 * 分配 4KB 对齐的帧列表，所有项初始化为 TERMINATE（空）
 * 并将物理地址写入 USBFLBASEADD 寄存器
 */
static int uhci_init_frame_list(struct uhci_hcd *uhci)
{
    unsigned int i;

    /*
     * 分配 4KB 对齐帧列表
     * kmalloc(4096, GFP_KERNEL) 保证 4KB 对齐（8KB 对齐分配）
     */
    uhci->frame_list = kmalloc(UHCI_NUM_FRAMES * sizeof(unsigned int),
                                GFP_KERNEL);
    if (!uhci->frame_list) {
        printk("UHCI: failed to allocate frame list\n");
        return -1;
    }

    /* 所有项初始化为 TERMINATE */
    for (i = 0; i < UHCI_NUM_FRAMES; i++)
        uhci->frame_list[i] = UHCI_FL_TERMINATE;

    /*
     * 写入帧列表基地址寄存器（需要物理地址）
     * LulaOS 内核虚拟地址 = 物理地址 + PAGE_OFFSET，使用 __pa() 转换
     */
    uhci_write32(uhci, UHCI_USBFLBASEADD,
                 (unsigned int)__pa((unsigned long)uhci->frame_list));

    return 0;
}

/*
 * uhci_start - HCD 回调：启动 UHCI 控制器
 *
 * 流程：
 *   1. 复位控制器
 *   2. 初始化帧列表
 *   3. 禁用所有中断（后续按需开启）
 *   4. 启动调度（USBCMD_RS=1）
 */
static int uhci_start(struct usb_hcd *hcd)
{
    struct uhci_hcd *uhci = (struct uhci_hcd *)hcd->hcd_priv;
    unsigned short sts;

    printk("UHCI: starting controller at io_base=0x%08lx\n", uhci->io_base);

    /* 检查控制器是否已停止 */
    sts = uhci_read16(uhci, UHCI_USBSTS);
    if (!(sts & USBSTS_HCH)) {
        /* 控制器仍在运行，先停止 */
        uhci_write16(uhci, UHCI_USBCMD, 0);
        /* 等待 HCH 位置位 */
        int timeout;
        for (timeout = 100; timeout > 0; timeout--) {
            if (uhci_read16(uhci, UHCI_USBSTS) & USBSTS_HCH)
                break;
        }
    }

    /* 复位控制器 */
    uhci_reset(uhci);

    /* 初始化帧列表 */
    if (uhci_init_frame_list(uhci) < 0)
        return -1;

    /* 禁用所有中断（简化实现，暂不使用中断驱动） */
    uhci_write16(uhci, UHCI_USBINTR, 0);

    /* 清除所有挂起的状态位（写 1 清除） */
    uhci_write16(uhci, UHCI_USBSTS,
                 USBSTS_USBINT | USBSTS_ERROR | USBSTS_RD |
                 USBSTS_HSERR | USBSTS_HCPE);

    /* 设置 Configure Flag（CF=1），使 LS/FS 设备路由到 UHCI */
    uhci_write16(uhci, UHCI_USBCMD, USBCMD_CF);

    /* 启动调度器（RS=1，CF=1，MAXP=1） */
    uhci_write16(uhci, UHCI_USBCMD,
                 USBCMD_RS | USBCMD_CF | USBCMD_MAXP);

    uhci->is_stopped = 0;
    uhci->rh_num_ports = 2;   /* UHCI 通常有 2 个端口 */

    /* 验证控制器已启动 */
    sts = uhci_read16(uhci, UHCI_USBSTS);
    if (sts & USBSTS_HCH) {
        printk("UHCI: controller failed to start (still halted)\n");
        return -1;
    }

    printk("UHCI: controller started, %d ports\n", uhci->rh_num_ports);
    return 0;
}

/*
 * uhci_stop - HCD 回调：停止 UHCI 控制器
 */
static void uhci_stop(struct usb_hcd *hcd)
{
    struct uhci_hcd *uhci = (struct uhci_hcd *)hcd->hcd_priv;

    if (uhci->is_stopped)
        return;

    printk("UHCI: stopping controller\n");

    /* 停止调度器 */
    uhci_write16(uhci, UHCI_USBCMD, USBCMD_CF);  /* RS=0, CF=1 */

    /* 等待 HCH 位置位 */
    int timeout;
    for (timeout = 100; timeout > 0; timeout--) {
        if (uhci_read16(uhci, UHCI_USBSTS) & USBSTS_HCH)
            break;
    }

    /* 禁用所有中断 */
    uhci_write16(uhci, UHCI_USBINTR, 0);

    uhci->is_stopped = 1;

    /* 释放帧列表 */
    if (uhci->frame_list) {
        kfree(uhci->frame_list);
        uhci->frame_list = NULL;
    }

    /* 释放 QH/TD 池 */
    if (uhci->qh_pool) {
        kfree(uhci->qh_pool);
        uhci->qh_pool = NULL;
    }
    if (uhci->td_pool) {
        kfree(uhci->td_pool);
        uhci->td_pool = NULL;
    }

    printk("UHCI: controller stopped\n");
}

/* ======================== HCD 驱动描述 ======================== */

static const struct hc_driver uhci_hc_driver = {
    .description = "uhci_hcd",
    .start       = uhci_start,
    .stop        = uhci_stop,
    .reset       = NULL,
    .urb_enqueue = NULL,
    .urb_dequeue = NULL,
    .hub_status_data = NULL,
    .hub_control = NULL,
    .get_frame_number = NULL,
};

/* ======================== PCI 驱动 ======================== */

/*
 * uhci_pci_probe - 发现 UHCI PCI 控制器时调用
 *
 * 流程：
 *   1. 使能设备（开启 I/O 响应）
 *   2. 从 BAR4 获取 I/O 基址
 *   3. 分配 uhci_hcd 私有数据
 *   4. 创建并注册 HCD
 */
static int uhci_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct usb_hcd *hcd;
    struct uhci_hcd *uhci;
    unsigned long io_base;

    printk("UHCI: found PCI device [%04x:%04x] at %02x:%02x.%x\n",
           pdev->vendor, pdev->device,
           pdev->bus, pdev->devfn >> 3, pdev->devfn & 7);

    /* 使能设备 I/O 空间响应 */
    pci_enable_device(pdev);

    /*
     * UHCI 使用 BAR4（resource[4]）作为 I/O 端口基址
     * BAR4 偏移 = PCI_BAR0 + 4*4 = 0x10 + 0x10 = 0x20
     */
    if (!(pdev->resource[4].flags & IORESOURCE_IO)) {
        printk("UHCI: BAR4 is not I/O space, flags=0x%08lx\n",
               pdev->resource[4].flags);
        return -1;
    }

    io_base = pdev->resource[4].start;
    printk("UHCI: BAR4 I/O base = 0x%08lx\n", io_base);

    /* 创建 HCD */
    hcd = usb_create_hcd(&uhci_hc_driver, &pdev->dev, "uhci");
    if (!hcd)
        return -1;

    hcd->io_base = io_base;
    hcd->irq = pdev->irq;

    /* 分配 UHCI 私有数据 */
    uhci = kmalloc(sizeof(*uhci), GFP_KERNEL);
    if (!uhci) {
        printk("UHCI: failed to allocate uhci_hcd\n");
        kfree(hcd);
        return -1;
    }
    uhci_memzero(uhci, sizeof(*uhci));
    uhci->io_base = io_base;

    /* 预分配 QH/TD 池 */
    uhci->qh_pool = kmalloc(sizeof(struct uhci_qh) * UHCI_QH_POOL_SIZE,
                             GFP_KERNEL);
    uhci->td_pool = kmalloc(sizeof(struct uhci_td) * UHCI_TD_POOL_SIZE,
                             GFP_KERNEL);
    uhci->qh_pool_size = UHCI_QH_POOL_SIZE;
    uhci->td_pool_size = UHCI_TD_POOL_SIZE;
    if (uhci->qh_pool)
        uhci_memzero(uhci->qh_pool, sizeof(struct uhci_qh) * UHCI_QH_POOL_SIZE);
    if (uhci->td_pool)
        uhci_memzero(uhci->td_pool, sizeof(struct uhci_td) * UHCI_TD_POOL_SIZE);

    hcd->hcd_priv = (void *)uhci;

    /* 注册 HCD（启动控制器 + 创建 Root Hub） */
    if (usb_add_hcd(hcd) < 0) {
        printk("UHCI: failed to add HCD\n");
        if (uhci->qh_pool) kfree(uhci->qh_pool);
        if (uhci->td_pool) kfree(uhci->td_pool);
        kfree(uhci);
        kfree(hcd);
        return -1;
    }

    /* 将 HCD 指针存入 PCI 设备 driver_data（供 remove 使用） */
    pdev->dev.driver_data = hcd;

    return 0;
}

/*
 * uhci_pci_remove - 移除 UHCI PCI 设备
 */
static void uhci_pci_remove(struct pci_dev *pdev)
{
    struct usb_hcd *hcd = (struct usb_hcd *)pdev->dev.driver_data;

    if (!hcd)
        return;

    usb_remove_hcd(hcd);
    pdev->dev.driver_data = NULL;
}

/*
 * PCI 设备 ID 表
 *
 * 匹配 class=0x0C0300（USB Serial Bus Controller / UHCI）
 */
static const struct pci_device_id uhci_pci_ids[] = {
    { .vendor = 0, .device = 0, .class = UHCI_PCI_CLASS },
    { 0, 0, 0 }   /* 终止项 */
};

/*
 * PCI 驱动描述
 */
static struct pci_driver uhci_pci_driver = {
    .driver   = { .name = "uhci_hcd" },
    .id_table = uhci_pci_ids,
    .probe    = uhci_pci_probe,
    .remove   = uhci_pci_remove,
};

/* ======================== 初始化入口 ======================== */

/*
 * uhci_init - 注册 UHCI PCI 驱动
 *
 * 在 kernel.c 中 usb_init() 之后调用
 * driver_register() 会扫描已有的 PCI 设备并触发 probe
 */
void uhci_init(void)
{
    printk("UHCI: registering PCI driver\n");
    pci_register_driver(&uhci_pci_driver);
}
