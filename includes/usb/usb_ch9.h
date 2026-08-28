/*
 * LulaOS USB Chapter 9 标准描述符定义
 *
 * 参考 linux-2.6.20/include/linux/usb_ch9.h
 * 提取 USB 2.0 规范 Chapter 9 核心常量与结构体
 */

#ifndef __USB_CH9_H__
#define __USB_CH9_H__

/* ======================== 方向 / 类型 / 接收者 ======================== */

/* bmRequestType 方向位 */
#define USB_DIR_OUT         0x00    /* 主机 -> 设备 */
#define USB_DIR_IN          0x80    /* 设备 -> 主机 */

/* bmRequestType 类型位 */
#define USB_TYPE_MASK       (0x03 << 5)
#define USB_TYPE_STANDARD   (0x00 << 5)   /* 标准请求 */
#define USB_TYPE_CLASS      (0x01 << 5)   /* 类请求 */
#define USB_TYPE_VENDOR     (0x02 << 5)   /* 厂商请求 */

/* bmRequestType 接收者位 */
#define USB_RECIP_MASK      0x1F
#define USB_RECIP_DEVICE    0x00    /* 设备 */
#define USB_RECIP_INTERFACE 0x01    /* 接口 */
#define USB_RECIP_ENDPOINT  0x02    /* 端点 */
#define USB_RECIP_OTHER     0x03    /* 其他 */

/* ======================== 标准请求码 ======================== */

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

/* ======================== 描述符类型码 ======================== */

#define USB_DT_DEVICE             0x01
#define USB_DT_CONFIG             0x02
#define USB_DT_STRING             0x03
#define USB_DT_INTERFACE          0x04
#define USB_DT_ENDPOINT           0x05
#define USB_DT_DEVICE_QUALIFIER   0x06
#define USB_DT_OTHER_SPEED_CONFIG 0x07
#define USB_DT_INTERFACE_POWER    0x08
#define USB_DT_HID                0x21
#define USB_DT_HID_REPORT         0x22

/* ======================== 设备类别码 ======================== */

#define USB_CLASS_PER_INTERFACE   0x00    /* 按接口定义 */
#define USB_CLASS_AUDIO           0x01
#define USB_CLASS_COMM            0x02
#define USB_CLASS_HID             0x03
#define USB_CLASS_PHYSICAL        0x05
#define USB_CLASS_STILL_IMAGE     0x06
#define USB_CLASS_PRINTER         0x07
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09
#define USB_CLASS_CDC_DATA        0x0A
#define USB_CLASS_VIDEO           0x0E
#define USB_CLASS_VENDOR_SPEC     0xFF

/* ======================== 端点属性 ======================== */

#define USB_ENDPOINT_XFER_MASK    0x03
#define USB_ENDPOINT_XFER_CONTROL 0x00
#define USB_ENDPOINT_XFER_ISOC    0x01
#define USB_ENDPOINT_XFER_BULK    0x02
#define USB_ENDPOINT_XFER_INT     0x03

/* 端点地址方向位 */
#define USB_ENDPOINT_DIR_MASK     0x80
#define USB_ENDPOINT_NUMBER_MASK  0x0F

/* ======================== 速度枚举 ======================== */

enum usb_device_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,        /* 1.5 Mbps */
    USB_SPEED_FULL,       /* 12 Mbps */
    USB_SPEED_HIGH,       /* 480 Mbps */
};

/* ======================== 设备状态枚举 ======================== */

enum usb_device_state {
    USB_STATE_NOTATTACHED = 0,
    USB_STATE_ATTACHED,
    USB_STATE_POWERED,
    USB_STATE_RECONNECTING,
    USB_STATE_UNAUTHENTICATED,
    USB_STATE_DEFAULT,
    USB_STATE_ADDRESS,
    USB_STATE_CONFIGURED,
    USB_STATE_SUSPENDED,
};

/* ======================== 描述符结构体（packed） ======================== */

/* 设备描述符 */
struct usb_device_descriptor {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short bcdUSB;
    unsigned char  bDeviceClass;
    unsigned char  bDeviceSubClass;
    unsigned char  bDeviceProtocol;
    unsigned char  bMaxPacketSize0;
    unsigned short idVendor;
    unsigned short idProduct;
    unsigned short bcdDevice;
    unsigned char  iManufacturer;
    unsigned char  iProduct;
    unsigned char  iSerialNumber;
    unsigned char  bNumConfigurations;
} __attribute__((packed));

/* 配置描述符 */
struct usb_config_descriptor {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short wTotalLength;
    unsigned char  bNumInterfaces;
    unsigned char  bConfigurationValue;
    unsigned char  iConfiguration;
    unsigned char  bmAttributes;
    unsigned char  bMaxPower;
} __attribute__((packed));

/* 接口描述符 */
struct usb_interface_descriptor {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned char  bInterfaceNumber;
    unsigned char  bAlternateSetting;
    unsigned char  bNumEndpoints;
    unsigned char  bInterfaceClass;
    unsigned char  bInterfaceSubClass;
    unsigned char  bInterfaceProtocol;
    unsigned char  iInterface;
} __attribute__((packed));

/* 端点描述符 */
struct usb_endpoint_descriptor {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned char  bEndpointAddress;
    unsigned char  bmAttributes;
    unsigned short wMaxPacketSize;
    unsigned char  bInterval;
} __attribute__((packed));

/* 标准请求结构（Setup Packet） */
struct usb_ctrlrequest {
    unsigned char  bRequestType;
    unsigned char  bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
} __attribute__((packed));

/* 配置描述符 bmAttributes 位 */
#define USB_CONFIG_ATT_ONE       0x80   /* 必须置位 */
#define USB_CONFIG_ATT_SELFPOWER 0x40
#define USB_CONFIG_ATT_WAKEUP    0x20

#endif /* __USB_CH9_H__ */
