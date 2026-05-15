/*
 * scmi_client.c - Android SCMI Client 库
 *
 * Android 内核空间调用此库访问 SCMI 服务端
 * 底层通过 virtio-scmi 与 pvm Linux 通信
 *
 * 线程安全版本，使用自旋锁保护请求
 */

#include "scmi_client.h"
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/random.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/delay.h>

/* ==================== 配置 ==================== */

#define SCMI_MAX_PAYLOAD  128
#define SCMI_TIMEOUT_MS   5000

/* ==================== 内部状态 ==================== */

typedef struct {
    uint32_t    proto;       /* protocol ID */
    uint32_t    msg_id;      /* message ID */
    uint32_t    token;       /* transaction token */
    uint32_t    in_len;      /* 输入数据长度 */
    uint8_t     in[SCMI_MAX_PAYLOAD];
    uint32_t    out_len;     /* 输出数据长度 */
    uint8_t     out[SCMI_MAX_PAYLOAD];
    bool        done;        /* 响应已到达 */
    int32_t     status;      /* 响应状态 */
    wait_queue_head_t wq;
} pending_xact_t;

/* 全局状态 */
static struct {
    spinlock_t       lock;
    uint32_t         next_token;
    pending_xact_t   pending[SCMI_MAX_PENDING];

    /* virtio transport 回调 */
    int (*send_fn)(uint32_t proto, uint32_t msg_id, uint32_t token,
                   const uint8_t *in, uint32_t in_len);
} g_scmi;

/* ==================== 内部辅助 ==================== */

static inline uint32_t next_token(void)
{
    unsigned long flags;
    spin_lock_irqsave(&g_scmi.lock, flags);
    uint32_t t = g_scmi.next_token++;
    spin_unlock_irqrestore(&g_scmi.lock, flags);
    return t & 0xFF;
}

static pending_xact_t* alloc_xact(void)
{
    unsigned long flags;
    spin_lock_irqsave(&g_scmi.lock, flags);

    for (int i = 0; i < SCMI_MAX_PENDING; i++) {
        if (!g_scmi.pending[i].done && !g_scmi.pending[i].in_len) {
            memset(&g_scmi.pending[i], 0, sizeof(pending_xact_t));
            init_waitqueue_head(&g_scmi.pending[i].wq);
            spin_unlock_irqrestore(&g_scmi.lock, flags);
            return &g_scmi.pending[i];
        }
    }

    spin_unlock_irqrestore(&g_scmi.lock, flags);
    return NULL;
}

static pending_xact_t* find_xact_by_token(uint32_t token)
{
    unsigned long flags;
    spin_lock_irqsave(&g_scmi.lock, flags);
    for (int i = 0; i < SCMI_MAX_PENDING; i++) {
        if (g_scmi.pending[i].token == token && !g_scmi.pending[i].done) {
            spin_unlock_irqrestore(&g_scmi.lock, flags);
            return &g_scmi.pending[i];
        }
    }
    spin_unlock_irqrestore(&g_scmi.lock, flags);
    return NULL;
}

/* ==================== 传输层绑定 ==================== */

/*
 * scmi_client_set_transport - 绑定底层 virtio-scmi 发送函数
 * send_fn: (proto, msg_id, token, in, in_len) -> 0=ok, negative=error
 */
int scmi_client_set_transport(int (*send_fn)(uint32_t, uint32_t, uint32_t,
                                             const uint8_t *, uint32_t))
{
    g_scmi.send_fn = send_fn;
    return 0;
}

/* ==================== 初始化 ==================== */

int scmi_client_init(void)
{
    memset(&g_scmi, 0, sizeof(g_scmi));
    spin_lock_init(&g_scmi.lock);
    pr_info("scmi-client: initialized\n");
    return 0;
}

void scmi_client_deinit(void)
{
    /* 等待所有 pending xact 完成 */
    pr_info("scmi-client: deinitialized\n");
}

/* ==================== 底层发送 ==================== */

/*
 * scmi_do_xact - 发起一次同步 SCMI 事务
 *
 * 流程:
 *  1. 分配 pending slot
 *  2. 填充数据，发送
 *  3. 等待 done (interruptible)
 *  4. 拷贝响应，返回
 */
static int scmi_do_xact(uint32_t proto, uint32_t msg_id,
                        const void *in, uint32_t in_len,
                        void *out, uint32_t *out_len)
{
    if (!g_scmi.send_fn)
        return -ENODEV;

    if (in_len > SCMI_MAX_PAYLOAD || (out && *out_len > SCMI_MAX_PAYLOAD))
        return -EINVAL;

    pending_xact_t *xact = alloc_xact();
    if (!xact)
        return -EBUSY;

    xact->proto   = proto;
    xact->msg_id  = msg_id;
    xact->token   = next_token();
    xact->in_len  = in_len;
    if (in && in_len)
        memcpy(xact->in, in, in_len);

    /* 发送到底层 virtio transport */
    int ret = g_scmi.send_fn(proto, msg_id, xact->token, xact->in, in_len);
    if (ret) {
        xact->in_len = 0;  /* 标记为空闲 */
        return ret;
    }

    /* 等待响应 */
    ret = wait_event_interruptible_timeout(xact->wq, xact->done,
                                           msecs_to_jiffies(SCMI_TIMEOUT_MS));

    unsigned long flags;
    spin_lock_irqsave(&g_scmi.lock, flags);
    xact->done = true;  /* 标记已消费 */
    spin_unlock_irqrestore(&g_scmi.lock, flags);

    if (ret <= 0) {
        /* 超时或被信号打断 */
        pr_warn("scmi-client: xact timeout token=%u proto=%u msg=%u\n",
                xact->token, proto, msg_id);
        xact->in_len = 0;
        return ret < 0 ? ret : -ETIMEDOUT;
    }

    /* 拷贝响应 */
    if (out && out_len && xact->out_len) {
        uint32_t copy_len = min(xact->out_len, *out_len);
        memcpy(out, xact->out, copy_len);
        *out_len = copy_len;
    }

    ret = xact->status;
    xact->in_len = 0;  /* 释放 slot */
    return ret;
}

/* ==================== 响应回调 (由 virtio transport 调用) ==================== */

/*
 * scmi_rx_complete - virtio-scmi 收到响应后调用此函数
 * @token:  匹配 pending xact
 * @status: SCMI status (0=success, negative=error code)
 * @data:   响应 payload
 * @len:    响应长度
 */
void scmi_rx_complete(uint32_t token, int32_t status, const void *data, size_t len)
{
    pending_xact_t *xact = find_xact_by_token(token);
    if (!xact) {
        pr_warn("scmi-client: unexpected token %u\n", token);
        return;
    }

    xact->status = status;
    if (data && len && len <= SCMI_MAX_PAYLOAD) {
        memcpy(xact->out, data, len);
        xact->out_len = len;
    }

    xact->done = true;
    wake_up(&xact->wq);
}

/* ==================== Base Protocol ==================== */

int scmi_base_get_version(uint32_t *version)
{
    uint32_t out_len = sizeof(*version);
    int ret = scmi_do_xact(0x10, 0x0, NULL, 0, version, &out_len);
    return ret;
}

int scmi_base_get_attributes(uint32_t *num_protocols, uint32_t *impl_version)
{
    uint8_t out[12];
    uint32_t out_len = sizeof(out);
    int ret = scmi_do_xact(0x10, 0x1, NULL, 0, out, &out_len);
    if (ret == 0) {
        if (num_protocols) *num_protocols = *(uint32_t *)(out + 0);
        if (impl_version)  *impl_version  = *(uint32_t *)(out + 4);
    }
    return ret;
}

int scmi_base_vendor_ident(char *vendor_id, size_t max_len)
{
    uint8_t out[20];
    uint32_t out_len = sizeof(out);
    int ret = scmi_do_xact(0x10, 0x3, NULL, 0, out, &out_len);
    if (ret == 0 && vendor_id)
        strncpy(vendor_id, (char *)out, max_len - 1);
    return ret;
}

/* ==================== Clock Protocol ==================== */

int scmi_clock_get_num_clocks(uint32_t *num)
{
    uint8_t out[8];
    uint32_t out_len = sizeof(out);
    int ret = scmi_do_xact(0x11, 0x0, NULL, 0, out, &out_len);
    if (ret == 0 && num)
        *num = *(uint32_t *)(out + 0);
    return ret;
}

int scmi_clock_get_attributes(uint32_t clock_id, uint32_t *attributes)
{
    uint8_t in[4], out[36];
    uint32_t out_len = sizeof(out);
    *(uint32_t *)in = clock_id;
    int ret = scmi_do_xact(0x11, 0x1, in, 4, out, &out_len);
    if (ret == 0 && attributes)
        *attributes = *(uint32_t *)(out + 0);
    return ret;
}

int scmi_clock_get_rate(uint32_t clock_id, uint64_t *rate)
{
    uint8_t in[4], out[12];
    uint32_t out_len = sizeof(out);
    *(uint32_t *)in = clock_id;
    int ret = scmi_do_xact(0x11, 0x2, in, 4, out, &out_len);
    if (ret == 0 && rate) {
        uint32_t lo = *(uint32_t *)(out + 0);
        uint32_t hi = *(uint32_t *)(out + 4);
        *rate = ((uint64_t)hi << 32) | lo;
    }
    return ret;
}

int scmi_clock_set_rate(uint32_t clock_id, uint64_t rate, uint64_t *actual_rate)
{
    uint8_t in[16], out[12];
    uint32_t out_len = sizeof(out);
    *(uint32_t *)(in + 0) = clock_id;
    *(uint32_t *)(in + 4) = 0; /* flags: round-nearest */
    *(uint32_t *)(in + 8) = (uint32_t)(rate & 0xFFFFFFFF);
    *(uint32_t *)(in + 12) = (uint32_t)(rate >> 32);
    int ret = scmi_do_xact(0x11, 0x3, in, 16, out, &out_len);
    if (ret == 0 && actual_rate) {
        uint32_t lo = *(uint32_t *)(out + 0);
        uint32_t hi = *(uint32_t *)(out + 4);
        *actual_rate = ((uint64_t)hi << 32) | lo;
    }
    return ret;
}

int scmi_clock_enable_notifications(uint32_t clock_id, bool enable)
{
    uint8_t in[8];
    uint32_t out_len = 4;
    *(uint32_t *)(in + 0) = clock_id;
    *(uint32_t *)(in + 4) = enable ? 1 : 0;
    return scmi_do_xact(0x11, 0x4, in, 8, NULL, &out_len);
}