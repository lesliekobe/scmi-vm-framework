/*
 * scmi_power_server.c - SCMI Power Domain Protocol 服务端实现
 *
 * Protocol ID: 0x12
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "scmi_power.h"
#include "scmi_defs.h"

/* ==================== 模拟硬件状态 ==================== */

typedef struct {
    uint32_t attributes;          /* PWR_ATTRIBUTES_* */
    uint8_t  name[SCMI_POWER_NAME_LEN];
    scmi_power_state_t state;
    bool     notifications_enabled;
} power_domain_info_t;

static power_domain_info_t g_power_domains[SCMI_POWER_MAX_DOMAINS] = {
    [0] = {
        .attributes = PWR_ATTRIBUTES_SYNC | PWR_ATTRIBUTES_ASYNC | PWR_ATTRIBUTES_NOTIF,
        .name = "VDD_CORE",
        .state = PWR_STATE_ON,
    },
    [1] = {
        .attributes = PWR_ATTRIBUTES_SYNC | PWR_ATTRIBUTES_NOTIF,
        .name = "VDD_CPU",
        .state = PWR_STATE_ON,
    },
    [2] = {
        .attributes = PWR_ATTRIBUTES_SYNC,
        .name = "VDD_MEM",
        .state = PWR_STATE_ON,
    },
    [3] = {
        .attributes = PWR_ATTRIBUTES_SYNC,
        .name = "VDD_PERI",
        .state = PWR_STATE_ON,
    },
};

static uint32_t g_num_domains = 4;

#define SCMI_POWER_MAX_DOMAINS  8

/* ==================== 消息处理 ==================== */

int scmi_power_handler(uint32_t proto, uint32_t msg_id,
                       const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t *out_len)
{
    (void)proto;
    (void)in_len;

    int32_t *status = (int32_t *)out;

#define REPLY(len) do { *out_len = (len); return *status; } while(0)

    switch (msg_id) {
        case POWER_DOMAIN_ATTRIBUTES: {
            uint32_t *num = (uint32_t *)(out + 4);
            uint32_t *attrs = (uint32_t *)(out + 8);
            *status = SCMI_SUCCESS;
            *num = g_num_domains;
            /* simplified: no per-domain attrs returned here */
            REPLY(8);
        }

        case POWER_DOMAIN_ATTRIBUTES_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;

            if (domain_id >= g_num_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            power_domain_info_t *d = &g_power_domains[domain_id];
            uint32_t *dom_attrs = (uint32_t *)(out + 4);
            memcpy(out + 8, d->name, SCMI_POWER_NAME_LEN);

            *status = SCMI_SUCCESS;
            *dom_attrs = d->attributes;
            REPLY(24);
        }

        case POWER_STATE_SET: {
            if (in_len < 12) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            uint32_t domain_id = *(const uint32_t *)in;
            uint32_t flags     = *(const uint32_t *)(in + 4);
            uint32_t state     = *(const uint32_t *)(in + 8);

            (void)flags;

            if (domain_id >= g_num_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            if (state > PWR_STATE_SLEEP) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            g_power_domains[domain_id].state = (scmi_power_state_t)state;
            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        case POWER_STATE_GET: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;

            if (domain_id >= g_num_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            uint32_t *pstate = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *pstate = g_power_domains[domain_id].state;
            REPLY(8);
        }

        case POWER_STATE_NOTIFY: {
            if (in_len < 8) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t domain_id = *(const uint32_t *)in;
            uint32_t enable    = *(const uint32_t *)(in + 4);

            if (domain_id >= g_num_domains) { *status = SCMI_NOT_FOUND; REPLY(4); }

            g_power_domains[domain_id].notifications_enabled = (enable != 0);
            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        default:
            *status = SCMI_NOT_SUPPORTED;
            REPLY(4);
    }
}