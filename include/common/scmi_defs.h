/*
 * scmi_defs.h - SCMI 核心定义
 *
 * 来自 ARM SCMI spec DEN0056
 * 所有guest/host共用的基础定义
 */

#ifndef __SCMI_DEFS_H
#define __SCMI_DEFS_H

#include <stdint.h>

/* ==================== 消息ID ==================== */

/* Base Protocol (ID=0x10) */
#define SCMI_BASEProtocolVersion    0x0
#define SCMI_BASEProtocolAttribs    0x1
#define SCMI_BASEProtocolMsgAttribs 0x2
#define SCMI_BASEVendorIdent         0x3
#define SCMI_BASEImplementationVersion 0x4
#define SCMI_BASESearchProtocol     0x5

#define SCMI_BASE_PROTO_ID   0x10

/* ==================== 基础结构 ==================== */

#define SCMI_MAX_CHANNELS   16
#define SCMI_MAX_PENDING    4
#define SCMI_MAX_PAYLOAD    128   /* 控制通道最大负载 */

/* SCMI 消息头 */
typedef struct __attribute__((packed)) {
    uint32_t msg_id   : 10;
    uint32_t protocol : 8;
    uint32_t token    : 8;
    uint32_t type     : 4;    /* 0=command, 1=async response, 2=delayed response, 3=notify */
    uint32_t reserved : 2;
} scmi_header_t;

/* 解析消息头 */
static inline uint32_t scmi_msg_id(const uint32_t *hdr)  { return (hdr[0] >> 0)  & 0x3FF; }
static inline uint32_t scmi_proto_id(const uint32_t *hdr){ return (hdr[0] >> 10) & 0xFF; }
static inline uint32_t scmi_token(const uint32_t *hdr)   { return (hdr[0] >> 18) & 0xFF; }
static inline uint32_t scmi_type(const uint32_t *hdr)    { return (hdr[0] >> 26) & 0xF; }

/* ==================== 返回值 ==================== */

#define SCMI_SUCCESS             0
#define SCMI_NOT_SUPPORTED      -1
#define SCMI_INVALID_PARAMETERS  -2
#define SCMI_DENIED             -3
#define SCMI_NOT_FOUND          -4
#define SCMI_OUT_OF_RANGE       -5
#define SCMI_NOMEM              -6
#define SCMI_NORESPONSE         -7
#define SCMI_TIMEOUT            -8
#define SCMI_BUSY               -9
#define SCMI_COMMS_ERROR        -10
#define SCMI_GENERIC_ERROR      -11

/* ==================== 公共接口 ==================== */

/* 公共错误码转字符串 */
const char* scmi_strerror(int ret);

#endif /* __SCMI_DEFS_H */