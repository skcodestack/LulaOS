/*
 * LulaOS USB 核心类型定义
 *
 * 参考 linux-2.6.20/include/linux/usb.h
 * 定义 USB 子系统的核心结构体：
 *   usb_device / usb_driver / usb_bus / usb_device_id 等
 */

#ifndef __USB_H__
#define __USB_H__

#include <libs/list.h>
#include <device/device.h>
#include <usb/usb_ch9.h>

/* ======================== 常量 ======================== */

#define USB_MAX_INTERFACES     8     /* 每配置最大接口数（简化） */
#define USB_MAX_ENDPOINTS      16    /* 每接口最大端点数（简化） */
#define USB_MAX_CHILDREN       8     /* Root Hub 最大子端口（简化） */

/* usb_device_id 匹配标志 */
#define USB_DEVICE_ID_MATCH_VENDOR       0x0001
#define USB_DEVICE_ID_MATCH_PRODUCT      0x0002
#define USB_DEVICE_ID_MATCH_DEV_CLASS    0x0004
#define USB_DEVICE_ID_MATCH_DEV_SUBCLASS 0x0008
#define USB_DEVICE_ID_MATCH_DEV_PROTOCOL 0x0010

/* ======================== Pipe 宏 ======================== */

/*
 * Pipe：将端点地址和方向编码为整数
 * bit 31:   方向 (1=IN, 0=OUT)
 * bit 30:28 传输类型 (0=ctrl, 1=isoc, 2=bulk, 3=int)
 * bit 7:0   端点地址
 *
 * UHCI/OHCI 使用 pipe 抽象，此处简化实现
 */
#define PIPE_CONTROL      0
#define PIPE_ISOCHRONOUS  1
#define PIPE_BULK         2
#define PIPE_INTERRUPT    3

#define usb_sndctrlpipe(dev, ep)  ((PIPE_CONTROL   << 28) | (ep))
#define usb_rcvctrlpipe(dev, ep)  ((PIPE_CONTROL   << 28) | (1 << 31) | (ep))
#define usb_sndbulkpipe(dev, ep)  ((PIPE_BULK      << 28) | (ep))
#define usb_rcvbulkpipe(dev, ep)  ((PIPE_BULK      << 28) | (1 << 31) | (ep))
#define usb_sndintpipe(dev, ep)   ((PIPE_INTERRUPT << 28) | (ep))
#define usb_rcvintpipe(dev, ep)   ((PIPE_INTERRUPT << 28) | (1 << 31) | (ep))

/* ======================== 数据结构 ======================== */

/*
 * usb_host_endpoint - 主机侧端点描述
 *
 * 嵌入 USB 标准端点描述符，可附加 HCD 私有数据
 */
struct usb_host_endpoint {
    struct usb_endpoint_descriptor desc;
    void *hcpriv;                      /* HCD 私有数据（QH/TD 等） */
};

/*
 * usb_host_interface - 主机侧接口描述
 *
 * 嵌入标准接口描述符 + 端点数组指针
 */
struct usb_host_interface {
    struct usb_interface_descriptor desc;
    struct usb_host_endpoint *endpoint;   /* 该接口所有端点 */
};

/*
 * usb_interface - USB 接口设备
 *
 * 一个 usb_device 可包含多个 usb_interface，
 * 每个 usb_interface 可嵌入 struct device 挂入设备模型
 */
struct usb_interface {
    struct usb_host_interface *altsetting;  /* 可选设置数组 */
    unsigned int num_altsetting;            /* 可选设置数量 */
    unsigned int cur_altsetting;            /* 当前激活的 altsetting 索引 */
    struct device dev;                      /* 嵌入统一设备模型 */
};

/*
 * usb_host_config - 主机侧配置描述
 *
 * 嵌入标准配置描述符 + 接口指针数组
 */
struct usb_host_config {
    struct usb_config_descriptor desc;
    struct usb_interface *interface[USB_MAX_INTERFACES];
};

/*
 * usb_bus - USB 总线描述
 *
 * 每条 USB 主机控制器对应一个 usb_bus
 */
struct usb_bus {
    int busnum;                             /* 总线编号 */
    struct device *controller;              /* 主机控制器设备 */
    struct usb_device *root_hub;            /* Root Hub 设备指针 */
    struct list_head bus_list;              /* 链入全局 USB 总线链表 */
};

/*
 * usb_device - USB 设备描述
 *
 * 参考 Linux 2.6.20 struct usb_device（简化版）
 * 嵌入 struct device 作为第一个成员，以便挂入统一设备模型
 */
struct usb_device {
    struct device dev;                      /* 必须作为第一个成员 */

    int devnum;                             /* 总线上的设备地址（1~127） */
    enum usb_device_speed speed;            /* 速度：LOW / FULL / HIGH */
    enum usb_device_state state;            /* 当前状态 */
    struct usb_device *parent;              /* 父设备（Hub 或 Root Hub） */
    struct usb_bus *bus;                    /* 所属 USB 总线 */

    struct usb_device_descriptor descriptor;      /* 设备描述符 */
    struct usb_host_config *config;               /* 所有配置数组 */
    struct usb_host_config *actconfig;            /* 当前激活的配置 */

    unsigned char portnum;                  /* 连接到的父 Hub 端口号 */
    unsigned int children;                  /* 子设备数（Hub 有效） */
    struct usb_device *child[USB_MAX_CHILDREN]; /* 子设备指针（Hub 有效） */
};

/*
 * usb_device_id - 设备匹配 ID 表项
 *
 * 驱动通过 id_table 数组声明支持的设备列表，以 {} 结尾
 * match_flags 指定参与匹配的字段
 */
struct usb_device_id {
    unsigned int  match_flags;              /* 匹配字段掩码 */
    unsigned short idVendor;                /* 厂商 ID（0=任意） */
    unsigned short idProduct;               /* 产品 ID（0=任意） */
    unsigned char  bDeviceClass;            /* 设备类别 */
    unsigned char  bDeviceSubClass;         /* 设备子类别 */
    unsigned char  bDeviceProtocol;         /* 设备协议 */
};

/*
 * usb_driver - USB 驱动描述符
 *
 * 嵌入 struct device_driver 作为第一个成员
 */
struct usb_driver {
    struct device_driver driver;            /* 必须作为第一个成员 */
    const struct usb_device_id *id_table;   /* 支持的设备 ID 表（{}结尾） */

    int  (*probe)(struct usb_interface *intf, const struct usb_device_id *id);
    void (*disconnect)(struct usb_interface *intf);
};

/* ======================== 辅助宏 ======================== */

#define to_usb_device(d)    container_of(d, struct usb_device, dev)
#define to_usb_driver(d)    container_of(d, struct usb_driver, driver)

/* ======================== 公共 API ======================== */

/* USB 总线类型（供统一设备模型使用） */
extern struct bus_type usb_bus_type;

/* 总线初始化（注册 usb_bus_type） */
void usb_init(void);

/* 驱动注册/注销 */
int  usb_register_driver(struct usb_driver *drv);
void usb_deregister(struct usb_driver *drv);

/* 设备注册（由 HCD 框架调用） */
int  usb_new_device(struct usb_device *dev);

#endif /* __USB_H__ */
