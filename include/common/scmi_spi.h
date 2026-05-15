/*
 * scmi_spi.h - SCMI SPI Protocol 定义
 *
 * Protocol ID: 0x17
 * 用于配置和控制 SPI 总线上的设备
 */

#ifndef __SCMI_SPI_H
#define __SCMI_SPI_H

#include <stdint.h>
#include "scmi_defs.h"

/* ==================== SPI Protocol 版本 ==================== */
#define SCMI_SPI_VERSION    0x10000  /* v1.0 */

/* ==================== SPI Message IDs ==================== */
#define SPI_ATTRIBUTES              0x0
#define SPI_DEVICE_ATTRIBUTES       0x1
#define SPI_DEVICE_OPEN             0x2
#define SPI_DEVICE_CLOSE            0x3
#define SPI_DEVICE_TRANSFER         0x4
#define SPI_DEVICE_GET_STATUS       0x5

/* ==================== 属性 ==================== */

#define SPI_MAX_DEVICES             16
#define SPI_MAX_DATA_LEN            256
#define SPI_MAX_BAUD                100000000  /* 100MHz max */

/* device attributes */
#define SPI_DEV_ATTR_HAS_FULL_DUPLEX  (1U << 0)
#define SPI_DEV_ATTR_HAS_READ        (1U << 1)
#define SPI_DEV_ATTR_HAS_WRITE       (1U << 2)
#define SPI_DEV_ATTR_HAS_MODE_POL     (1U << 3)
#define SPI_DEV_ATTR_HAS_MODE_PHA     (1U << 4)
#define SPI_DEV_ATTR_HAS_CS_POLARITY  (1U << 5)
#define SPI_DEV_ATTR_SUPPORTS_WORD_LEN (1U << 6)

/* transfer flags */
#define SPI_FLAG_READ              (0U << 0)   /* 1=read, 0=write */
#define SPI_FLAG_WRITE             (1U << 0)
#define SPI_FLAG_CS_ASSERT         (0U << 1)   /* 0=keep cs, 1=release after */
#define SPI_FLAG_CS_RELEASE        (1U << 1)
#define SPI_FLAG_MODE_0            (0U << 2)   /* CPOL=0, CPHA=0 */
#define SPI_FLAG_MODE_1            (1U << 2)   /* CPOL=0, CPHA=1 */
#define SPI_FLAG_MODE_2            (2U << 2)   /* CPOL=1, CPHA=0 */
#define SPI_FLAG_MODE_3            (3U << 2)   /* CPOL=1, CPHA=1 */

/* ==================== 数据结构 ==================== */

/* SPI_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t num_devices;
    uint32_t max_data_len;
    uint32_t capabilities;
} spi_attrs_t;

/* SPI_DEVICE_ATTRIBUTES 返回 */
typedef struct __attribute__((packed)) {
    uint32_t dev_id;
    uint32_t max_baud;
    uint32_t word_length;       /* 支持的字长掩码, e.g. (8|16) */
    uint32_t attributes;
    uint32_t min_speed;
    uint32_t max_speed;
    uint8_t  dev_name[SCMI_SPI_DEV_NAME_LEN];
} spi_dev_attr_t;

#define SCMI_SPI_DEV_NAME_LEN  16

/* SPI_DEVICE_OPEN 请求 */
typedef struct __attribute__((packed)) {
    uint32_t dev_id;
    uint32_t speed;             /* Hz */
    uint32_t word_len;          /* 8/16 */
    uint32_t mode;              /* SPI_FLAG_MODE_* */
    uint32_t flags;             /* 其他配置 */
} spi_open_req_t;

/* SPI_DEVICE_TRANSFER 请求 */
typedef struct __attribute__((packed)) {
    uint32_t handle;           /* open返回的句柄 */
    uint32_t flags;
    uint32_t cs;                /* 片选: 0=assert, 其他=release */
    uint32_t data_len;
    uint8_t  data[SPI_MAX_DATA_LEN];
} spi_transfer_req_t;

/* SPI_DEVICE_TRANSFER 响应 */
typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t data_len;
    uint8_t  data[SPI_MAX_DATA_LEN];
} spi_transfer_resp_t;

#define SPI_STATUS_OK              0
#define SPI_STATUS_BUS_ERROR       1
#define SPI_STATUS_TIMEOUT         2
#define SPI_STATUS_INVALID_HANDLE  -2

/* ==================== API ==================== */

int scmi_spi_handler(uint32_t proto, uint32_t msg_id,
                    const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t *out_len);

#endif /* __SCMI_SPI_H */