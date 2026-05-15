/*
 * virtio_scmi.c - Android Guest virtio-scmi 驱动
 *
 * 运行在 Android (Linux kernel) 作为 virtio-scmi 设备的 guest 端
 * 通过 virtqueue 与 pvm Linux host 通信
 *
 * 框架: 简化版 virtio-scmi guest 驱动骨架
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

/* ==================== virtio-scmi 定义 ==================== */

/* virtio ID - 待申请 (virtio_scmi) */
#define VIRTIO_ID_SCMI     42  /* 示例，需官方分配 */

#define VIRTIO_SCMI_VQ_A2P   0  /* Agent→Platform (我们发请求) */
#define VIRTIO_SCMI_VQ_P2A   1  /* Platform→Agent (我们收响应) */
#define VIRTIO_SCMI_VQ_EVENT 2  /* 异步事件 (optional) */

/* ==================== 内部结构 ==================== */

/* virtqueue 缓冲区描述符 */
struct vq_descriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct scmi_device {
    struct virtio_device *vdev;
    struct virtqueue    *vqs[3];
    spinlock_t           lock;

    /* 等待的请求队列 */
    struct list_head     pending_list;
    wait_queue_head_t    resp_wq;

    /* 特性 */
    uint32_t             features;
};

static struct scmi_device *g_scmi_dev;

/* ==================== 工具 ==================== */

static inline void *alloc_pages_buf(size_t size)
{
    /* 实际应该用: alloc_pages / get_free_pages */
    return kmalloc(size, GFP_KERNEL);
}

/* ==================== 发送请求 ==================== */

/*
 * virtio_scmi_send_message - 发送 SCMI 消息
 *
 * @vq:     使用的 virtqueue (A2P)
 * @proto:  protocol ID
 * @msg_id: message ID
 * @payload: 数据负载
 * @len:    数据长度
 * @resp:   接收响应的 buffer
 * @resp_len: 输入: resp buffer 大小，输出: 实际响应长度
 *
 * 返回: 0=成功, 负数=错误
 */
static int virtio_scmi_send_message(struct virtqueue *vq,
                                     uint32_t proto,
                                     uint32_t msg_id,
                                     uint8_t token,
                                     const void *payload,
                                     size_t len,
                                     void *resp,
                                     size_t *resp_len)
{
    struct virtio_scmi_hdr {
        uint32_t hdr;
    } __attribute__((packed));

    struct scmi_device *dev = g_scmi_dev;
    struct scatterlist sg[2], resp_sg;
    int ret;

    /* 组 header */
    struct virtio_scmi_hdr msg_hdr;
    msg_hdr.hdr = (proto << 10) | (msg_id << 0) | (token << 18) | (0 << 26);
    /* type=0 (command) */

    sg_init_one(&sg[0], &msg_hdr, sizeof(msg_hdr));
    sg_init_one(&sg[1], payload, len);
    sg_init_one(&resp_sg, resp, *resp_len);

    /* 加入 virtqueue */
    /* virtqueue_add_buf(vq, sg, 2, 1, resp, gfp) */
    /* virtqueue_kick(vq); */

    /* 等待响应 (wait_event_timeout) */
    /* ret = wait_event_interruptible_timeout(dev->resp_wq, ...); */

    return 0;
}

/* ==================== virtqueue 回调 ==================== */

/*
 * vq callback - virtqueue 有可用 buffer 时被调用
 */
static void virtio_scmi_rx_callback(struct virtqueue *vq)
{
    /* P2A virtqueue 有响应到达 */
    /* 遍历 used ring，取出响应包，唤醒等待的进程 */
    struct scmi_device *dev = g_scmi_dev;

    spin_lock(&dev->lock);

    /* while (1) {
     *     struct vq_descriptor *desc = virtqueue_get_buf(vq, &len);
     *     if (!desc) break;
     *
     *     // 处理响应，匹配 token，唤醒对应等待者
     *     // 或放入 resp FIFO / list
     * }
     */

    spin_unlock(&dev->lock);

    wake_up_interruptible(&dev->resp_wq);
}

static void virtio_scmi_tx_callback(struct virtqueue *vq)
{
    /* A2P 发送完成回调 */
    (void)vq;
}

static void virtio_scmi_event_callback(struct virtqueue *vq)
{
    /* 异步事件到达 */
    (void)vq;
}

/* ==================== SCMI transport 实现 ==================== */

/*
 * scmi_virtio_init - 初始化 virtio-scmi
 */
static int scmi_virtio_init(struct virtio_device *vdev)
{
    struct scmi_device *dev;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->vdev = vdev;
    spin_lock_init(&dev->lock);
    init_waitqueue_head(&dev->resp_wq);
    INIT_LIST_HEAD(&dev->pending_list);

    /* 探测 virtqueues */
    /* vdev->config->find_vqs(vdev, 3, dev->vqs, callbacks, names); */
    /* virtqueue_set callback(dev->vqs[VIRTIO_SCMI_VQ_A2P], virtio_scmi_tx_callback); */
    /* virtqueue_set callback(dev->vqs[VIRTIO_SCMI_VQ_P2A], virtio_scmi_rx_callback); */
    /* virtqueue_set callback(dev->vqs[VIRTIO_SCMI_VQ_EVENT], virtio_scmi_event_callback); */

    g_scmi_dev = dev;
    return 0;
}

static void scmi_virtio_deinit(void)
{
    if (g_scmi_dev) {
        /* vdev->config->del_vqs(g_scmi_dev->vdev); */
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
    int ret = scmi_virtio_init(vdev);
    if (ret)
        return ret;

    /* 从 virtio config space 读取 channel 信息 */
    /* virtio_scmi_config_init(vdev); */

    return 0;
}

static void virtio_scmi_remove(struct virtio_device *vdev)
{
    (void)vdev;
    scmi_virtio_deinit();
}

static const struct virtio_config_ops virtio_scmi_config_ops = {
    /* .get  = ... */
    /* .set  = ... */
};

static struct virtio_driver virtio_scmi_driver = {
    .driver.name    = KBUILD_MODNAME,
    .driver.owner   = THIS_MODULE,
    .id_table       = virtio_scmi_id_table,
    .probe          = virtio_scmi_probe,
    .remove         = virtio_scmi_remove,
    .config_enabled = virtio_scmi_config_ops,
};
module_virtio_driver(virtio_scmi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("virtio-scmi guest driver for Android");