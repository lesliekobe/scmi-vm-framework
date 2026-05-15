/*
 * scmi_i2c_server.c - SCMI I2C Protocol 服务端实现
 *
 * Protocol ID: 0x16
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "scmi_i2c.h"
#include "scmi_defs.h"

/* ==================== 模拟 I2C 设备 ==================== */

typedef struct {
    uint32_t dev_addr;
    uint32_t max_data_len;
    uint32_t capabilities;
    uint32_t min_speed;
    uint32_t max_speed;
    uint8_t  name[SCMI_I2C_DEV_NAME_LEN];
} i2c_dev_info_t;

static i2c_dev_info_t g_i2c_devices[SCMI_I2C_MAX_DEVICES] = {
    [0] = {
        .dev_addr = 0x50,
        .max_data_len = 128,
        .capabilities = I2C_DEV_ATTR_HAS_BROADCAST | I2C_DEV_ATTR_HAS_10BIT_ADDR | I2C_DEV_ATTR_SUPPORTS_SPEED,
        .min_speed = 100000,
        .max_speed = 400000,
        .name = "EEPROM 24C64",
    },
    [1] = {
        .dev_addr = 0x3C,
        .max_data_len = 32,
        .capabilities = 0,
        .min_speed = 100000,
        .max_speed = 400000,
        .name = "OLED Display",
    },
    [2] = {
        .dev_addr = 0x68,
        .max_data_len = 64,
        .capabilities = I2C_DEV_ATTR_SUPPORTS_SPEED,
        .min_speed = 100000,
        .max_speed = 1000000,
        .name = "MPU6050 Gyro",
    },
};

static uint32_t g_num_i2c_devices = 3;
#define SCMI_I2C_MAX_DEVICES  8

/* 模拟 I2C 传输状态 */
typedef struct {
    bool     busy;
    uint32_t last_status;
    uint32_t last_dev_addr;
} i2c_bus_state_t;

static i2c_bus_state_t g_i2c_bus = { .busy = false };

/* ==================== 模拟 I2C 操作 ==================== */

static int simulate_i2c_transfer(uint32_t dev_addr, uint32_t flags,
                                  const uint8_t *data, uint32_t len,
                                  uint8_t *resp, uint32_t *resp_len)
{
    (void)dev_addr;

    if (g_i2c_bus.busy) return I2C_STATUS_BUS_ERROR;

    g_i2c_bus.busy = true;
    g_i2c_bus.last_dev_addr = dev_addr;

    /* 模拟处理 */
    if (flags & I2C_FLAG_READ) {
        /* 模拟读取：回显 0xAA ... */
        memset(resp, 0xAA, len > 16 ? 16 : len);
        *resp_len = len > 16 ? 16 : len;
    } else {
        /* 写操作: 记录数据 */
        (void)data;
        *resp_len = 0;
    }

    g_i2c_bus.last_status = I2C_STATUS_OK;
    g_i2c_bus.busy = false;
    return I2C_STATUS_OK;
}

/* ==================== 消息处理 ==================== */

int scmi_i2c_handler(uint32_t proto, uint32_t msg_id,
                     const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t *out_len)
{
    (void)proto;

    int32_t *status = (int32_t *)out;

#define REPLY(len) do { *out_len = (len); return *status; } while(0)

    switch (msg_id) {
        case I2C_ATTRIBUTES: {
            uint32_t *num   = (uint32_t *)(out + 4);
            uint32_t *maxlen = (uint32_t *)(out + 8);
            uint32_t *cap  = (uint32_t *)(out + 12);
            *status = SCMI_SUCCESS;
            *num = g_num_i2c_devices;
            *maxlen = I2C_MAX_DATA_LEN;
            *cap = 0; /* no special bus capabilities */
            REPLY(16);
        }

        case I2C_DEVICE_ATTRIBUTES: {
            if (in_len < 4) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }
            uint32_t dev_id = *(const uint32_t *)in;

            if (dev_id >= g_num_i2c_devices) { *status = SCMI_NOT_FOUND; REPLY(4); }

            i2c_dev_info_t *d = &g_i2c_devices[dev_id];
            uint32_t *da     = (uint32_t *)(out + 4);
            uint32_t *maxlen = (uint32_t *)(out + 8);
            uint32_t *cap    = (uint32_t *)(out + 12);
            uint32_t *minspd = (uint32_t *)(out + 16);
            uint32_t *maxspd = (uint32_t *)(out + 20);
            memcpy(out + 24, d->name, SCMI_I2C_DEV_NAME_LEN);

            *status = SCMI_SUCCESS;
            *da = d->dev_addr;
            *maxlen = d->max_data_len;
            *cap = d->capabilities;
            *minspd = d->min_speed;
            *maxspd = d->max_speed;
            REPLY(40);
        }

        case I2C_DEVICE_TRANSFER: {
            if (in_len < 20) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            const i2c_transfer_req_t *req = (const void *)in;

            if (req->dev_id >= g_num_i2c_devices) { *status = SCMI_NOT_FOUND; REPLY(4); }
            if (req->data_len > I2C_MAX_DATA_LEN) { *status = SCMI_INVALID_PARAMETERS; REPLY(4); }

            i2c_transfer_resp_t *resp = (void *)out;
            resp->status = simulate_i2c_transfer(
                req->dev_addr, req->flags,
                req->data, req->data_len,
                resp->data, &resp->data_len);

            *status = SCMI_SUCCESS;
            REPLY(8 + resp->data_len);
        }

        case I2C_DEVICE_CLOSE: {
            /* 简化版无会话，I2C_DEVICE_CLOSE 直接返回成功 */
            *status = SCMI_SUCCESS;
            REPLY(4);
        }

        default:
            *status = SCMI_NOT_SUPPORTED;
            REPLY(4);
    }
}