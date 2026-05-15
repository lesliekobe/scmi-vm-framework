/*
 * scmi-pvm.c - Linux PVM SCMI 分发器
 *
 * 位于 PVM Linux 内核的 drivers/firmware/arm_scmi/ 下，
 * 负责：
 *  1. 接收来自 Android virtio-scmi 的请求（作为 virtio host）
 *  2. 通过 doorbell 转发给 mygearvm
 *  3. 接收 mygearvm 响应后回传给 Android
 *
 * 简化版骨架，不含完整 SCMI 核心
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include "scmi_doorbell.h"
#include "scmi_defs.h"

/* ==================== 配置 ==================== */

#define MAX_PENDING_REQUESTS   16
#define SCMI_SHM_CH_SIZE       (32 * 1024)

/* ==================== 内部状态 ==================== */

/* SCMI channel 管理 */
struct scmi_pvm_channel {
    uint32_t            id;
    spinlock_t          lock;
    bool                in_use;
    uint64_t            shm_base_phys;

    /* 等待队列 (用于同步等待响应) */
    wait_queue_head_t   wait_q;
    atomic_t            pending;

    /* 请求/响应 FIFO (用于传递消息给 user 线程) */
    struct kfifo        msg_fifo;
};

static struct scmi_pvm_channel g_channels[SCMI_MAX_CHANNELS];
static struct device *g_scmi_dev;

/* ==================== 门铃底层接口 (doorbell.c) ==================== */

extern int doorbell_init(uint64_t shm_phys_base, uint64_t shm_size);
extern int doorbell_send(uint32_t channel_id, uint64_t msg_addr, uint32_t msg_size);
extern int doorbell_poll_response(uint32_t channel_id, uint32_t timeout_ms,
                                  uint32_t *out_msg_size, uint64_t *out_resp_addr);

/* ==================== SCMI 消息处理 ==================== */

/*
 * scmi_dispatch - SCMI 消息分发入口
 *
 * 从任意来源（virtio-scmi 或本地SCP）收到 SCMI 消息后，
 * 根据 protocol_id 路由到对应 handler。
 *
 * 当前简化版：仅处理 doorbell 通道（pvm<->mygearvm）
 */
static int scmi_dispatch(struct scmi_pvm_channel *ch,
                         uint32_t protocol_id,
                         uint32_t msg_id,
                         const void *payload,
                         size_t in_len,
                         void *resp,
                         size_t *resp_len)
{
    int ret;

    /* 1. 组装 SCMI 帧 -> 放入共享内存 */
    uint64_t msg_addr = ch->shm_base_phys;

    /* 简化版：payload 直接拷贝到共享内存 */
    memcpy((void *)msg_addr, payload, in_len);

    /* 2. 触发 doorbell 发往 mygearvm */
    ret = doorbell_send(ch->id, msg_addr, in_len);
    if (ret)
        return ret;

    /* 3. 等待响应 (同步) */
    ret = doorbell_poll_response(ch->id, 5000, (uint32_t *)resp_len, &msg_addr);
    if (ret)
        return ret;

    /* 4. 拷贝响应 */
    memcpy(resp, (const void *)msg_addr, *resp_len);

    return 0;
}

/*
 * scmi_xlate_response - 解析响应头，获取状态
 * 用户层调用
 */
static int32_t scmi_xlate_response(const void *resp_buf, size_t resp_len)
{
    if (resp_len < sizeof(uint32_t))
        return SCMI_NORESPONSE;

    /* SCMI 响应: 第一个字是返回码 (status) */
    return (int32_t)((const uint32_t *)resp_buf)[0];
}

/* ==================== virtio-scmi host 接口 ==================== */

/*
 * 当 Linux 作为 virtio-scmi 的 host 端时，
 * 来自 Android guest 的请求通过 virtqueue 到达这里。
 *
 * 简化版骨架，未包含 virtio bus 集成代码
 */

/* virtio-scmi 收到 Android 发来的消息 */
static int virtio_scmi_rx_callback(void *priv, uint8_t *data, size_t len)
{
    struct scmi_pvm_channel *ch = priv;

    /* 解析 SCMI 头 */
    if (len < 4) return -EINVAL;

    uint32_t hdr = *(uint32_t *)data;
    uint32_t proto_id = (hdr >> 10) & 0xFF;
    uint32_t msg_id   = (hdr >> 0)  & 0x3FF;
    uint32_t token    = (hdr >> 18) & 0xFF;

    /* 分发到 doorbell 通道 */
    uint32_t resp_len;
    unsigned char resp[SCMI_MAX_PAYLOAD];

    int ret = scmi_dispatch(ch, proto_id, msg_id,
                            data + 4, len - 4,
                            resp, &resp_len);

    /* 填充 token -> 响应 */
    if (ret == 0) {
        /* 写回响应到 virtqueue (P2A) - 实际由 virtio 层完成 */
        /* virtio_scmi_notify_peer(ch->id, resp, resp_len); */
    }

    return ret;
}

/* ==================== 初始化/退出 ==================== */

static int __init scmi_pvm_init(void)
{
    int i;

    /* 初始化 doorbell */
    doorbell_init(SCMI_SHM_BASE, SCMI_SHM_SIZE);

    /* 初始化 channels */
    for (i = 0; i < SCMI_MAX_CHANNELS; i++) {
        g_channels[i].id = i;
        spin_lock_init(&g_channels[i].lock);
        init_waitqueue_head(&g_channels[i].wait_q);
        atomic_set(&g_channels[i].pending, 0);
        kfifo_init(&g_channels[i].msg_fifo,
                   kmalloc(sizeof(struct scmi_msg) * MAX_PENDING_REQUESTS, GFP_KERNEL),
                   sizeof(struct scmi_msg) * MAX_PENDING_REQUESTS);

        g_channels[i].shm_base_phys = SCMI_SHM_BASE + (i * SCMI_SHM_CH_SIZE);
    }

    /* TODO: 注册 virtio-scmi host 设备 */
    /* virtio_scmi_register_host(virtio_scmi_rx_callback, g_channels); */

    pr_info("scmi-pvm: initialized %d channels\n", SCMI_MAX_CHANNELS);
    return 0;
}

static void __exit scmi_pvm_exit(void)
{
    /* TODO: unregister */
    pr_info("scmi-pvm: exited\n");
}

module_init(scmi_pvm_init);
module_exit(scmi_pvm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("Linux PVM SCMI dispatcher - doorbell to mygearvm + virtio to Android");