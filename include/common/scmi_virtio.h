/*
 * scmi_virtio.h - virtio-scmi 共享定义
 *
 * virtio-scmi 设备规格：
 *   - virtio ID: 待申请 (virtio-scmi)
 *   - Feature bits: VQ pairs (TX/RX), event vq, firmware config
 *   - PCI/Platform device
 */

#ifndef __SCMI_VIRTIO_H
#define __SCMI_VIRTIO_H

#include <stdint.h>

/* virtio-scmi 特性 */
#define VIRTIO_SCMI_F_P2A_CHANNELS   (1 << 0)  /* Platform to Agent channels */
#define VIRTIO_SCMI_F_A2P_CHANNELS   (1 << 1)  /* Agent to Platform channels */
#define VIRTIO_SCMI_F_CMD_WAIT        (1 << 2)  /* 支持 CMD_WAIT (delayed response) */
#define VIRTIO_SCMI_F_SUSPEND        (1 << 3)  /* 支持 suspend/resume */

/* virtqueue ID */
#define VIRTIO_SCMI_VQ_A2P    0   /* Agent→Platform (guest发送请求) */
#define VIRTIO_SCMI_VQ_P2A    1   /* Platform→Agent (guest接收响应) */
#define VIRTIO_SCMI_VQ_EVENT   2   /* 异步事件 (可选) */
#define VIRTIO_SCMI_VQ_CONFIG  3   /* firmware config (可选) */

/* virtio-scmi message types */
#define VIRTIO_SCMI_MSGTYPE_CMD    0  /* 同步命令 */
#define VIRTIO_SCMI_MSGTYPE_CMD_W  1  /* 等待响应 (CMD_WAIT) */
#define VIRTIO_SCMI_MSGTYPE_EVENT  2  /* 异步事件 */
#define VIRTIO_SCMI_MSGTYPE_DELAYED 3 /* 延迟响应 */

/* virtio-scmi shared memory region */
struct virtio_scmi_shm {
    uint32_t fwid;          /* firmware ID */
    uint32_t num_a2p_chans; /* A2P channel count */
    uint32_t num_p2a_chans; /* P2A channel count */
    /* channel descriptors follow */
} __attribute__((packed));

/* 一个 channel 描述符 */
struct virtio_scmi_channel {
    uint64_t shmem_addr;    /* 共享内存物理地址 */
    uint64_t shmem_size;    /* 共享内存大小 */
    uint32_t evt_idx;       /* 事件索引 (用于event vq) */
    uint8_t  role;          /* 0=agent, 1=platform */
    uint8_t  reserved[3];
} __attribute__((packed));

/* virtio config space (legacy) */
struct virtio_scmi_config {
    uint32_t num_a2p_channels;
    uint32_t num_p2a_channels;
    uint32_t shm_base_addr_low;
    uint32_t shm_base_addr_high;
} __attribute__((packed));

#endif /* __SCMI_VIRTIO_H */