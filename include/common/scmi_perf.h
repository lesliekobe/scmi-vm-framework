/*
 * scmi_perf.h - SCMI Performance Domain Protocol 定义
 *
 * Protocol ID: 0x13
 * spec: ARM SCMI spec (DEN0056) Section 4
 *
 * Performance domain = CPU 核心或 GPU 等性能调节单元
 * 支持 DVFS (Dynamic Voltage and Frequency Scaling)
 */

#ifndef __SCMI_PERF_H
#define __SCMI_PERF_H

#include <stdint.h>
#include "scmi_defs.h"

/* ==================== Perf Protocol 版本 ==================== */
#define SCMI_PERF_VERSION    0x30000  /* v3.0 */

/* ==================== Perf Message IDs ==================== */
#define PERF_DOMAIN_ATTRIBUTES         0x0
#define PERF_DESCRIBE_LEVELS            0x1
#define PERF_LIMITS_SET                 0x2
#define PERF_LIMITS_GET                 0x3
#define PERF_LEVEL_GET                  0x4
#define PERF_LEVEL_SET                  0x5
#define PERF_NOTIFY_LIMITS              0x6
#define PERF_NOTIFY_LEVEL               0x7
#define PERF_DESCRIBE_FASTCHANNEL       0x8

/* ==================== 属性标志 ==================== */

#define PERF_DOMAIN_ATTR_HAS_LIMIT_NOTIFY  (1U << 0)
#define PERF_DOMAIN_ATTR_HAS_LEVEL_NOTIFY  (1U << 1)
#define PERF_DOMAIN_ATTR_SUPPORTS_SET     (1U << 2)
#define PERF_DOMAIN_ATTR_SUPPORTS_PERF_MGMT (1U << 3)

/* ==================== Level Descriptions ==================== */

typedef struct __attribute__((packed)) {
    uint32_t performance_level;
    uint32_t cost;          /* 相对功耗成本 */
    uint32_t attr;          /* 级别属性 */
} perf_level_entry_t;

#define PERF_LEVEL_ATTR_IS_SUSTAINED   (1U << 0)  /* 持续性能级别 */
#define PERF_LEVEL_ATTR_PEAK           (1U << 1)  /* 峰值性能级别 */

/* DESCRIBE_LEVELS flags */
#define PERF_LEVELS_FLAG_FASTCHANNEL   (1U << 0)  /* 支持快速通道 */
#define PERF_LEVELS_FLAG_COUNT         (1U << 1)  /* 返回数量而非详情 */

/* ==================== Limits ==================== */

/* PERF_LIMITS_SET flags */
#define PERF_LIMIT_FLAG_MIN_SET        (1U << 0)
#define PERF_LIMIT_FLAG_MAX_SET        (1U << 1)

/* PERF_LIMITS_GET/SET 请求 */
typedef struct __attribute__((packed)) {
    uint32_t domain_id;
    uint32_t flags;
    uint32_t min_level;
    uint32_t max_level;
} perf_limits_t;

/* ==================== Level Get/Set ==================== */

typedef struct __attribute__((packed)) {
    uint32_t domain_id;
    uint32_t level;
} perf_level_req_t;

/* ==================== Notify ==================== */

typedef struct __attribute__((packed)) {
    uint32_t domain_id;
    uint32_t notify_enable;
} perf_notify_req_t;

/* ==================== 数据结构 ==================== */

/* PERF_DOMAIN_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t num_domains;
    uint32_t stats_addr_low;
    uint32_t stats_addr_high;
    uint32_t stats_size;
    uint32_t domain_attrs[SCMI_PERF_MAX_DOMAINS];  /* 各自属性 */
} perf_attrs_t;

#define SCMI_PERF_MAX_DOMAINS    16

/* ==================== API ==================== */

int scmi_perf_handler(uint32_t proto, uint32_t msg_id,
                      const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t *out_len);

#endif /* __SCMI_PERF_H */