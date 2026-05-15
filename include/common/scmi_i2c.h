/*
 * scmi_i2c.h - SCMI I2C Protocol 定义
 *
 * Protocol ID: 0x16
 * 用于配置和控制 I2C 总线上的设备
 */

#ifndef __SCMI_I2C_H
#define __SCMI_I2C_H

#include <stdint.h>
#include "scmi_defs.h"

/* ==================== I2C Protocol 版本 ==================== */
#define SCMI_I2C_VERSION    0x10000  /* v1.0 */

/* ==================== I2C Message IDs ==================== */
#define I2C_ATTRIBUTES              0x0
#define I2C_DEVICE_ATTRIBUTES       0x1
#define I2C_DEVICE_TRANSFER         0x2
#define I2C_DEVICE_CLOSE            0x3

/* ==================== 属性 ==================== */

#define I2C_MAX_PENDING_TRANSACTIONS  8
#define I2C_MAX_DATA_LEN             128  /* 单次传输最大字节数 */

/* device attributes */
#define I2C_DEV_ATTR_HAS_BROADCAST   (1U << 0)
#define I2C_DEV_ATTR_HAS_10BIT_ADDR  (1U << 1)
#define I2C_DEV_ATTR_SUPPORTS_SPEED  (1U << 2)

/* transfer flags */
#define I2C_FLAG_READ              (0U << 0)
#define I2C_FLAG_WRITE            (1U << 0)
#define I2C_FLAG_BROADCAST        (1U << 1)
#define I2C_FLAG_10BIT_ADDR       (1U << 2)
#define I2C_FLAG_STOP             (1U << 3)
#define I2C_FLAG_NO_START         (1U << 4)
#define I2C_FLAG_REPEAT_START     (1U << 5)

/* ==================== 数据结构 ==================== */

/* I2C_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t num_devices;
    uint32_t max_data_len;
    uint32_t capabilities;
} i2c_attrs_t;

/* I2C_DEVICE_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t dev_addr;
    uint32_t max_data_len;
    uint32_t capabilities;
    uint32_t min_speed;       /* Hz */
    uint32_t max_speed;       /* Hz */
    uint8_t  dev_name[SCMI_I2C_DEV_NAME_LEN];
} i2c_dev_attr_t;

#define SCMI_I2C_DEV_NAME_LEN  16

/* I2C_DEVICE_TRANSFER 请求 */
typedef struct __attribute__((packed)) {
    uint32_t dev_id;
    uint32_t dev_addr;           /* 实际I2C地址(7bit或10bit) */
    uint32_t speed;              /* Hz */
    uint32_t flags;              /* I2C_FLAG_* */
    uint32_t data_len;
    uint8_t  data[I2C_MAX_DATA_LEN];
} i2c_transfer_req_t;

/* I2C_DEVICE_TRANSFER 响应 */
typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t data_len;
    uint8_t  data[I2C_MAX_DATA_LEN];
} i2c_transfer_resp_t;

#define I2C_STATUS_OK          0
#define I2C_STATUS_BUS_ERROR   1
#define I2C_STATUS_NACK        2
#define I2C_STATUS_TIMEOUT     3
#define I2C_STATUS_INVALID_PARAM  -2

/* ==================== API ==================== */

int scmi_i2c_handler(uint32_t proto, uint32_t msg_id,
                     const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t *out_len);

#endif /* __SCMI_I2C_H */