/*
 * scmi_clock_server.c - SCMI Clock Protocol 服务端实现
 *
 * Protocol ID: 0x11
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "scmi_clock.h"
#include "scmi_defs.h"

/* ==================== 模拟硬件状态 ==================== */

typedef struct {
    uint32_t attributes;           /* CLOCK_HAS_* */
    uint8_t  name[SCMI_CLOCK_NAME_LEN];
    uint64_t current_rate;          /* Hz */
    uint64_t min_rate;
    uint64_t max_rate;
    bool     notifications_enabled;
} clock_info_t;

static clock_info_t g_clocks[SCMI_CLOCK_MAX] = {
    [0] = {
        .attributes = CLOCK_HAS_NOTIFICATIONS | CLOCK_SUPPORTS_RATE_CHANGE_REQUEST,
        .name = "CPU CLK",
        .current_rate = 1200000000ULL,
        .min_rate = 200000000ULL,
        .max_rate = 2400000000ULL,
    },
    [1] = {
        .attributes = 0,
        .name = "UART CLK",
        .current_rate = 24000000ULL,
        .min_rate = 12000000ULL,
        .max_rate = 50000000ULL,
    },
    [2] = {
        .attributes = CLOCK_HAS_NOTIFICATIONS,
        .name = "SPI CLK",
        .current_rate = 100000000ULL,
        .min_rate = 10000000ULL,
        .max_rate = 200000000ULL,
    },
};

static uint32_t g_num_clocks = 3;

/* ==================== 辅助 ==================== */

static uint32_t get_clock_rate(uint32_t clock_id)
{
    if (clock_id >= g_num_clocks) return 0;
    return g_clocks[clock_id].current_rate;
}

/* ==================== 消息处理 ==================== */

int scmi_clock_handler(uint32_t proto, uint32_t msg_id,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len)
{
    (void)proto;
    (void)in_len;

    int32_t *status = (int32_t *)out;

#define REPLY(len) do { *out_len = (len); return *status; } while(0)

    switch (msg_id) {
        case CLOCK_ATTRIBUTES: {
            uint32_t *num = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *num = g_num_clocks;
            REPLY(8);
        }

        case CLOCK_ATTRIBUTE_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t clock_id = *(const uint32_t *)in;

            if (clock_id >= g_num_clocks) { *status = SCMI_NOT_FOUND; REPLY(4); }

            clock_info_t *c = &g_clocks[clock_id];
            uint32_t *attr = (uint32_t *)(out + 4);
            uint64_t *range = (uint64_t *)(out + 8);
            uint32_t *name = (uint32_t *)(out + 16);

            *status = SCMI_SUCCESS;
            *attr = c->attributes;
            *range = c->min_rate | ((uint64_t)c->max_rate << 32);
            /* name: 16 bytes from offset 16 */
            memcpy(out + 16, c->name, SCMI_CLOCK_NAME_LEN);
            REPLY(36);
        }

        case CLOCK_RATE_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t clock_id = *(const uint32_t *)in;

            if (clock_id >= g_num_clocks) { *status = SCMI_NOT_FOUND; REPLY(4); }

            clock_info_t *c = &g_clocks[clock_id];
            uint32_t *rate_lo = (uint32_t *)(out + 4);
            uint32_t *rate_hi = (uint32_t *)(out + 8);

            *status = SCMI_SUCCESS;
            *rate_lo = (uint32_t)(c->current_rate & 0xFFFFFFFF);
            *rate_hi = (uint32_t)(c->current_rate >> 32);
            REPLY(12);
        }

        case CLOCK_RATE_SET: {
            if (in_len < 16) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            uint32_t clock_id = *(const uint32_t *)in;
            uint32_t flags    = *(const uint32_t *)(in + 4);
            uint64_t rate = *(const uint64_t *)(in + 8);

            (void)flags;

            if (clock_id >= g_num_clocks) { *status = SCMI_NOT_FOUND; REPLY(4); }

            clock_info_t *c = &g_clocks[clock_id];

            /* clamp to range */
            if (rate < c->min_rate) rate = c->min_rate;
            if (rate > c->max_rate) rate = c->max_rate;

            c->current_rate = rate;
            *status = SCMI_SUCCESS;

            /* 返回设置后的实际频率 */
            uint32_t *rate_lo = (uint32_t *)(out + 4);
            uint32_t *rate_hi = (uint32_t *)(out + 8);
            *rate_lo = (uint32_t)(c->current_rate & 0xFFFFFFFF);
            *rate_hi = (uint32_t)(c->current_rate >> 32);
            REPLY(12);
        }

        case CLOCK_RATE_NOTIFY: {
            if (in_len < 8) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t clock_id = *(const uint32_t *)in;
            uint32_t enable   = *(const uint32_t *)(in + 4);

            if (clock_id >= g_num_clocks) { *status = SCMI_NOT_FOUND; REPLY(4); }

            g_clocks[clock_id].notifications_enabled = (enable != 0);
            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        default:
            *status = SCMI_NOT_SUPPORTED;
            REPLY(4);
    }
}