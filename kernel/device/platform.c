/*
 * LulaOS Platform 总线实现
 *
 * 参考   drivers/base/platform.c
 *
 * Platform 总线用于描述非 PCI 设备，通过设备名称字符串匹配设备和驱动。
 */

#include <device/platform.h>
#include <device/device.h>
#include <libs/string.h>
#include <printk.h>

/* Platform 总线实例 */
struct bus_type platform_bus_type = {
    .name = "platform",
};

/*
 * platform_match - Platform 总线匹配函数
 *
 * 比较 platform_device 的名称和 platform_driver 的名称，
 * 相同则匹配成功。
 */
static int platform_match(struct device *dev, struct device_driver *drv)
{
    struct platform_device *pdev = to_platform_device(dev);

    return strcmp(pdev->dev.name, drv->name) == 0;
}

/*
 * platform_probe - Platform 总线 probe 转发
 *
 * 将统一设备模型的 probe 调用转发到 platform_driver->probe()。
 */
static int platform_probe(struct device *dev)
{
    struct platform_driver *pdrv = to_platform_driver(dev->driver);
    struct platform_device *pdev = to_platform_device(dev);

    if (pdrv->probe) {
        int ret = pdrv->probe(pdev);
        if (ret == 0)
            printk("platform: driver '%s' bound to '%s'\n",
                   pdrv->driver.name, pdev->dev.name);
        return ret;
    }
    return 0;
}

/*
 * platform_remove - Platform 总线 remove 转发
 */
static void platform_remove(struct device *dev)
{
    struct platform_driver *pdrv = to_platform_driver(dev->driver);
    struct platform_device *pdev = to_platform_device(dev);

    if (pdrv->remove)
        pdrv->remove(pdev);
}

/*
 * platform_bus_init - 注册 Platform 总线
 *
 * 在调用 platform_device_register / platform_driver_register 之前
 * 必须先调用此函数。通常由 kernel/kernel.c 在初始化早期调用。
 */
int platform_bus_init(void)
{
    platform_bus_type.match = platform_match;
    platform_bus_type.probe = platform_probe;
    platform_bus_type.remove = platform_remove;

    return bus_register(&platform_bus_type);
}

/*
 * platform_device_register - 注册 Platform 设备
 *
 * 设置设备的所属总线，然后调用 device_register() 触发匹配。
 */
int platform_device_register(struct platform_device *pdev)
{
    if (!pdev)
        return -1;

    pdev->dev.bus = &platform_bus_type;
    INIT_LIST_HEAD(&pdev->dev.bus_node);

    if (!pdev->dev.name[0]) {
        printk("platform: device has no name\n");
        return -1;
    }

    return device_register(&pdev->dev);
}

/*
 * platform_driver_register - 注册 Platform 驱动
 *
 * 设置驱动的所属总线，然后调用 driver_register() 触发匹配。
 */
int platform_driver_register(struct platform_driver *pdrv)
{
    if (!pdrv || !pdrv->driver.name)
        return -1;

    pdrv->driver.bus = &platform_bus_type;
    INIT_LIST_HEAD(&pdrv->driver.bus_node);

    return driver_register(&pdrv->driver);
}

/*
 * platform_find_device - 按名称查找 Platform 设备
 */
struct platform_device *platform_find_device(const char *name)
{
    struct list_head *pos;

    if (!name)
        return NULL;

    list_for_each(pos, &platform_bus_type.devices) {
        struct device *dev = list_entry(pos, struct device, bus_node);
        struct platform_device *pdev = to_platform_device(dev);

        if (strcmp(pdev->dev.name, name) == 0)
            return pdev;
    }
    return NULL;
}

/*
 * platform_get_resource - 获取指定类型和序号的资源
 *
 * type: IORESOURCE_MEM / IORESOURCE_IO / IORESOURCE_IRQ
 * index: 该类型资源的第几个（0 开始）
 */
struct platform_resource *platform_get_resource(struct platform_device *pdev,
                                                 unsigned long type, int index)
{
    int i;
    int found = 0;

    if (!pdev)
        return NULL;

    for (i = 0; i < pdev->num_resources; i++) {
        if (pdev->resource[i].flags == type) {
            if (found == index)
                return &pdev->resource[i];
            found++;
        }
    }
    return NULL;
}
