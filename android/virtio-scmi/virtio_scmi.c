/*
 * virtio_scmi.c - Android Guest virtio-scmi 驱动
 *
 * 运行在 Android (Linux kernel) 作为 virtio-scmi 设备的 guest 端
 * 通过 virtqueue 与 pvm Linux host 通信
 *
 * 承担两个角色:
 *  1. virtio device driver: 与 host 进行 virtio 通信
 *  2. SCMI transport: 将 scmi_client 的消息通过 virtio 发送
 *
 * 完整驱动参考: Linux kernel drivers/virtio/virtio_scmi.c
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <scmi_client.h>

/* ==================== virtio-scmi 定义 ==================== */

/* virtio ID - 待申请 (virtio_scmi) */
#define VIRTIO_ID_SCMI     42  /* 示例，需官方分配 */

#define VIRTIO_SCMI_VQ_A2P   0  /* Agent→Platform (我们发请求) */
#define VIRTIO_SCMI_VQ_P2A   1  /* Platform→Agent (我们收响应) */
#define VIRTIO_SCMI_VQ_EVENT 2  /* 异步事件 (optional) */

/* ==================== 内部结构 ==================== */

struct scmi_device {
    struct virtio_device *vdev;
    struct virtqueue    *vqs[3];
    spinlock_t           lock;

    /* 等待的请求队列 */
    struct list_head     pending_list;
    wait_queue_head_t    resp_wq;

    /* 特性 */
    uint32_t             features;

    /* 是否已初始化 */
    bool                 ready;
};

static struct scmi_device *g_scmi_dev;

/* ==================== virtqueue 辅助 ==================== */

static inline void *alloc_pages_buf(size_t size)
{
    return kmalloc(size, GFP_ATOMIC);
}

/* ==================== SCMI Transport 实现 ==================== */

/*
 * virtio_scmi_send - SCMI transport 发送函数
 *
 * 实现 scmi_client.h 中定义的 send_fn 类型
 * 被 scmi_client 调用，将消息放入 A2P virtqueue 并等待 P2A 响应
 *
 * 返回: 0=成功 (响应已写入 out), 负数=错误 (如 ETIMEDOUT)
 */
static int virtio_scmi_send(uint32_t proto, uint32_t msg_id, uint32_t token,
                             const uint8_t *in, uint32_t in_len)
{
    struct scmi_device *dev = g_scmi_dev;
    struct virtqueue *vq_a2p = dev->vqs[VIRTIO_SCMI_VQ_A2P];
    struct virtqueue *vq_p2a = dev->vqs[VIRTIO_SCMI_VQ_P2A];

    if (!dev->ready || !vq_a2p || !vq_p2a)
        return -ENODEV;

    /* 构造 16 字节 header + payload SG list */
    uint32_t hdr = (proto << 10) | (msg_id << 0) | (token << 18) | (0U << 26);

    struct scatterlist hdr_sg, payload_sg, resp_sg;
    sg_init_one(&hdr_sg, &hdr, sizeof(hdr));
    sg_init_one(&payload_sg, in, in_len);

    /* 预分配响应 buffer (放在栈上或用 per-xact buffer) */
    uint8_t resp_buf[SCMI_MAX_PAYLOAD];
    sg_init_one(&resp_sg, resp_buf, sizeof(resp_buf));

    struct scatterlist *sgs[3] = { &hdr_sg, &payload_sg, &resp_sg };
    int sgs_count = 2;

    /* 添加到 A2P virtqueue (输出到 host) */
    int n = virtqueue_add_sgs(vq_a2p, sgs, sgs_count, 1, resp_buf, GFP_ATOMIC);
    if (n) {
        dev_err(&dev->vdev->dev, "virtqueue_add_sgs failed: %d\n", n);
        return -ENOMEM;
    }
    virtqueue_kick(vq_a2p);

    /* 等待 P2A virtqueue 返回响应 */
    /* poll with timeout */
    int timeout = 2000;  /* 2s */
    while (timeout > 0) {
        /* 检查 vq 是否有可用的 used descriptor (即响应已到) */
        /* virtqueue_get_buf 返回 NULL 表示还没有 */
        void *resp = virtqueue_get_buf(vq_p2a, &n);
        if (resp) {
            /* 响应到达，调用 scmi_client 的 rx_complete 通知上层 */
            /* 解析返回码: scmi 协议 status 在 resp_buf[0..3] */
            int32_t status = *(int32_t *)resp_buf;
            uint8_t *payload = resp_buf + 4;
            size_t payload_len = n > 4 ? n - 4 : 0;

            /* 调用 scmi_client 的完成回调 (内部处理等待的 xact) */
            scmi_rx_complete(token, status, payload, payload_len);

            return 0;
        }

        msleep(10);
        timeout -= 10;
    }

    dev_warn(&dev->vdev->dev, "scmi xact timeout: proto=%u msg=%u token=%u\n",
             proto, msg_id, token);
    return -ETIMEDOUT;
}

/* ==================== virtqueue 回调 ==================== */

/*
 * virtio_scmi_rx_callback - P2A virtqueue 有响应到达
 *
 * 收到 host 发来的响应，通知 scmi_client 层的 pending xact
 */
static void virtio_scmi_rx_callback(struct virtqueue *vq)
{
    struct scmi_device *dev = g_scmi_dev;

    spin_lock(&dev->lock);

    /* 从 P2A virtqueue 取出所有响应 */
    while (1) {
        unsigned int len;
        void *resp = virtqueue_get_buf(vq, &len);
        if (!resp)
            break;

        /*
         * 解析响应，通知 scmi_client
         *
         * resp 格式: [status(4B)][payload(N)]
         * 根据 token 匹配对应 pending xact，唤醒等待者
         *
         * 这里简化处理：resp_buf 在 virtio_scmi_send 中分配，
         * 实际协议中响应由 scmi_client 的 rx_complete 处理。
         */
        dev_dbg(&dev->vdev->dev, "P2A response: len=%u\n", len);
    }

    spin_unlock(&dev->lock);
    wake_up_interruptible_all(&dev->resp_wq);
}

/*
 * virtio_scmi_tx_callback - A2P virtqueue 发送完成
 *
 * 通知 scmi_client 某个请求已成功发送到 host (不等待响应)
 */
static void virtio_scmi_tx_callback(struct virtqueue *vq)
{
    struct scmi_device *dev = g_scmi_dev;

    spin_lock(&dev->lock);

    /* 回收已发送的 descriptor，以便重用 */
    while (1) {
        unsigned int len;
        void *resp = virtqueue_get_buf(vq, &len);
        if (!resp)
            break;
        /* 已成功发送到 host，等待 P2A 的响应 */
    }

    spin_unlock(&dev->lock);
}

static void virtio_scmi_event_callback(struct virtqueue *vq)
{
    /* 异步事件到达 (SCMI notifications) */
    struct scmi_device *dev = g_scmi_dev;
    unsigned int len;
    void *evt;

    while ((evt = virtqueue_get_buf(vq, &len)) != NULL) {
        dev_info(&dev->vdev->dev, "SCMI event: len=%u\n", len);
        /* TODO: 解析事件类型，通知对应 protocol handler */
    }
}

/* ==================== virtio device 回调 ==================== */

static void virtio_scmi_del_vqs(struct virtio_device *vdev)
{
    vdev->config->del_vqs(vdev);
}

static int virtio_scmi_find_vqs(struct virtio_device *vdev)
{
    static const char *names[] = { "a2p", "p2a", "event" };
    vq_callback_t *callbacks[] = {
        virtio_scmi_tx_callback,
        virtio_scmi_rx_callback,
        virtio_scmi_event_callback,
    };

    return vdev->config->find_vqs(vdev, 3, vdev->vqs, callbacks, names);
}

/* ==================== SCMI transport 实现 ==================== */

static int scmi_virtio_init(struct virtio_device *vdev)
{
    struct scmi_device *dev;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->vdev = vdev;
    spin_lock_init(&dev->lock);
    init_waitqueue_head(&dev->resp_wq);
    INIT_LIST_HEAD(&dev->pending_list);

    /* 探测 virtqueues */
    ret = virtio_scmi_find_vqs(vdev);
    if (ret) {
        dev_err(&vdev->dev, "find_vqs failed: %d\n", ret);
        goto err_free;
    }

    /* 初始化 scmi_client 库，绑定 transport */
    ret = scmi_client_init();
    if (ret) {
        dev_err(&vdev->dev, "scmi_client_init failed: %d\n", ret);
        goto err_del_vqs;
    }

    ret = scmi_client_set_transport(virtio_scmi_send);
    if (ret) {
        dev_err(&vdev->dev, "scmi_client_set_transport failed: %d\n", ret);
        goto err_client_deinit;
    }

    dev->ready = true;
    g_scmi_dev = dev;
    dev_info(&vdev->dev, "virtio-scmi initialized: A2P/P2A/Event vqs ready\n");
    return 0;

err_client_deinit:
    scmi_client_deinit();
err_del_vqs:
    virtio_scmi_del_vqs(vdev);
err_free:
    kfree(dev);
    return ret;
}

static void scmi_virtio_deinit(void)
{
    if (g_scmi_dev) {
        struct virtio_device *vdev = g_scmi_dev->vdev;

        g_scmi_dev->ready = false;
        scmi_client_deinit();

        if (vdev)
            virtio_scmi_del_vqs(vdev);

        kfree(g_scmi_dev);
        g_scmi_dev = NULL;
    }
}

/* ==================== virtio 设备驱动注册 ==================== */

static const struct virtio_device_id virtio_scmi_id_table[] = {
    { VIRTIO_ID_SCMI, VIRTIO_DEV_ANY_ID },
    { 0 },
};
MODULE_DEVICE_TABLE(virtio, virtio_scmi_id_table);

static int virtio_scmi_probe(struct virtio_device *vdev)
{
    return scmi_virtio_init(vdev);
}

static void virtio_scmi_remove(struct virtio_device *vdev)
{
    (void)vdev;
    scmi_virtio_deinit();
}

/* virtio config ops (只实现必须的) */
static const struct virtio_config_ops virtio_scmi_config_ops = {
    .find_vqs = virtio_scmi_find_vqs,
    .del_vqs  = virtio_scmi_del_vqs,
};

static struct virtio_driver virtio_scmi_driver = {
    .driver.name    = "virtio_scmi",
    .driver.owner   = THIS_MODULE,
    .id_table       = virtio_scmi_id_table,
    .probe          = virtio_scmi_probe,
    .remove         = virtio_scmi_remove,
    .config         = &virtio_scmi_config_ops,
};
module_virtio_driver(virtio_scmi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("virtio-scmi guest driver for Android - bridges SCMI client to pvm Linux host");