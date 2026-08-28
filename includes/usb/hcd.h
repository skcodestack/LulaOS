/*
 * LulaOS USB 主机控制器驱动（HCD）抽象层
 *
 * 参考 linux-2.6.20 drivers/usb/core/hcd.h
 *
 * 提供：
 *   hc_driver：HCD 驱动回调（start/stop/urb_enqueue 等）
 *   usb_hcd：HCD 实例，内嵌 usb_bus
 */

#ifndef __HCD_H__
#define __HCD_H__

#include <usb/usb.h>

/* ======================== HCD 状态标志 ======================== */

#define HCD_FLAG_HW_ACCESSIBLE  0x00000001  /* 硬件可访问 */
#define HCD_FLAG_POLL_RH        0x00000004  /* 启用 Root Hub 轮询 */

/* ======================== 数据结构 ======================== */

/*
 * hc_driver - HCD 驱动回调接口
 *
 * 各 HCD（UHCI/OHCI/EHCI）实现各自的回调函数
 */
struct hc_driver {
    const char *description;           /* 驱动名称，如 "uhci_hcd" */

    /* 硬件生命周期 */
    int  (*start)(struct usb_hcd *hcd);   /* 启动控制器 */
    void (*stop)(struct usb_hcd *hcd);    /* 停止控制器 */
    void (*reset)(struct usb_hcd *hcd);   /* 复位控制器（可选） */

    /* 传输（简化版，暂不实现完整 URB） */
    int  (*urb_enqueue)(struct usb_hcd *hcd, void *urb);
    int  (*urb_dequeue)(struct usb_hcd *hcd, void *urb);

    /* 端口/Hub 控制（简化版） */
    int  (*hub_status_data)(struct usb_hcd *hcd, char *buf);
    int  (*hub_control)(struct usb_hcd *hcd, unsigned int type,
                        unsigned int request, unsigned int value,
                        unsigned int index, char *buf, unsigned int len);

    /* 帧号（用于等时传输） */
    int  (*get_frame_number)(struct usb_hcd *hcd);
};

/*
 * usb_hcd - USB 主机控制器实例
 *
 * 每个物理 USB 控制器对应一个 usb_hcd，
 * 内嵌 usb_bus 作为对外暴露的总线描述
 */
struct usb_hcd {
    /*
     * 必须作为第一个成员：可通过 (struct usb_bus *) 直接强转
     * 参考 Linux 2.6.20 的 struct usb_hcd
     */
    struct usb_bus self;               /* 对外暴露的总线 */

    const struct hc_driver *driver;    /* HCD 驱动回调 */

    /* 硬件信息 */
    unsigned long io_base;             /* I/O 端口基址 */
    unsigned long regs;                /* MMIO 寄存器基址（EHCI 用） */
    unsigned int irq;                  /* 中断号 */

    struct usb_device *root_hub;       /* Root Hub 设备指针 */

    unsigned long flags;               /* HCD_FLAG_* 标志 */
    unsigned int state;                /* 控制器状态（简化） */

    void *hcd_priv;                    /* HCD 私有数据（uhci_hcd 等） */
};

/* ======================== 公共 API ======================== */

/*
 * usb_create_hcd - 分配并初始化 usb_hcd
 *
 * driver : HCD 驱动
 * dev    : 控制器设备指针（如 pci_dev->dev）
 * bus_name: 总线名称（用于日志）
 *
 * 返回：usb_hcd 指针，失败返回 NULL
 */
struct usb_hcd *usb_create_hcd(const struct hc_driver *driver,
                                struct device *dev,
                                const char *bus_name);

/*
 * usb_add_hcd - 注册 HCD 到 USB 子系统
 *
 * 流程：
 *   1. 调用 driver->start() 启动硬件
 *   2. 创建 Root Hub usb_device
 *   3. 调用 usb_new_device() 注册到总线
 */
int usb_add_hcd(struct usb_hcd *hcd);

/*
 * usb_remove_hcd - 注销 HCD
 *
 * 停止控制器，释放 Root Hub，清理资源
 */
void usb_remove_hcd(struct usb_hcd *hcd);

#endif /* __HCD_H__ */
