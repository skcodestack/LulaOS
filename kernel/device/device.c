/*
 * LulaOS 统一设备模型核心实现
 *
 * 参考  drivers/base/bus.c, drivers/base/core.c
 *
 * 提供 bus_type / device / device_driver 的注册、注销和匹配机制。
 */

#include <device/device.h>
#include <printk.h>
#include <stddef.h>

/* 全局总线链表 */
static LIST_HEAD(bus_list);

/*
 * bus_register - 注册一条总线类型
 *
 * 初始化总线的设备链表和驱动链表，加入全局总线链表。
 * 通常在子系统初始化时调用一次。
 */
int bus_register(struct bus_type *bus)
{
    if (!bus || !bus->name)
        return -1;

    INIT_LIST_HEAD(&bus->devices);
    INIT_LIST_HEAD(&bus->drivers);
    INIT_LIST_HEAD(&bus->list);
    list_add_tail(&bus->list, &bus_list);

    printk("bus: registered '%s'\n", bus->name);
    return 0;
}

/*
 * device_attach - 尝试将设备绑定到某个已注册驱动
 *
 * 遍历该总线上的所有驱动，调用 bus_type->match()。
 * 匹配成功后调用 bus_type->probe()，并记录驱动指针。
 */
static int device_attach(struct device *dev)
{
    struct list_head *pos;

    if (!dev || !dev->bus)
        return -1;

    list_for_each(pos, &dev->bus->drivers) {
        struct device_driver *drv = list_entry(pos, struct device_driver, bus_node);

        if (dev->bus->match && dev->bus->match(dev, drv)) {
            dev->driver = drv;
            if (dev->bus->probe)
                dev->bus->probe(dev);
            return 0;
        }
    }
    return -1;
}

/*
 * driver_attach - 尝试将驱动绑定到所有已注册设备
 *
 * 遍历该总线上的所有设备，调用 bus_type->match()。
 * 匹配成功后调用 bus_type->probe()。
 */
static void driver_attach(struct device_driver *drv)
{
    struct list_head *pos;

    if (!drv || !drv->bus)
        return;

    list_for_each(pos, &drv->bus->devices) {
        struct device *dev = list_entry(pos, struct device, bus_node);

        if (dev->driver)
            continue;   /* 已绑定，跳过 */

        if (drv->bus->match && drv->bus->match(dev, drv)) {
            dev->driver = drv;
            if (drv->bus->probe)
                drv->bus->probe(dev);
        }
    }
}

/*
 * device_register - 注册设备
 *
 * 将设备加入所属总线的设备链表，然后尝试匹配已注册驱动。
 */
int device_register(struct device *dev)
{
    if (!dev || !dev->bus)
        return -1;

    INIT_LIST_HEAD(&dev->bus_node);
    list_add_tail(&dev->bus_node, &dev->bus->devices);
    dev->driver = NULL;

    device_attach(dev);
    return 0;
}

/*
 * device_unregister - 注销设备
 *
 * 从总线设备链表中移除。若已绑定驱动且总线提供 remove 回调，
 * 则调用 remove。
 */
void device_unregister(struct device *dev)
{
    if (!dev)
        return;

    if (dev->driver && dev->bus && dev->bus->remove)
        dev->bus->remove(dev);

    list_del(&dev->bus_node);
    dev->driver = NULL;
}

/*
 * driver_register - 注册驱动
 *
 * 将驱动加入所属总线的驱动链表，然后尝试匹配所有已注册设备。
 */
int driver_register(struct device_driver *drv)
{
    if (!drv || !drv->bus)
        return -1;

    INIT_LIST_HEAD(&drv->bus_node);
    list_add_tail(&drv->bus_node, &drv->bus->drivers);

    driver_attach(drv);
    return 0;
}

/*
 * driver_unregister - 注销驱动
 *
 * 从总线驱动链表中移除，并解除所有绑定该驱动的设备。
 */
void driver_unregister(struct device_driver *drv)
{
    struct list_head *pos, *n;

    if (!drv || !drv->bus)
        return;

    list_for_each_safe(pos, n, &drv->bus->devices) {
        struct device *dev = list_entry(pos, struct device, bus_node);
        if (dev->driver == drv) {
            if (drv->bus->remove)
                drv->bus->remove(dev);
            dev->driver = NULL;
        }
    }

    list_del(&drv->bus_node);
}
