/*
 * scmi_power.h - SCMI Power Domain Protocol 定义
 *
 * Protocol ID: 0x12
 * spec: ARM SCMI spec (DEN0056) Section 6
 */

#ifndef __SCMI_POWER_H
#define __SCMI_POWER_H

#include <stdint.h>
#include "scmi_defs.h"

/* ==================== Power Protocol 版本 ==================== */
#define SCMI_POWER_VERSION    0x10000  /* v1.0 */

/* ==================== Power Message IDs ==================== */
#define POWER_DOMAIN_ATTRIBUTES        0x0
#define POWER_DOMAIN_ATTRIBUTES_GET    0x1
#define POWER_STATE_SET                0x2
#define POWER_STATE_GET                0x3
#define POWER_STATE_NOTIFY             0x4

/* ==================== Power State ==================== */

typedef enum {
    PWR_STATE_ON      = 0,   /* 运行 */
    PWR_STATE_OFF     = 1,   /* 完全关闭 */
    PWR_STATE_SUSPEND = 2,   /* 挂起 */
    PWR_STATE_SLEEP   = 3,   /* 低功耗睡眠 */
} scmi_power_state_t;

/* State set flags */
#define PWR_STATE_SET_FLAGS_STATE(x)     ((x) & 0xF)
#define PWR_STATE_SET_FLAGS_RESPOND     (1U << 4)  /* 需要平台响应 */
#define PWR_STATE_SET_FLAGS_ASYNC       (1U << 5)  /* 异步响应 */

/* State attributes */
#define PWR_ATTRIBUTES_SYNC     (1U << 0)   /* 支持同步状态设置 */
#define PWR_ATTRIBUTES_ASYNC    (1U << 1)   /* 支持异步状态设置 */
#define PWR_ATTRIBUTES_NOTIF    (1U << 2)   /* 支持通知 */

/* ==================== 异步通知事件ID ==================== */
#define POWER_EVENT_STATE_CHANGED   0x0

/* ==================== 数据结构 ==================== */

/* POWER_DOMAIN_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t num_domains;
    uint32_t attributes;   /* 0=无async支持, 1=有async支持 */
} power_attrs_t;

/* POWER_DOMAIN_ATTRIBUTES_GET 返回 */
typedef struct __attribute__((packed)) {
    uint32_t domain_attributes;
    uint8_t  domain_name[SCMI_POWER_NAME_LEN];
    /* 0xFF if async-only */
} power_domain_attr_t;

#define SCMI_POWER_NAME_LEN  16

/* POWER_STATE_SET 请求 */
typedef struct __attribute__((packed)) {
    uint32_t domain_id;
    uint32_t flags;         /* PWR_STATE_SET_FLAGS_* */
    uint32_t state;         /* PWR_STATE_* */
} power_state_set_t;

/* POWER_STATE_NOTIFY */
typedef struct __attribute__((packed)) {
    uint32_t domain_id;
    uint32_t enable;        /* 1=启用通知 */
} power_notify_t;

/* ==================== API ==================== */

int scmi_power_handler(uint32_t proto, uint32_t msg_id,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len);

#endif /* __SCMI_POWER_H */