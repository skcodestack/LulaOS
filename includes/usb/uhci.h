/*
 * LulaOS UHCI 主机控制器驱动头文件
 *
 * 参考 linux-2.6.20 drivers/usb/host/uhci-hcd.h
 *
 * UHCI（Universal Host Controller Interface）寄存器定义与私有结构
 * 适用于 Intel PIIX3/PIIX4/ICH 系列 USB 控制器
 *
 * 寄存器访问方式：I/O 端口（BAR4）
 */

#ifndef __UHCI_H__
#define __UHCI_H__

#include <usb/hcd.h>

/* ======================== I/O 寄存器偏移 ======================== */

#define UHCI_USBCMD         0x00    /* 命令寄存器（16 bit） */
#define UHCI_USBSTS         0x02    /* 状态寄存器（16 bit） */
#define UHCI_USBINTR        0x04    /* 中断使能寄存器（16 bit） */
#define UHCI_USBFRNUM       0x06    /* 帧号寄存器（10 bit 只读） */
#define UHCI_USBFLBASEADD   0x08    /* 帧列表基地址（32 bit，物理地址） */
#define UHCI_USBSOF         0x0C    /* SOF 修改寄存器（8 bit） */
#define UHCI_USBPORTSC1     0x10    /* 端口状态/控制 1（16 bit） */
#define UHCI_USBPORTSC2     0x12    /* 端口状态/控制 2（16 bit） */

/* ======================== USBCMD 位定义 ======================== */

#define USBCMD_RS           0x0001  /* Run/Stop（1=运行，0=停止） */
#define USBCMD_HCRESET      0x0002  /* Host Controller Reset */
#define USBCMD_GRESET       0x0004  /* Global Reset（所有端口复位） */
#define USBCMD_EGSM         0x0008  /* Enter Global Suspend Mode */
#define USBCMD_FGR          0x0010  /* Force Global Resume */
#define USBCMD_SWDBG        0x0020  /* Software Debug */
#define USBCMD_CF           0x0040  /* Configure Flag */
#define USBCMD_MAXP         0x0080  /* Max Packet Size（1=64, 0=32） */

/* ======================== USBSTS 位定义 ======================== */

#define USBSTS_USBINT       0x0001  /* USB 中断（TD 完成） */
#define USBSTS_ERROR        0x0002  /* TD 错误 */
#define USBSTS_RD           0x0004  /* Resume Detect */
#define USBSTS_HSERR        0x0008  /* Host System Error */
#define USBSTS_HCPE         0x0010  /* Host Process Error */
#define USBSTS_HCH          0x0020  /* HC Halted（1=已停止） */

/* ======================== USBINTR 位定义 ======================== */

#define USBINTR_TIMEOUT     0x0001  /* Timeout/CRC 错误中断使能 */
#define USBINTR_RESUME      0x0002  /* Resume 中断使能 */
#define USBINTR_IOC         0x0004  /* Interrupt on Complete 使能 */
#define USBINTR_SP          0x0008  /* Short Packet 中断使能 */

/* ======================== USBPORTSC 位定义 ======================== */

#define USBPORTSC_CCS       0x0001  /* Current Connection Status（1=已连接） */
#define USBPORTSC_CSC       0x0002  /* Connection Status Change */
#define USBPORTSC_PE        0x0004  /* Port Enable */
#define USBPORTSC_PEDC      0x0008  /* Port Enable/Disable Change */
#define USBPORTSC_LS        0x0030  /* Line Status（bit4:5） */
#define USBPORTSC_RD        0x0040  /* Resume Detect */
#define USBPORTSC_LSDA      0x0100  /* Low Speed Device Attached */
#define USBPORTSC_PR        0x0200  /* Port Reset */
#define USBPORTSC_SUSP      0x1000  /* Suspend */

/* ======================== 帧列表相关常量 ======================== */

#define UHCI_NUM_FRAMES     1024    /* 帧列表大小（1024 项，1ms 帧） */
#define UHCI_FRAME_ALIGN    4096    /* 帧列表必须 4KB 对齐 */

/* Frame List Entry：bit0=1 表示无效（T=Terminate） */
#define UHCI_FL_TERMINATE   0x00000001
#define UHCI_FL_QH          0x00000002  /* bit1=1: QH，bit1=0: TD */

/* ======================== TD 状态位 ======================== */

#define TD_CTRL_ACTLEN      0x007F  /* Actual Length（bit6:0） */
#define TD_CTRL_STALLED     0x0040  /* Stalled */
#define TD_CTRL_DBUFERR     0x0080  /* Data Buffer Error */
#define TD_CTRL_BABBLE      0x0100  /* Babble Detected */
#define TD_CTRL_NAK         0x0200  /* NAK Received */
#define TD_CTRL_CRCTIMEO    0x0400  /* CRC/Timeout Error */
#define TD_CTRL_BITSTUFF    0x0800  /* Bitstuff Error */
#define TD_CTRL_ACTIVE      0x80000000  /* TD Active（HCD 拥有） */

/* ======================== TD 令牌位 ======================== */

#define TD_TOKEN_PID_SETUP  0x0000002D  /* SETUP PID */
#define TD_TOKEN_PID_IN     0x00000069  /* IN PID */
#define TD_TOKEN_PID_OUT    0x000000E1  /* OUT PID */

/* ======================== 数据结构 ======================== */

/*
 * uhci_td - UHCI Transfer Descriptor（32 字节对齐）
 *
 * 硬件直接使用此结构，必须物理地址对齐
 */
struct uhci_td {
    unsigned int link;             /* 下一个 TD/QH 物理地址（含 T/QH 位） */
    unsigned int cs_status;        /* Control/Status 字段 */
    unsigned int token;            /* Token 字段（PID/DevAddr/Endpoint/Data） */
    unsigned int buffer;           /* 数据缓冲区物理地址 */

    /* 软件字段（硬件不访问） */
    struct uhci_td *next;          /* 逻辑链表 */
    void *priv;                    /* 软件私有数据（URB 指针等） */
} __attribute__((aligned(16)));

/*
 * uhci_qh - UHCI Queue Head（16 字节对齐）
 *
 * 硬件直接使用此结构，必须物理地址对齐
 */
struct uhci_qh {
    unsigned int link;             /* 下一个 QH 物理地址 */
    unsigned int element;          /* 当前 TD 物理地址 */

    /* 软件字段（硬件不访问） */
    struct uhci_qh *next_qh;      /* 逻辑 QH 链表 */
    struct uhci_td *first_td;     /* 第一个 TD */
} __attribute__((aligned(16)));

/*
 * uhci_hcd - UHCI HCD 私有数据
 *
 * 嵌入在 usb_hcd->hcd_priv 中
 */
struct uhci_hcd {
    /* 硬件信息 */
    unsigned long io_base;         /* I/O 端口基址（BAR4） */

    /* 帧列表（1024 项，每项 4 字节，4KB 对齐） */
    unsigned int *frame_list;      /* 虚拟地址（指向帧列表） */

    /* QH/TD 池（简化实现，静态分配） */
    struct uhci_qh *qh_pool;      /* QH 池 */
    struct uhci_td *td_pool;      /* TD 池 */
    unsigned int qh_pool_size;    /* QH 池大小 */
    unsigned int td_pool_size;    /* TD 池大小 */

    /* 控制器状态 */
    unsigned int rh_num_ports;    /* Root Hub 端口数（通常 2） */
    int is_stopped;               /* 1 = 已停止 */
};

/* ======================== UHCI HCD 公共 API ======================== */

/*
 * uhci_init - UHCI 子系统初始化
 *
 * 注册 PCI 驱动，自动发现并绑定 UHCI 控制器
 * 在 kernel.c 中 usb_init() 之后调用
 */
void uhci_init(void);

#endif /* __UHCI_H__ */
