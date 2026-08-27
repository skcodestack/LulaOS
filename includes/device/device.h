/*
 * LulaOS 统一设备模型
 *
 * 参考   drivers/base/ 实现简化版统一设备模型：
 *   - bus_type：总线类型抽象，定义该总线下的设备/驱动匹配规则
 *   - device：设备基础结构，各总线扩展（如 pci_dev、platform_device）
 *   - device_driver：驱动基础结构，各总线扩展（如 pci_driver、platform_driver）
 *
 * 核心流程：
 *   device_register() / driver_register() 将设备/驱动加入其所属总线，
 *   并调用 bus_type->match() 尝试匹配，匹配成功后调用 bus_type->probe()。
 */

#ifndef __DEVICE_H__
#define __DEVICE_H__

#include <libs/list.h>

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))
#endif

#define DEVICE_NAME_SIZE  32

/*
 * bus_type - 总线类型抽象
 *
 * 每种总线（PCI / Platform / USB）各有一个 bus_type 实例，
 * 定义该总线特有的 match/probe/remove 行为。
 */
struct bus_type {
    const char *name;                       /* 总线名称，如 "pci" / "platform" */
    struct list_head list;                  /* 链入全局 bus_list */
    struct list_head devices;               /* 该总线上的设备链表 */
    struct list_head drivers;               /* 该总线上的驱动链表 */

    int  (*match)(struct device *dev, struct device_driver *drv);
    int  (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
};

/*
 * device - 设备基础结构
 *
 * 各总线的设备描述符必须将该结构作为第一个成员嵌入，
 * 以便通过 container_of 从 struct device * 获取具体总线设备。
 */
struct device {
    char name[DEVICE_NAME_SIZE];            /* 设备名称 */
    struct bus_type *bus;                   /* 所属总线 */
    struct list_head bus_node;              /* 链入 bus->devices */
    struct device_driver *driver;           /* 已绑定的驱动（匹配成功后设置） */
    void *driver_data;                      /* 驱动私有数据 */
};

/*
 * device_driver - 驱动基础结构
 *
 * 各总线的驱动描述符必须将该结构作为第一个成员嵌入。
 */
struct device_driver {
    const char *name;                       /* 驱动名称 */
    struct bus_type *bus;                   /* 所属总线 */
    struct list_head bus_node;              /* 链入 bus->drivers */
};

/* 总线注册/注销 */
int  bus_register(struct bus_type *bus);

/* 设备注册/注销（注册时会触发匹配） */
int  device_register(struct device *dev);
void device_unregister(struct device *dev);

/* 驱动注册/注销（注册时会触发匹配） */
int  driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);

#endif /* __DEVICE_H__ */
