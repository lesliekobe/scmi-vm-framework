/*
 * scmi_clock.h - SCMI Clock Protocol 定义
 *
 * Protocol ID: 0x11
 * spec: ARM SCMI spec (DEN0056) Section 5
 */

#ifndef __SCMI_CLOCK_H
#define __SCMI_CLOCK_H

#include <stdint.h>
#include "scmi_defs.h"

/* ==================== Clock Protocol 版本 ==================== */
#define SCMI_CLOCK_VERSION    0x20000  /* v2.0 */

/* ==================== Clock Message IDs ==================== */
#define CLOCK_ATTRIBUTES          0x0
#define CLOCK_ATTRIBUTE_GET       0x1
#define CLOCK_RATE_GET            0x2
#define CLOCK_RATE_SET            0x3
#define CLOCK_RATE_NOTIFY         0x4
#define CLOCK_RATE_CHANGE_REQUESTED_NOTIFY  0x5
#define CLOCK_COMPLETED_NOTIFICATIONS_MASK  0x6
#define CLOCK_COMPLETED_NOTIFICATIONS       0x7

/* ==================== Clock 数量上限 ==================== */
#define SCMI_CLOCK_MAX           32
#define SCMI_CLOCK_NAME_LEN      16

/* ==================== 属性 ==================== */

#define CLOCK_HAS_NOTIFICATIONS (1U << 0)  /* 支持异步通知 */
#define CLOCK_SUPPORTS_RATE_CHANGE_REQUEST (1U << 1)

/* ==================== 异步通知事件ID ==================== */
#define CLOCK_EVENT_RATE_CHANGED      0x0
#define CLOCK_EVENT_RATE_CHANGE_REQ   0x1

/* ==================== 数据结构 ==================== */

/* CLOCK_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t clock_id;
    uint32_t clock_attributes;   /* CLOCK_HAS_NOTIFICATIONS 等 */
    uint8_t  clock_name[SCMI_CLOCK_NAME_LEN];
} clock_attr_t;

/* CLOCK_ATTRIBUTE_GET 返回 (指定 clock) */
typedef struct __attribute__((packed)) {
    uint32_t attributes;
    uint32_t range_min;    /* 最低频率，单位 Hz */
    uint32_t range_max;    /* 最高频率 */
    uint8_t  name[SCMI_CLOCK_NAME_LEN];
} clock_dev_attr_t;

/* CLOCK_RATE_GET 返回 */
typedef struct __attribute__((packed)) {
    uint32_t rate_low;
    uint32_t rate_high;    /* 64bit 频率，单位 Hz */
} clock_rate_t;

/* CLOCK_RATE_SET 请求参数 */
typedef struct __attribute__((packed)) {
    uint32_t flags;               /* 0=round-down, 1=round-nearest, 2=round-up */
    uint32_t rate_low;
    uint32_t rate_high;
} clock_rate_set_t;

/* ==================== 辅助 ==================== */

static inline uint64_t clock_rate_get64(const clock_rate_t *r)
{
    return ((uint64_t)r->rate_high << 32) | r->rate_low;
}

/* ==================== API ==================== */

/* 内部: clock protocol handler (由 scmi_dispatch 调用) */
int scmi_clock_handler(uint32_t proto, uint32_t msg_id,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len);

#endif /* __SCMI_CLOCK_H */