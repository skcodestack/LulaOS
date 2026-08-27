/*
 * LulaOS Platform 总线
 *
 * 参考   include/linux/platform_device.h 和 drivers/base/platform.c
 *
 * Platform 总线用于非 PCI 发现机制的设备，例如：
 *   - SoC 内部外设（UART、GPIO、Timer）
 *   - PS/2 键盘/鼠标控制器
 *   - 中断控制器
 *   - 设备树/ACPI/板级代码中描述的固定地址设备
 *
 * 匹配方式：platform_device.dev.name == platform_driver.driver.name
 */

#ifndef __PLATFORM_DEVICE_H__
#define __PLATFORM_DEVICE_H__

#include <device/device.h>

#define PLATFORM_NAME_SIZE      32
#define PLATFORM_MAX_RESOURCES  6

/* 资源标志扩展 */
#define IORESOURCE_IO       0x00000100
#define IORESOURCE_MEM      0x00000200
#define IORESOURCE_IRQ      0x00000400

/*
 * platform_resource - Platform 设备资源描述
 *
 * 描述设备的 MMIO 范围、I/O 端口范围或 IRQ 号。
 */
struct platform_resource {
    unsigned long start;        /* 起始地址或 IRQ 号 */
    unsigned long end;          /* 结束地址（IRQ 资源与 start 相同） */
    unsigned long flags;        /* IORESOURCE_MEM / IORESOURCE_IO / IORESOURCE_IRQ */
};

/*
 * platform_device - Platform 设备描述符
 *
 * 通过 dev.name 与 platform_driver 匹配。
 */
struct platform_device {
    struct device dev;              /* 必须作为第一个成员嵌入 */
    int id;                         /* 设备实例 ID（-1 表示唯一设备） */
    int num_resources;
    struct platform_resource resource[PLATFORM_MAX_RESOURCES];
};

/*
 * platform_driver - Platform 驱动描述符
 *
 * 通过 driver.name 与 platform_device 匹配。
 */
struct platform_driver {
    struct device_driver driver;    /* 必须作为第一个成员嵌入 */
    int (*probe)(struct platform_device *pdev);
    int (*remove)(struct platform_device *pdev);
};

/* 全局 Platform 总线类型 */
extern struct bus_type platform_bus_type;

/* Platform 总线初始化（注册 bus_type） */
int platform_bus_init(void);

/* 辅助宏 */
#define to_platform_device(d)   container_of(d, struct platform_device, dev)
#define to_platform_driver(d)   container_of(d, struct platform_driver, driver)

/* Platform 设备/驱动注册 */
int platform_device_register(struct platform_device *pdev);
int platform_driver_register(struct platform_driver *pdrv);

/* 按名称查找 Platform 设备 */
struct platform_device *platform_find_device(const char *name);

/* 获取资源 */
struct platform_resource *platform_get_resource(struct platform_device *pdev,
                                                 unsigned long type, int index);

#endif /* __PLATFORM_DEVICE_H__ */
