/*
 * LulaOS USB 核心总线实现
 *
 * 参考 linux-2.6.20 drivers/usb/core/usb.c, drivers/usb/core/driver.c
 *
 * 实现功能：
 *   - 定义 usb_bus_type（match / probe / remove）
 *   - usb_init()：注册总线
 *   - usb_register_driver() / usb_deregister()：驱动注册/注销
 *   - usb_match_id()：基于 usb_device_id 表匹配设备
 *   - usb_new_device()：注册 USB 设备
 */

#include <usb/usb.h>
#include <device/device.h>
#include <printk.h>
#include <libs/vsprintf.h>
#include <stddef.h>

/* ======================== 匹配逻辑 ======================== */

/*
 * usb_match_id - 遍历 id_table 匹配设备
 *
 * 根据 match_flags 决定参与匹配的字段：
 *   VENDOR/PRODUCT：精确匹配
 *   DEV_CLASS/SUBCLASS/PROTOCOL：精确匹配
 * 所有指定字段均匹配才返回该项
 */
static const struct usb_device_id *usb_match_id(
    const struct usb_device_id *id,
    const struct usb_device_descriptor *desc)
{
    if (!id)
        return NULL;

    /* 遍历直到全零终止项 */
    while (id->match_flags || id->idVendor || id->idProduct ||
           id->bDeviceClass || id->bDeviceSubClass || id->bDeviceProtocol) {

        int match = 1;

        if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
            id->idVendor != desc->idVendor)
            match = 0;

        if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
            id->idProduct != desc->idProduct)
            match = 0;

        if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) &&
            id->bDeviceClass != desc->bDeviceClass)
            match = 0;

        if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) &&
            id->bDeviceSubClass != desc->bDeviceSubClass)
            match = 0;

        if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) &&
            id->bDeviceProtocol != desc->bDeviceProtocol)
            match = 0;

        if (match)
            return id;

        id++;
    }

    return NULL;
}

/* ======================== 总线类型回调 ======================== */

/*
 * usb_bus_match - USB 总线匹配函数
 *
 * 检查设备描述符是否匹配驱动的 id_table
 */
static int usb_bus_match(struct device *dev, struct device_driver *drv)
{
    struct usb_device *udev = to_usb_device(dev);
    struct usb_driver *udrv = to_usb_driver(drv);

    return usb_match_id(udrv->id_table, &udev->descriptor) != NULL;
}

/*
 * usb_bus_probe - USB 总线 probe 转发
 *
 * 匹配成功后调用 usb_driver->probe()
 * 注意：简化实现中，直接对 usb_device 调用，不区分 interface
 */
static int usb_bus_probe(struct device *dev)
{
    struct usb_device *udev = to_usb_device(dev);
    struct usb_driver *udrv = to_usb_driver(dev->driver);
    const struct usb_device_id *id;

    id = usb_match_id(udrv->id_table, &udev->descriptor);
    if (!id)
        return -1;

    /*
     * 简化实现：Linux 中 probe 以 usb_interface 为单位，
     * 此处暂以 usb_device 为单位，传入 NULL interface
     */
    if (udrv->probe)
        return udrv->probe(NULL, id);

    return 0;
}

/*
 * usb_bus_remove - USB 总线 remove 转发
 */
static void usb_bus_remove(struct device *dev)
{
    struct usb_driver *udrv = to_usb_driver(dev->driver);

    if (udrv->disconnect)
        udrv->disconnect(NULL);
}

/* ======================== USB 总线类型定义 ======================== */

struct bus_type usb_bus_type = {
    .name   = "usb",
    .match  = usb_bus_match,
    .probe  = usb_bus_probe,
    .remove = usb_bus_remove,
};

/* ======================== 公共 API ======================== */

/*
 * usb_init - 注册 USB 总线类型
 *
 * 在 kernel.c 中 pci_init() 之后调用
 */
void usb_init(void)
{
    bus_register(&usb_bus_type);
    printk("USB: bus registered\n");
}

/*
 * usb_register_driver - 注册 USB 驱动
 *
 * 设置 driver.bus = &usb_bus_type，调用 driver_register() 触发匹配
 */
int usb_register_driver(struct usb_driver *drv)
{
    if (!drv || !drv->driver.name)
        return -1;

    drv->driver.bus = &usb_bus_type;
    INIT_LIST_HEAD(&drv->driver.bus_node);

    return driver_register(&drv->driver);
}

/*
 * usb_deregister - 注销 USB 驱动
 */
void usb_deregister(struct usb_driver *drv)
{
    if (!drv)
        return;

    driver_unregister(&drv->driver);
}

/*
 * usb_new_device - 注册 USB 设备到总线
 *
 * 由 HCD 框架（usb_add_hcd）调用，将设备挂入 usb_bus_type 并触发匹配
 */
int usb_new_device(struct usb_device *dev)
{
    if (!dev)
        return -1;

    dev->dev.bus = &usb_bus_type;
    INIT_LIST_HEAD(&dev->dev.bus_node);

    return device_register(&dev->dev);
}
