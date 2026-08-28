/*
 * LulaOS ACPI DSDT 设备扫描
 *
 * 实现最小 AML 遍历器，扫描 DSDT 命名空间结构：
 *   - Scope、Device、Name 对象
 *   - 提取 _HID、_CID、_CRS 属性
 *   - 将 _CRS 资源描述符转为 platform_resource
 *   - 注册为 Platform 设备
 *
 * 注：不执行 Method，只做只读扫描。
 */

#include <device/platform.h>
#include <arch/x86/acpi.h>
#include <arch/x86/page.h>
#include <arch/x86/highmem.h>
#include <printk.h>
#include <libs/string.h>
#include <libs/memcpy.h>
#include <stddef.h>

/* AML 常用 opcode */
#define AML_ZERO_OP           0x00
#define AML_BYTE_PREFIX       0x0A
#define AML_WORD_PREFIX       0x0B
#define AML_DWORD_PREFIX      0x0C
#define AML_STRING_PREFIX     0x0D
#define AML_SCOPE_OP          0x10
#define AML_BUFFER_OP         0x11
#define AML_PACKAGE_OP        0x12
#define AML_METHOD_OP         0x14
#define AML_NAME_OP           0x08
#define AML_ALIAS_OP          0x06
#define AML_EXT_OP_PREFIX     0x5B
#define AML_MUTEX_OP          0x01
#define AML_EVENT_OP          0x02
#define AML_CONDREFOF_OP      0x12
#define AML_CREATEFIELD_OP    0x13
#define AML_DEVICE_OP         0x82
#define AML_PROCESSOR_OP      0x83
#define AML_POWER_RES_OP      0x84
#define AML_THERMAL_ZONE_OP   0x85
#define AML_FIELD_OP          0x86
#define AML_INDEXFIELD_OP     0x87
#define AML_BANKFIELD_OP      0x88

/* NameChar 范围 */
#define AML_LEADNAMECHAR(c)   (((c) >= 'A' && (c) <= 'Z') || (c) == '_')
#define AML_DIGITCHAR(c)      ((c) >= '0' && (c) <= '9')
#define AML_NAMECHAR(c)       (AML_LEADNAMECHAR(c) || AML_DIGITCHAR(c))

/* DualNamePrefix / MultiNamePrefix */
#define AML_DUALNAME_PREFIX   0x2E
#define AML_MULTI_PREFIX      0x2F
#define AML_ROOT_CHAR         0x5C
#define AML_PARENT_CHAR       0x5E

/* 资源标志（与 platform.h 保持一致） */
#define AML_RESOURCE_IO       0x00000100
#define AML_RESOURCE_MEM      0x00000200
#define AML_RESOURCE_IRQ      0x00000400

/* 小型/大型资源描述符 tag */
#define AML_SMALL_IRQ         0x20   /* IRQ (bit7=0, type=4) */
#define AML_SMALL_ENDTAG      0x78   /* End Tag */
#define AML_LARGE_PREFIX      0x7F   /* 0x7F 表示大资源 */
#define AML_LARGE_IO_PORT     0x47   /* IO Port descriptor */
#define AML_LARGE_MEM32       0x81   /* 32-bit Memory descriptor */
#define AML_LARGE_IRQ_EXT     0x89   /* Extended IRQ */

/* 扫描栈深度 */
#define AML_MAX_DEPTH         16
#define AML_PATH_MAX          64

/* 设备扫描上下文 */
struct aml_scan_ctx {
    const uint8_t *base;            /* DSDT AML 起始地址 */
    const uint8_t *end;             /* DSDT AML 结束地址 */
    const uint8_t *cur;             /* 当前扫描位置 */

    /* 路径栈 */
    char path[AML_PATH_MAX];
    int  depth;

    /* 当前设备信息 */
    char cur_dev_name[ACPI_DEV_NAME_SIZE];
    char cur_hid[ACPI_HID_SIZE];
    char cur_cid[ACPI_HID_SIZE];
    int  cur_num_res;
    struct acpi_resource_info cur_res[ACPI_MAX_CRS_RESOURCES];
    int  in_device;
};

/* ======================== 辅助函数 ======================== */

static uint16_t aml_read16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t aml_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 将 ACPI EISA ID 整数解码为 "PNPxxxx" 字符串 */
static void acpi_eisa_id_to_str(uint32_t id, char *buf, int buflen)
{
    char vendor[4];
    uint16_t prod;

    if (buflen < 8)
        return;

    /*
     * EISA ID 编码（32 位）：
     *   bits[31:26] = char1 (A=0, B=1, ...)
     *   bits[25:21] = char2
     *   bits[20:16] = char3
     *   bits[15:12] = hex digit 1
     *   bits[11:8]  = hex digit 2
     *   bits[7:4]   = hex digit 3
     *   bits[3:0]   = hex digit 4
     */
    vendor[0] = 'A' + ((id >> 26) & 0x1F);
    vendor[1] = 'A' + ((id >> 21) & 0x1F);
    vendor[2] = 'A' + ((id >> 16) & 0x1F);
    vendor[3] = '\0';

    prod = (uint16_t)(id & 0xFFFF);

    /* 格式化：vendor + 4 位 16 进制 */
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = vendor[0];
    buf[1] = vendor[1];
    buf[2] = vendor[2];
    buf[3] = hex[(prod >> 12) & 0xF];
    buf[4] = hex[(prod >> 8) & 0xF];
    buf[5] = hex[(prod >> 4) & 0xF];
    buf[6] = hex[prod & 0xF];
    buf[7] = '\0';
}

/* ======================== AML NameString 解析 ======================== */

/*
 * 解析一个 NameSeg（4 字节），写入 buf
 * 返回消耗的字节数（4）
 */
static int aml_parse_name_seg(const uint8_t *p, char *buf, int buflen)
{
    int i;

    if (buflen < 5)
        return 4;

    for (i = 0; i < 4 && AML_NAMECHAR(p[i]); i++)
        buf[i] = p[i];
    buf[i] = '\0';

    /* 截断尾部空格/下划线（ACPI 名称不足 4 字符用 '_' 填充） */
    while (i > 0 && (buf[i - 1] == '_' || buf[i - 1] == '\0'))
        buf[--i] = '\0';
    if (i == 0)
        buf[0] = '\0';

    return 4;
}

/*
 * 解析 NameString，返回完整路径片段
 * 返回消耗的字节数
 */
static int aml_parse_namestring(const uint8_t *p, char *buf, int buflen)
{
    const uint8_t *start = p;
    int pos = 0;

    buf[0] = '\0';

    /* 根前缀 */
    if (*p == AML_ROOT_CHAR) {
        if (pos + 2 < buflen) {
            buf[pos++] = '\\';
            buf[pos] = '\0';
        }
        p++;
    }

    /* 父前缀（多个 ^） */
    while (*p == AML_PARENT_CHAR) {
        if (pos + 2 < buflen) {
            buf[pos++] = '^';
            buf[pos] = '\0';
        }
        p++;
    }

    /* NullName */
    if (*p == 0x00) {
        p++;
        return (int)(p - start);
    }

    /* DualName */
    if (*p == AML_DUALNAME_PREFIX) {
        p++;
        pos += aml_parse_name_seg(p, buf + pos, buflen - pos);
        p += 4;
        if (pos + 2 < buflen) {
            buf[pos++] = '.';
            buf[pos] = '\0';
        }
        pos += aml_parse_name_seg(p, buf + pos, buflen - pos);
        p += 4;
        return (int)(p - start);
    }

    /* MultiName */
    if (*p == AML_MULTI_PREFIX) {
        uint8_t count;
        p++;
        count = *p++;
        while (count--) {
            pos += aml_parse_name_seg(p, buf + pos, buflen - pos);
            p += 4;
            if (count > 0 && pos + 2 < buflen) {
                buf[pos++] = '.';
                buf[pos] = '\0';
            }
        }
        return (int)(p - start);
    }

    /* 单个 NameSeg */
    pos += aml_parse_name_seg(p, buf + pos, buflen - pos);
    p += 4;
    return (int)(p - start);
}

/* ======================== 数据项跳过 ======================== */

/*
 * 跳过 AML 数据项（TermArg / DataObject），返回消耗的字节数。
 * 用于跳过我们不需要解析的 Name 值。
 */
static int aml_skip_data_object(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *start = p;
    uint8_t op;
    uint16_t len16;
    uint32_t len32;

    if (p >= end)
        return 0;

    op = *p;

    /* 简单整数 */
    switch (op) {
    case AML_ZERO_OP:
        return 1;
    case AML_BYTE_PREFIX:
        return 2;
    case AML_WORD_PREFIX:
        return 3;
    case AML_DWORD_PREFIX:
        return 5;
    case AML_STRING_PREFIX:
        /* 字符串：跳过到 '\0' */
        p++;
        while (p < end && *p != '\0')
            p++;
        p++;   /* 跳过 '\0' */
        return (int)(p - start);
    }

    /* Buffer */
    if (op == AML_BUFFER_OP) {
        p++;
        /* 后跟 PkgLength + BufferSize */
        /* PkgLength 编码复杂，简化处理 */
        uint8_t lead = *p;
        if ((lead & 0xC0) == 0) {
            /* 1 字节长度 */
            len32 = lead;
            p++;
        } else {
            int n_bytes = (lead >> 6) & 0x3;
            len32 = lead & 0x0F;
            p++;
            for (int i = 0; i < n_bytes; i++) {
                len32 |= ((uint32_t)*p) << (4 + i * 8);
                p++;
            }
        }
        /* 跳过 PkgLength 剩余内容 */
        /* 这里简单返回 lead + len */
        return (int)(p - start) + (int)len32 - (int)(p - start - 1);
    }

    /* Package */
    if (op == AML_PACKAGE_OP) {
        p++;
        /* PkgLength + NumElements */
        uint8_t lead = *p;
        if ((lead & 0xC0) == 0) {
            len32 = lead;
            p++;
        } else {
            int n_bytes = (lead >> 6) & 0x3;
            len32 = lead & 0x0F;
            p++;
            for (int i = 0; i < n_bytes; i++) {
                len32 |= ((uint32_t)*p) << (4 + i * 8);
                p++;
            }
        }
        p++; /* NumElements */
        return (int)(p - start) + (int)len32 - (int)(p - start - 1);
    }

    /* 其他情况，保守返回 1 */
    return 1;
}

/* ======================== _CRS 资源解析 ======================== */

/*
 * 解析 _CRS Buffer 中的资源描述符
 * 将 IRQ/I/O/Memory 描述符转为 acpi_resource_info
 */
static int acpi_parse_crs_buffer(const uint8_t *buf, int buflen,
                                  struct acpi_resource_info *res, int max_res)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + buflen;
    int count = 0;

    while (p < end && count < max_res) {
        if (*p == AML_SMALL_ENDTAG)
            break;

        if ((*p & 0x80) == 0) {
            /* 小型资源描述符 */
            uint8_t type = (*p >> 3) & 0x0F;
            uint8_t len = *p & 0x07;
            p++;

            switch (type) {
            case 4: /* IRQ */
                if (len >= 2) {
                    uint16_t irq_mask = aml_read16(p);
                    int irq = -1;
                    for (int i = 0; i < 16; i++) {
                        if (irq_mask & (1 << i)) {
                            irq = i;
                            break;
                        }
                    }
                    if (irq >= 0) {
                        res[count].start = irq;
                        res[count].end = irq;
                        res[count].flags = AML_RESOURCE_IRQ;
                        count++;
                    }
                }
                break;
            case 8: /* I/O Port */
                if (len >= 7) {
                    uint16_t min_addr = aml_read16(p + 1);
                    uint16_t max_addr = aml_read16(p + 3);
                    uint8_t length = p[5];
                    res[count].start = min_addr;
                    res[count].end = max_addr + length - 1;
                    res[count].flags = AML_RESOURCE_IO;
                    count++;
                }
                break;
            }

            p += len;
        } else {
            /* 大型资源描述符 */
            uint8_t type = *p & 0x7F;
            p++;
            uint16_t len = aml_read16(p);
            p += 2;

            switch (type) {
            case AML_LARGE_IO_PORT: /* I/O Port */
                if (len >= 7) {
                    uint16_t min_addr = aml_read16(p + 2);
                    uint16_t max_addr = aml_read16(p + 4);
                    uint8_t length = p[6];
                    res[count].start = min_addr;
                    res[count].end = max_addr + length - 1;
                    res[count].flags = AML_RESOURCE_IO;
                    count++;
                }
                break;
            case AML_LARGE_MEM32: /* 32-bit Memory */
                if (len >= 17) {
                    uint32_t min_addr = aml_read32(p + 4);
                    uint32_t max_addr = aml_read32(p + 8);
                    uint32_t length = aml_read32(p + 16);
                    res[count].start = min_addr;
                    res[count].end = max_addr + length - 1;
                    res[count].flags = AML_RESOURCE_MEM;
                    count++;
                }
                break;
            }

            p += len;
        }
    }

    return count;
}

/* ======================== 设备节点扫描 ======================== */

/*
 * 在 Device 对象内扫描属性（_HID, _CID, _CRS 等）
 * p 指向 Device 对象的 PkgLength 起始处
 */
static void aml_scan_device_attrs(struct aml_scan_ctx *ctx,
                                   const uint8_t *dev_start,
                                   const uint8_t *dev_end)
{
    const uint8_t *p = dev_start;

    /* 重置当前设备信息 */
    ctx->cur_dev_name[0] = '\0';
    ctx->cur_hid[0] = '\0';
    ctx->cur_cid[0] = '\0';
    ctx->cur_num_res = 0;

    /* 读取 PkgLength */
    uint8_t lead = *p;
    uint32_t pkg_len;
    if ((lead & 0xC0) == 0) {
        pkg_len = lead;
        p++;
    } else {
        int n_bytes = (lead >> 6) & 0x3;
        pkg_len = lead & 0x0F;
        p++;
        for (int i = 0; i < n_bytes; i++) {
            pkg_len |= ((uint32_t)*p) << (4 + i * 8);
            p++;
        }
    }

    const uint8_t *body_start = p;
    const uint8_t *real_end = body_start + pkg_len - (body_start - dev_start);
    if (real_end > dev_end)
        real_end = dev_end;

    /* 读取 Device NameString */
    char dev_name[ACPI_DEV_NAME_SIZE];
    int consumed = aml_parse_namestring(p, dev_name, sizeof(dev_name));
    p += consumed;

    /* 复制设备短名到上下文 */
    strncpy(ctx->cur_dev_name, dev_name, ACPI_DEV_NAME_SIZE - 1);
    ctx->cur_dev_name[ACPI_DEV_NAME_SIZE - 1] = '\0';

    ctx->in_device = 1;

    /* 扫描 Device 内部对象 */
    while (p < real_end) {
        if (*p == AML_EXT_OP_PREFIX) {
            /* 扩展 opcode */
            uint8_t ext_op = p[1];
            if (ext_op == AML_DEVICE_OP || ext_op == AML_PROCESSOR_OP ||
                ext_op == AML_POWER_RES_OP || ext_op == AML_THERMAL_ZONE_OP) {
                /* 嵌套设备，递归扫描 */
                aml_scan_device_attrs(ctx, p + 2, real_end);
                /* 跳回当前设备上下文 */
            }
            /* 跳过整个子对象：PkgLength 后继续 */
            /* 这里简化处理，直接 break 退出 */
            break;
        }

        if (*p == AML_SCOPE_OP) {
            /* Scope 对象：跳过 */
            break;
        }

        if (*p == AML_NAME_OP) {
            /* Name 对象 */
            p++;
            char name_seg[8];
            consumed = aml_parse_namestring(p, name_seg, sizeof(name_seg));
            p += consumed;

            /* 检查是否是 _HID, _CID, _CRS */
            if (strncmp(name_seg, "_HID", 4) == 0) {
                /* _HID 值 */
                if (*p == AML_STRING_PREFIX) {
                    p++;
                    strncpy(ctx->cur_hid, (const char *)p, ACPI_HID_SIZE - 1);
                    ctx->cur_hid[ACPI_HID_SIZE - 1] = '\0';
                    while (p < real_end && *p != '\0')
                        p++;
                    p++; /* '\0' */
                } else if (*p == AML_DWORD_PREFIX) {
                    p++;
                    uint32_t val = aml_read32(p);
                    acpi_eisa_id_to_str(val, ctx->cur_hid, ACPI_HID_SIZE);
                    p += 4;
                } else if (*p == AML_WORD_PREFIX) {
                    p++;
                    uint32_t val = aml_read16(p);
                    acpi_eisa_id_to_str(val, ctx->cur_hid, ACPI_HID_SIZE);
                    p += 2;
                } else if (*p == AML_BYTE_PREFIX) {
                    p++;
                    acpi_eisa_id_to_str(*p, ctx->cur_hid, ACPI_HID_SIZE);
                    p++;
                } else {
                    p += aml_skip_data_object(p, real_end);
                }
            } else if (strncmp(name_seg, "_CID", 4) == 0) {
                /* _CID 值 */
                if (*p == AML_STRING_PREFIX) {
                    p++;
                    strncpy(ctx->cur_cid, (const char *)p, ACPI_HID_SIZE - 1);
                    ctx->cur_cid[ACPI_HID_SIZE - 1] = '\0';
                    while (p < real_end && *p != '\0')
                        p++;
                    p++;
                } else if (*p == AML_DWORD_PREFIX) {
                    p++;
                    uint32_t val = aml_read32(p);
                    acpi_eisa_id_to_str(val, ctx->cur_cid, ACPI_HID_SIZE);
                    p += 4;
                } else {
                    p += aml_skip_data_object(p, real_end);
                }
            } else if (strncmp(name_seg, "_CRS", 4) == 0) {
                /* _CRS 值：通常是 Buffer */
                if (*p == AML_BUFFER_OP) {
                    p++;
                    /* PkgLength */
                    lead = *p;
                    uint32_t pkg_len2;
                    if ((lead & 0xC0) == 0) {
                        pkg_len2 = lead;
                        p++;
                    } else {
                        int nb = (lead >> 6) & 0x3;
                        pkg_len2 = lead & 0x0F;
                        p++;
                        for (int i = 0; i < nb; i++) {
                            pkg_len2 |= ((uint32_t)*p) << (4 + i * 8);
                            p++;
                        }
                    }
                    /* BufferSize（通常是 ByteData） */
                    uint32_t buf_size = 0;
                    if (*p == AML_BYTE_PREFIX) {
                        p++;
                        buf_size = *p++;
                    } else if (*p == AML_WORD_PREFIX) {
                        p++;
                        buf_size = aml_read16(p);
                        p += 2;
                    } else if (*p == AML_DWORD_PREFIX) {
                        p++;
                        buf_size = aml_read32(p);
                        p += 4;
                    } else {
                        buf_size = *p++;
                    }
                    /* 解析 CRS 资源 */
                    ctx->cur_num_res = acpi_parse_crs_buffer(p, buf_size,
                                                              ctx->cur_res,
                                                              ACPI_MAX_CRS_RESOURCES);
                    p += buf_size;
                } else {
                    p += aml_skip_data_object(p, real_end);
                }
            } else {
                /* 其他 Name：跳过值 */
                p += aml_skip_data_object(p, real_end);
            }
            continue;
        }

        /* 未知 opcode：尝试跳过 */
        p++;
    }

    ctx->in_device = 0;
}

/*
 * 将当前扫描到的设备保存到 acpi_context
 */
static void aml_save_device(struct aml_scan_ctx *ctx)
{
    if (acpi_context.dsdt_device_count >= ACPI_MAX_DSDT_DEVICES)
        return;

    struct acpi_dsdt_device *adev =
        &acpi_context.dsdt_devices[acpi_context.dsdt_device_count];

    strncpy(adev->name, ctx->cur_dev_name, ACPI_DEV_NAME_SIZE - 1);
    adev->name[ACPI_DEV_NAME_SIZE - 1] = '\0';
    strncpy(adev->hid, ctx->cur_hid, ACPI_HID_SIZE - 1);
    adev->hid[ACPI_HID_SIZE - 1] = '\0';
    strncpy(adev->cid, ctx->cur_cid, ACPI_HID_SIZE - 1);
    adev->cid[ACPI_HID_SIZE - 1] = '\0';

    adev->num_resources = ctx->cur_num_res;
    for (int i = 0; i < ctx->cur_num_res; i++) {
        adev->resource[i].start = ctx->cur_res[i].start;
        adev->resource[i].end = ctx->cur_res[i].end;
        adev->resource[i].flags = ctx->cur_res[i].flags;
    }

    acpi_context.dsdt_device_count++;
}

/* ======================== 顶层 AML 遍历 ======================== */

static void acpi_dsdt_scan_devices(unsigned long dsdt_phys)
{
    struct aml_scan_ctx ctx;
    acpi_table_header *hdr;

    hdr = (acpi_table_header *)_rang_mapping(dsdt_phys, sizeof(acpi_table_header));
    if (!hdr) {
        printk("ACPI: failed to map DSDT header at %x\n", dsdt_phys);
        return;
    }

    /* 映射整个 DSDT 表 */
    uint32_t dsdt_len = hdr->length;
    const uint8_t *aml_start = (const uint8_t *)_rang_mapping(dsdt_phys, dsdt_len);
    if (!aml_start) {
        printk("ACPI: failed to map DSDT at %x (len=%d)\n", dsdt_phys, dsdt_len);
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.base = aml_start + sizeof(acpi_table_header);
    ctx.end = aml_start + dsdt_len;
    ctx.cur = ctx.base;
    ctx.depth = 0;
    ctx.in_device = 0;

    printk("ACPI DSDT: scanning AML (%d bytes)...\n", dsdt_len - (int)sizeof(acpi_table_header));

    /* 遍历 AML 顶层对象 */
    while (ctx.cur < ctx.end) {
        uint8_t op = *ctx.cur;

        if (op == AML_EXT_OP_PREFIX) {
            uint8_t ext_op = ctx.cur[1];
            if (ext_op == AML_DEVICE_OP) {
                /* 顶层 Device */
                aml_scan_device_attrs(&ctx, ctx.cur + 2, ctx.end);
                if (ctx.cur_hid[0] || ctx.cur_cid[0]) {
                    aml_save_device(&ctx);
                    printk("ACPI DSDT: device '%s' HID='%s' CID='%s' res=%d\n",
                           ctx.cur_dev_name, ctx.cur_hid, ctx.cur_cid, ctx.cur_num_res);
                }
                /* 跳过整个 Device 对象 */
                /* 简化处理：直接前进 PkgLength */
                uint8_t lead2 = ctx.cur[2];
                uint32_t plen;
                if ((lead2 & 0xC0) == 0) {
                    plen = lead2;
                    ctx.cur += 3 + plen;
                } else {
                    int nb2 = (lead2 >> 6) & 0x3;
                    plen = lead2 & 0x0F;
                    for (int i = 0; i < nb2; i++)
                        plen |= ((uint32_t)ctx.cur[3 + i]) << (4 + i * 8);
                    ctx.cur += 3 + nb2 + plen - 1;
                }
                continue;
            } else if (ext_op == AML_SCOPE_OP) {
                /* Scope：进入内部继续扫描 */
                uint8_t lead2 = ctx.cur[2];
                uint32_t plen;
                if ((lead2 & 0xC0) == 0) {
                    plen = lead2;
                    ctx.cur += 3;
                } else {
                    int nb2 = (lead2 >> 6) & 0x3;
                    plen = lead2 & 0x0F;
                    for (int i = 0; i < nb2; i++)
                        plen |= ((uint32_t)ctx.cur[3 + i]) << (4 + i * 8);
                    ctx.cur += 3 + nb2;
                }
                /* 递归扫描 Scope 内容 */
                const uint8_t *scope_end = ctx.cur + plen - 1;
                if (scope_end > ctx.end)
                    scope_end = ctx.end;
                /* TODO: 递归调用扫描 Scope 内部 */
                ctx.cur = scope_end;
                continue;
            }
            /* 其他扩展 opcode：跳过 */
            ctx.cur += 2;
            continue;
        }

        if (op == AML_SCOPE_OP) {
            /* Primary Scope */
            uint8_t lead2 = ctx.cur[1];
            uint32_t plen;
            if ((lead2 & 0xC0) == 0) {
                plen = lead2;
                ctx.cur += 2;
            } else {
                int nb2 = (lead2 >> 6) & 0x3;
                plen = lead2 & 0x0F;
                for (int i = 0; i < nb2; i++)
                    plen |= ((uint32_t)ctx.cur[2 + i]) << (4 + i * 8);
                ctx.cur += 2 + nb2;
            }
            /* 跳过 Scope 内容（简化） */
            ctx.cur += plen - 1;
            continue;
        }

        /* 其他顶层 opcode：跳过 */
        ctx.cur++;
    }

    printk("ACPI DSDT: found %d devices\n", acpi_context.dsdt_device_count);
}

/* ======================== Platform 设备注册 ======================== */

/* 静态 Platform 设备数组（由 ACPI 发现） */
static struct platform_resource acpi_plat_res[ACPI_MAX_DSDT_DEVICES][ACPI_MAX_CRS_RESOURCES];
static struct platform_device acpi_plat_devs[ACPI_MAX_DSDT_DEVICES];
static int acpi_plat_dev_count = 0;

void acpi_register_platform_devices(void)
{
    int i, j;

    if (!acpi_context.dsdt_address)
        return;

    acpi_dsdt_scan_devices(acpi_context.dsdt_address);

    for (i = 0; i < (int)acpi_context.dsdt_device_count; i++) {
        struct acpi_dsdt_device *adev = &acpi_context.dsdt_devices[i];

        /* 跳过没有 HID 的设备 */
        if (!adev->hid[0])
            continue;

        struct platform_device *pdev = &acpi_plat_devs[acpi_plat_dev_count];

        /* 设置设备名：优先用 HID，否则用 AML 名 */
        strncpy(pdev->dev.name, adev->hid, DEVICE_NAME_SIZE - 1);
        pdev->dev.name[DEVICE_NAME_SIZE - 1] = '\0';
        pdev->id = -1;

        /* 复制资源 */
        pdev->num_resources = adev->num_resources;
        for (j = 0; j < adev->num_resources; j++) {
            acpi_plat_res[acpi_plat_dev_count][j].start = adev->resource[j].start;
            acpi_plat_res[acpi_plat_dev_count][j].end = adev->resource[j].end;
            acpi_plat_res[acpi_plat_dev_count][j].flags = adev->resource[j].flags;
            pdev->resource[j] = acpi_plat_res[acpi_plat_dev_count][j];
        }

        platform_device_register(pdev);

        printk("ACPI: registered platform device '%s' (HID='%s' CID='%s')\n",
               pdev->dev.name, adev->hid, adev->cid[0] ? adev->cid : "none");
        for (j = 0; j < adev->num_resources; j++) {
            const char *type = "???";
            if (adev->resource[j].flags == AML_RESOURCE_IO)  type = "IO";
            if (adev->resource[j].flags == AML_RESOURCE_MEM) type = "MEM";
            if (adev->resource[j].flags == AML_RESOURCE_IRQ) type = "IRQ";
            printk("  res[%d]: %s %#x - %#x\n", j, type,
                   adev->resource[j].start, adev->resource[j].end);
        }

        acpi_plat_dev_count++;
    }
}
