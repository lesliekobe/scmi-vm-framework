/*
 * scmi_perf_server.c - SCMI Performance Domain Protocol 服务端实现
 *
 * Protocol ID: 0x13
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "scmi_perf.h"
#include "scmi_defs.h"

/* ==================== 模拟硬件状态 ==================== */

#define MAX_PERF_LEVELS  8

typedef struct {
    uint32_t attributes;
    uint32_t min_limit;
    uint32_t max_limit;
    uint32_t current_level;
    uint32_t num_levels;
    /* 性能级别表 */
    struct {
        uint32_t level;
        uint32_t cost;
        uint32_t attr;
    } levels[MAX_PERF_LEVELS];
} perf_domain_info_t;

static perf_domain_info_t g_perf_domains[SCMI_PERF_MAX_DOMAINS] = {
    [0] = { /* CPU domain */
        .attributes = PERF_DOMAIN_ATTR_HAS_LIMIT_NOTIFY
                    | PERF_DOMAIN_ATTR_HAS_LEVEL_NOTIFY
                    | PERF_DOMAIN_ATTR_SUPPORTS_SET
                    | PERF_DOMAIN_ATTR_SUPPORTS_PERF_MGMT,
        .min_limit = 200,
        .max_limit = 4096,
        .current_level = 2048,
        .num_levels = 6,
        .levels = {
            { .level = 200,  .cost = 10, .attr = 0 },
            { .level = 400,  .cost = 20, .attr = 0 },
            { .level = 800,  .cost = 40, .attr = PERF_LEVEL_ATTR_IS_SUSTAINED },
            { .level = 1200, .cost = 70, .attr = 0 },
            { .level = 2048, .cost = 100, .attr = PERF_LEVEL_ATTR_IS_SUSTAINED },
            { .level = 4096, .cost = 200, .attr = PERF_LEVEL_ATTR_PEAK },
        },
    },
    [1] = { /* GPU domain */
        .attributes = PERF_DOMAIN_ATTR_SUPPORTS_SET
                    | PERF_DOMAIN_ATTR_HAS_LEVEL_NOTIFY,
        .min_limit = 100,
        .max_limit = 1000,
        .current_level = 500,
        .num_levels = 4,
        .levels = {
            { .level = 100,  .cost = 30,  .attr = 0 },
            { .level = 300,  .cost = 80,  .attr = 0 },
            { .level = 500,  .cost = 150, .attr = PERF_LEVEL_ATTR_IS_SUSTAINED },
            { .level = 1000, .cost = 300, .attr = PERF_LEVEL_ATTR_PEAK },
        },
    },
};

static uint32_t g_num_perf_domains = 2;

/* ==================== 消息处理 ==================== */

int scmi_perf_handler(uint32_t proto, uint32_t msg_id,
                      const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t *out_len)
{
    (void)proto;

    int32_t *status = (int32_t *)out;

#define REPLY(len) do { *out_len = (len); return *status; } while(0)

    switch (msg_id) {
        case PERF_DOMAIN_ATTRIBUTES: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            perf_domain_info_t *d = &g_perf_domains[domain_id];
            uint32_t *attr = (uint32_t *)(out + 4);

            *status = SCMI_SUCCESS;
            *attr = d->attributes;
            REPLY(8);
        }

        case PERF_DESCRIBE_LEVELS: {
            if (in_len < 8) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;
            uint32_t level_index = *(const uint32_t *)(in + 4);

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            perf_domain_info_t *d = &g_perf_domains[domain_id];

            /* level_index=0 返回总级别数，level_index>0 返回详情 */
            if (level_index == 0) {
                uint32_t *num = (uint32_t *)(out + 4);
                uint32_t *attributes = (uint32_t *)(out + 8);
                *status = SCMI_SUCCESS;
                *num = d->num_levels;
                *attributes = 0; /* no fastchannel support in this simplified version */
                REPLY(12);
            } else {
                /* 返回指定级别的详细信息 (每个entry 12B) */
                uint32_t start_idx = level_index - 1;
                if (start_idx >= d->num_levels) { *status = SCMI_NOT_FOUND; REPLY(4); }

                /* 简化：只返回一个级别，复杂版本可返回范围 */
                uint32_t *level = (uint32_t *)(out + 4);
                uint32_t *cost   = (uint32_t *)(out + 8);
                uint32_t *attr   = (uint32_t *)(out + 12);

                *status = SCMI_SUCCESS;
                *level = d->levels[start_idx].level;
                *cost  = d->levels[start_idx].cost;
                *attr  = d->levels[start_idx].attr;
                REPLY(16);
            }
        }

        case PERF_LIMITS_SET: {
            if (in_len < 16) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            uint32_t domain_id = *(const uint32_t *)in;
            uint32_t flags     = *(const uint32_t *)(in + 4);
            uint32_t min_limit = *(const uint32_t *)(in + 8);
            uint32_t max_limit = *(const uint32_t *)(in + 12);

            (void)flags;

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            perf_domain_info_t *d = &g_perf_domains[domain_id];

            if (min_limit > d->max_limit || max_limit < d->min_limit) {
                *status = SCMI_INVALID_PARAMETERS; REPLY(4);
            }

            if (flags & PERF_LIMIT_FLAG_MIN_SET) d->min_limit = min_limit;
            if (flags & PERF_LIMIT_FLAG_MAX_SET) d->max_limit = max_limit;

            /* clamp current level */
            if (d->current_level < d->min_limit) d->current_level = d->min_limit;
            if (d->current_level > d->max_limit) d->current_level = d->max_limit;

            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        case PERF_LIMITS_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            perf_domain_info_t *d = &g_perf_domains[domain_id];
            uint32_t *min_l = (uint32_t *)(out + 4);
            uint32_t *max_l = (uint32_t *)(out + 8);

            *status = SCMI_SUCCESS;
            *min_l = d->min_limit;
            *max_l = d->max_limit;
            REPLY(12);
        }

        case PERF_LEVEL_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            uint32_t *level = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *level = g_perf_domains[domain_id].current_level;
            REPLY(8);
        }

        case PERF_LEVEL_SET: {
            if (in_len < 8) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;
            uint32_t level     = *(const uint32_t *)(in + 4);

            if (domain_id >= g_num_perf_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            perf_domain_info_t *d = &g_perf_domains[domain_id];

            /* clamp to limits */
            if (level < d->min_limit) level = d->min_limit;
            if (level > d->max_limit) level = d->max_limit;

            d->current_level = level;
            *status = SCMI_SUCCESS;

            uint32_t *actual = (uint32_t *)(out + 4);
            *actual = d->current_level;
            REPLY(8);
        }

        case PERF_NOTIFY_LIMITS:
        case PERF_NOTIFY_LEVEL:
            /* 通知类消息，简化处理 */
            *status = SCMI_SUCCESS;
            REPLY(4);

        default:
            *status = SCMI_NOT_SUPPORTED;
            REPLY(4);
    }
}