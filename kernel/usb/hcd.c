/*
 * LulaOS USB HCD 框架实现
 *
 * 参考 linux-2.6.20 drivers/usb/core/hcd.c
 *
 * 实现功能：
 *   - usb_create_hcd()：分配并初始化 HCD 实例
 *   - usb_add_hcd()：启动控制器、创建 Root Hub、注册到总线
 *   - usb_remove_hcd()：停止控制器、清理资源
 */

#include <usb/hcd.h>
#include <usb/usb.h>
#include <device/device.h>
#include <mm/slab.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <stddef.h>
#include <libs/list.h>

/* 全局总线编号，每注册一个 HCD 递增 */
static int usb_busnum = 0;

/* ======================== 辅助函数 ======================== */

/*
 * 清零内存（kmalloc 后使用）
 */
static void memzero(void *p, unsigned int size)
{
    unsigned char *c = (unsigned char *)p;
    unsigned int i;
    for (i = 0; i < size; i++)
        c[i] = 0;
}

/*
 * create_root_hub - 创建并初始化 Root Hub usb_device
 *
 * Root Hub 由 HCD 内置，模拟一个 USB_CLASS_HUB 设备
 * 简化实现：不执行真实控制传输，直接填充描述符
 */
static struct usb_device *create_root_hub(struct usb_hcd *hcd)
{
    struct usb_device *rhdev;

    rhdev = kmalloc(sizeof(*rhdev), GFP_KERNEL);
    if (!rhdev) {
        printk("USB: failed to allocate root hub\n");
        return NULL;
    }
    memzero(rhdev, sizeof(*rhdev));

    /* 填充设备描述符（模拟 Root Hub） */
    rhdev->descriptor.bLength            = sizeof(struct usb_device_descriptor);
    rhdev->descriptor.bDescriptorType    = USB_DT_DEVICE;
    rhdev->descriptor.bcdUSB             = 0x0110;     /* USB 1.1 */
    rhdev->descriptor.bDeviceClass       = USB_CLASS_HUB;
    rhdev->descriptor.bDeviceSubClass    = 0;
    rhdev->descriptor.bDeviceProtocol    = 0;
    rhdev->descriptor.bMaxPacketSize0    = 64;
    rhdev->descriptor.idVendor           = 0x0000;     /* 虚拟厂商 */
    rhdev->descriptor.idProduct          = 0x0000;     /* 虚拟产品 */
    rhdev->descriptor.bcdDevice          = 0x0100;
    rhdev->descriptor.bNumConfigurations = 1;

    rhdev->devnum  = 1;
    rhdev->speed   = USB_SPEED_FULL;
    rhdev->state   = USB_STATE_CONFIGURED;
    rhdev->parent  = NULL;
    rhdev->bus     = &hcd->self;
    rhdev->portnum = 0;

    /* 设备名称：usbN */
    snprintf(rhdev->dev.name, DEVICE_NAME_SIZE, "usb%d", hcd->self.busnum);
    INIT_LIST_HEAD(&rhdev->dev.bus_node);
    rhdev->dev.driver = NULL;
    rhdev->dev.driver_data = NULL;

    return rhdev;
}

/* ======================== 公共 API ======================== */

/*
 * usb_create_hcd - 分配并初始化 usb_hcd
 */
struct usb_hcd *usb_create_hcd(const struct hc_driver *driver,
                                struct device *dev,
                                const char *bus_name)
{
    struct usb_hcd *hcd;

    hcd = kmalloc(sizeof(*hcd), GFP_KERNEL);
    if (!hcd) {
        printk("USB: failed to allocate HCD\n");
        return NULL;
    }
    memzero(hcd, sizeof(*hcd));

    hcd->driver = driver;

    /* 初始化 self（usb_bus） */
    hcd->self.busnum    = usb_busnum++;
    hcd->self.controller = dev;
    hcd->self.root_hub = NULL;
    INIT_LIST_HEAD(&hcd->self.bus_list);

    hcd->flags |= HCD_FLAG_HW_ACCESSIBLE;

    printk("USB: created HCD '%s' busnum=%d\n",
           driver->description, hcd->self.busnum);

    return hcd;
}

/*
 * usb_add_hcd - 注册 HCD 到 USB 子系统
 *
 * 流程：
 *   1. 调用 driver->start() 启动硬件
 *   2. 创建 Root Hub usb_device
 *   3. 调用 usb_new_device() 注册到 usb_bus_type
 */
int usb_add_hcd(struct usb_hcd *hcd)
{
    int ret;

    if (!hcd)
        return -1;

    /* 调用 HCD 驱动的 start 回调初始化硬件 */
    if (hcd->driver->start) {
        ret = hcd->driver->start(hcd);
        if (ret < 0) {
            printk("USB: HCD start failed: %d\n", ret);
            return ret;
        }
    }

    /* 创建 Root Hub */
    hcd->root_hub = create_root_hub(hcd);
    if (!hcd->root_hub) {
        if (hcd->driver->stop)
            hcd->driver->stop(hcd);
        return -1;
    }

    hcd->self.root_hub = hcd->root_hub;

    /* 将 Root Hub 注册到 usb_bus_type */
    ret = usb_new_device(hcd->root_hub);
    if (ret < 0) {
        printk("USB: failed to register root hub\n");
        kfree(hcd->root_hub);
        hcd->root_hub = NULL;
        if (hcd->driver->stop)
            hcd->driver->stop(hcd);
        return ret;
    }

    printk("USB: HCD registered, root hub '%s'\n",
           hcd->root_hub->dev.name);

    return 0;
}

/*
 * usb_remove_hcd - 注销 HCD
 */
void usb_remove_hcd(struct usb_hcd *hcd)
{
    if (!hcd)
        return;

    /* 注销 Root Hub */
    if (hcd->root_hub) {
        device_unregister(&hcd->root_hub->dev);
        kfree(hcd->root_hub);
        hcd->root_hub = NULL;
    }

    /* 停止控制器 */
    if (hcd->driver->stop)
        hcd->driver->stop(hcd);

    kfree(hcd);
}
