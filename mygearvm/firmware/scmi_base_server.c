/*
 * scmi_base_server.c - SCMI Base Protocol 服务端
 *
 * 实现必选的 BASE protocol (protocol_id = 0x10)
 * 支持:
 *   - VERSION
 *   - ATTRIBUTES
 *   - MSG_ATTRIBUTES
 *   - VENDOR_IDENT / IMPLEMENTATION_VERSION
 *   - BASE_SEARCH_PROTOCOL
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ==================== 静态信息 ==================== */

#define SCMI_BASE_VERSION    0x30000  /* v3.0.0 */
#define SCMI_BASE_VERSION_MINOR 0

#define NUM_PROTOCOLS_IMP   2   /* 已实现的协议数量 */
#define BASE_PROTOCOL_ID    0x10

/* 协议列表 (BASE_SEARCH_PROTOCOL 返回) */
static const uint8_t g_implemented_protocols[] = {
    0x10,  /* BASE */
    0x11,  /* CLOCK (示例) */
};

typedef struct {
    uint32_t version;
    uint32_t num_protocols;
    uint32_t impl_ver;
    uint8_t  vendor_id[16];
    uint8_t  impl_id[16];
} scmi_base_attrs_t;

static const scmi_base_attrs_t g_base_attrs = {
    .version       = SCMI_BASE_VERSION,
    .num_protocols = NUM_PROTOCOLS_IMP,
    .impl_ver      = 0x01000000,  /* v1.0.0 */
    .vendor_id     = "MyGearVM",
    .impl_id       = "mygearvm-scmi",
};

/* ==================== 消息属性表 ==================== */

typedef struct {
    uint8_t  msg_id;
    bool     supported;
    uint32_t attributes;  /* 附加属性 */
} msg_attr_entry_t;

/* BASE protocol 消息属性 */
static const msg_attr_entry_t g_base_msg_attrs[] = {
    { 0x0, true,  0 },   /* VERSION */
    { 0x1, true,  0 },   /* ATTRIBUTES */
    { 0x2, true,  0 },   /* MSG_ATTRIBUTES */
    { 0x3, true,  0 },   /* VENDOR_IDENT */
    { 0x4, true,  0 },   /* IMPLEMENTATION_VERSION */
    { 0x5, false, 0 },   /* BASE_SEARCH_PROTOCOL (optional) */
};

static bool msg_supported(uint8_t msg_id)
{
    for (size_t i = 0; i < sizeof(g_base_msg_attrs)/sizeof(g_base_msg_attrs[0]); i++) {
        if (g_base_msg_attrs[i].msg_id == msg_id)
            return g_base_msg_attrs[i].supported;
    }
    return false;
}

/* ==================== 消息处理器 ==================== */

/*
 * 返回值: SCMI_SUCCESS 或错误码
 */
int scmi_base_handler(uint32_t proto, uint32_t msg_id,
                      const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t *out_len)
{
    (void)proto;
    (void)in;
    (void)in_len;

    /* out 格式: [status(4B)][payload...] */
    int32_t *status = (int32_t *)out;

    switch (msg_id) {
        case 0x0: { /* VERSION */
            uint32_t *ver = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *ver = SCMI_BASE_VERSION;
            *out_len = 8;
            break;
        }

        case 0x1: { /* ATTRIBUTES */
            uint32_t *attr = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            attr[0] = g_base_attrs.num_protocols;
            attr[1] = g_base_attrs.impl_ver;
            memcpy(out + 12, g_base_attrs.vendor_id, 16);
            memcpy(out + 28, g_base_attrs.impl_id, 16);
            *out_len = 44;
            break;
        }

        case 0x2: { /* MSG_ATTRIBUTES */
            if (in_len < 1) { *status = SCMI_INVALID_PARAMETERS; break; }
            uint8_t query_msg = in[0];

            uint32_t *attr = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            if (msg_supported(query_msg)) {
                /* 查找属性 */
                for (size_t i = 0; i < sizeof(g_base_msg_attrs)/sizeof(g_base_msg_attrs[0]); i++) {
                    if (g_base_msg_attrs[i].msg_id == query_msg) {
                        *attr = g_base_msg_attrs[i].attributes;
                        break;
                    }
                }
            } else {
                *status = SCMI_NOT_FOUND;
                *out_len = 4;
            }
            *out_len = 8;
            break;
        }

        case 0x3: { /* VENDOR_IDENT */
            *status = SCMI_SUCCESS;
            memcpy(out + 4, g_base_attrs.vendor_id, 16);
            *out_len = 20;
            break;
        }

        case 0x4: { /* IMPLEMENTATION_VERSION */
            uint32_t *ver = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *ver = g_base_attrs.impl_ver;
            *out_len = 8;
            break;
        }

        case 0x5: { /* BASE_SEARCH_PROTOCOL */
            /* 可选: 返回所有已实现的协议列表 */
            uint32_t *num = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *num = g_base_attrs.num_protocols;
            memcpy(out + 8, g_implemented_protocols, g_base_attrs.num_protocols);
            *out_len = 8 + g_base_attrs.num_protocols;
            break;
        }

        default:
            *status = SCMI_NOT_SUPPORTED;
            *out_len = 4;
            break;
    }

    return *status;
}