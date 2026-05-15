/*
 * scmi_spi_server.c - SCMI SPI Protocol 服务端实现
 *
 * Protocol ID: 0x17
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "scmi_spi.h"
#include "scmi_defs.h"

/* ==================== 模拟 SPI 设备 ==================== */

typedef struct {
    uint32_t max_baud;
    uint32_t word_length;     /* 支持的字长掩码 */
    uint32_t attributes;
    uint32_t min_speed;
    uint32_t max_speed;
    uint8_t  name[SCMI_SPI_DEV_NAME_LEN];
} spi_dev_info_t;

static spi_dev_info_t g_spi_devices[SCMI_SPI_MAX_DEVICES] = {
    [0] = {
        .max_baud = 50000000,
        .word_length = 8 | 16,
        .attributes = SPI_DEV_ATTR_HAS_FULL_DUPLEX | SPI_DEV_ATTR_HAS_WRITE | SPI_DEV_ATTR_HAS_READ
                   | SPI_DEV_ATTR_HAS_MODE_POL | SPI_DEV_ATTR_HAS_MODE_PHA
                   | SPI_DEV_ATTR_HAS_CS_POLARITY,
        .min_speed = 1000000,
        .max_speed = 50000000,
        .name = "Flash W25Q128",
    },
    [1] = {
        .max_baud = 10000000,
        .word_length = 8,
        .attributes = SPI_DEV_ATTR_HAS_FULL_DUPLEX | SPI_DEV_ATTR_HAS_WRITE | SPI_DEV_ATTR_HAS_READ,
        .min_speed = 100000,
        .max_speed = 10000000,
        .name = "TFT Display",
    },
};

static uint32_t g_num_spi_devices = 2;
#define SCMI_SPI_MAX_DEVICES  8

/* SPI 会话状态 */
typedef struct {
    bool     active;
    uint32_t dev_id;
    uint32_t speed;
    uint32_t word_len;
    uint32_t mode;
} spi_session_t;

static spi_session_t g_sessions[4] = { { 0 } };  /* 最多4个并发会话 */
#define NUM_SPI_SESSIONS 4

/* ==================== 辅助 ==================== */

static spi_session_t* find_free_session(void)
{
    for (int i = 0; i < NUM_SPI_SESSIONS; i++) {
        if (!g_sessions[i].active) return &g_sessions[i];
    }
    return NULL;
}

static int session_from_handle(uint32_t handle)
{
    if (handle < NUM_SPI_SESSIONS && g_sessions[handle].active) return 0;
    return -1;
}

/* 模拟 SPI 传输 */
static int simulate_spi_transfer(uint32_t handle,
                                  const uint8_t *data, uint32_t len,
                                  uint8_t *out, uint32_t *out_len)
{
    (void)handle;
    /* 模拟: 回显数据，但做简单变换 */
    memcpy(out, data, len);
    /* 模拟读取：回显但每字节取反 */
    for (uint32_t i = 0; i < len; i++) {
        out[i] = ~data[i];
    }
    *out_len = len;
    return SPI_STATUS_OK;
}

/* ==================== 消息处理 ==================== */

int scmi_spi_handler(uint32_t proto, uint32_t msg_id,
                     const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t *out_len)
{
    (void)proto;

    int32_t *status = (int32_t *)out;

#define REPLY(len) do { *out_len = (len); return *status; } while(0)

    switch (msg_id) {
        case SPI_ATTRIBUTES: {
            uint32_t *num  = (uint32_t *)(out + 4);
            uint32_t *maxlen = (uint32_t *)(out + 8);
            uint32_t *cap  = (uint32_t *)(out + 12);
            *status = SCMI_SUCCESS;
            *num = g_num_spi_devices;
            *maxlen = SPI_MAX_DATA_LEN;
            *cap = 0;
            REPLY(16);
        }

        case SPI_DEVICE_ATTRIBUTES: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t dev_id = *(const uint32_t *)in;

            if (dev_id >= g_num_spi_devices) { *status = SCMI_NOT_FOUND; REPLY(4); }

            spi_dev_info_t *d = &g_spi_devices[dev_id];
            uint32_t *mid    = (uint32_t *)(out + 4);
            uint32_t *maxbd  = (uint32_t *)(out + 8);
            uint32_t *wordln = (uint32_t *)(out + 12);
            uint32_t *attr   = (uint32_t *)(out + 16);
            uint32_t *minspd = (uint32_t *)(out + 20);
            uint32_t *maxspd = (uint32_t *)(out + 24);
            memcpy(out + 28, d->name, SCMI_SPI_DEV_NAME_LEN);

            *status = SCMI_SUCCESS;
            *mid = dev_id;
            *maxbd = d->max_baud;
            *wordln = d->word_length;
            *attr = d->attributes;
            *minspd = d->min_speed;
            *maxspd = d->max_speed;
            REPLY(44);
        }

        case SPI_DEVICE_OPEN: {
            if (in_len < 20) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            uint32_t dev_id = *(const uint32_t *)in;

            if (dev_id >= g_num_spi_devices) { *status = SCMI_NOT_FOUND; REPLY(4); }

            spi_session_t *sess = find_free_session();
            if (!sess) { *status = SCMI_NOMEM; REPLY(4); }

            /* 保存会话参数 */
            uint32_t speed  = *(const uint32_t *)(in + 4);
            uint32_t wordln = *(const uint32_t *)(in + 8);
            uint32_t mode   = *(const uint32_t *)(in + 12);
            uint32_t flags  = *(const uint32_t *)(in + 16);

            (void)flags;
            (void)mode;
            (void)wordln;

            sess->active = true;
            sess->dev_id = dev_id;
            sess->speed  = speed;

            /* 返回句柄 */
            uint32_t *handle = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *handle = (uint32_t)(sess - g_sessions);  /* session index as handle */
            REPLY(8);
        }

        case SPI_DEVICE_CLOSE: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t handle = *(const uint32_t *)in;

            if (session_from_handle(handle) < 0) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            g_sessions[handle].active = false;
            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        case SPI_DEVICE_TRANSFER: {
            if (in_len < 20) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            uint32_t handle = *(const uint32_t *)in;
            uint32_t flags  = *(const uint32_t *)(in + 4);

            if (session_from_handle(handle) < 0) { *status = SPI_STATUS_INVALID_HANDLE; REPLY(4); }

            uint32_t data_len = *(const uint32_t *)(in + 12);
            if (data_len > SPI_MAX_DATA_LEN) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            spi_transfer_resp_t *resp = (void *)out;
            resp->status = simulate_spi_transfer(
                handle, in + 16, data_len, resp->data, &resp->data_len);

            *status = SCMI_SUCCESS;
            REPLY(8 + resp->data_len);
        }

        case SPI_DEVICE_GET_STATUS: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t handle = *(const uint32_t *)in;

            if (session_from_handle(handle) < 0) { *status = SPI_STATUS_INVALID_HANDLE; REPLY(4); }

            /* 返回虚拟状态: idle=0, busy=1 */
            uint32_t *sts = (uint32_t *)(out + 4);
            *status = SCMI_SUCCESS;
            *sts = 0; /* idle */
            REPLY(8);
        }

        default:
            *status = SCMI_NOT_SUPPORTED;
            REPLY(4);
    }
}