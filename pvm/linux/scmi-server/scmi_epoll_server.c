/*
 * scmi_epoll_server.c - PVM Linux SCMI 服务端
 *
 * 使用 epoll 监听多个事件源：
 *  1. virtio-scmi 字符设备 fd (Android 的请求)
 *  2. doorbell fd (mygearvm 响应)
 *  3. 内核用户通信 socket (其他内核模块 -> SCMI)
 *
 * 每个源对应一个 worker 线程池处理请求，
 * 请求通过 doorbell 转发给 mygearvm，响应通过原路返回。
 */

#include "scmi_epoll_server.h"
#include "scmi_doorbell.h"
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/eventfd.h>
#include <linux/kfifo.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>

/* ==================== 配置 ==================== */

#define MAX_EPOLL_EVENTS     64
#define MAX_WORKERS          8
#define MAX_PENDING_REQUESTS 256
#define REQ_FIFO_SIZE        (MAX_PENDING_REQUESTS * sizeof(scmi_req_entry_t))

/* ==================== 请求条目 ==================== */

typedef struct {
    uint32_t        ch;
    uint64_t        msg_addr;       /* 共享内存中请求的物理地址 */
    uint32_t        msg_size;
    uint32_t        token;
    uint32_t        proto;
    uint32_t        msg_id;
    void           *priv;            /* 原始 client 指针 (virtio fd / socket) */
    int             fd;              /* 响应用的 fd */
    uint8_t        *resp_shm;        /* 响应存放的共享内存虚拟地址 */
    struct list_head node;
} scmi_req_entry_t;

/* ==================== 全局状态 ==================== */

struct scmi_epoll_server {
    int                 epoll_fd;
    int                 doorbell_fd;   /* doorbell 通知 eventfd */
    struct workqueue_struct *wq;
    struct work_struct   dispatch_work;

    /* client 链表 (通过 virtio char device 或 socket 接入) */
    struct list_head     clients;
    spinlock_t           clients_lock;

    /* 全局请求 FIFO (所有 client 的请求入队) */
    struct kfifo         req_fifo;
    spinlock_t           req_fifo_lock;

    /* 等待 doorbell 响应的 pending 队列 */
    struct list_head     pending_list;
    spinlock_t           pending_lock;
    wait_queue_head_t    resp_wq;

    atomic_t             req_seq;
};

/* ==================== 全局实例 ==================== */

static struct scmi_epoll_server *g_server;

/* ==================== 工具 ==================== */

static inline uint32_t next_req_seq(void)
{
    return atomic_inc_return(&g_server->req_seq);
}

/* ==================== virtio scmi char device 事件处理 ==================== */

/*
 * virtio_scmi_cdev_poll - 轮询 virtio scmi 字符设备
 * 返回: POLLIN 表示有请求可读
 */
static __poll_t virtio_scmi_cdev_poll(struct file *filp,
                                        struct poll_table_struct *pts)
{
    __poll_t mask = 0;
    poll_wait(filp, &g_server->resp_wq, pts);  /* 注册等待队列 */
    /* TODO: 检查 virtqueue 是否有响应待读 */
    return mask;
}

/*
 * handle_virtio_scmi_req - 从 virtio scmi fd 读取 Android 请求
 *
 * virtio scmi 的 A2P 请求通过共享内存传递，
 * 这里 filp 实际对应 /dev/virtio-scmi，read 返回消息描述符。
 * 简化处理: 直接调用 doorbell 发送。
 */
static int handle_virtio_scmi_req(int fd, uint32_t ch)
{
    /*
     * 实际 virtio scmi 中，Android guest 的请求已经在共享内存中。
     * 这里 fd 主要用于事件通知和响应写回。
     *
     * 简化: 从 fd 读出请求元数据 (channel, msg_addr, size)
     * 然后将请求派发给 worker。
     */
    scmi_req_entry_t *req = kmalloc(sizeof(*req), GFP_ATOMIC);
    if (!req)
        return -ENOMEM;

    memset(req, 0, sizeof(*req));
    req->ch = ch;
    req->fd = fd;
    req->priv = NULL;

    /* 消息已在共享内存中 (msg_addr 由 virtio 协议约定) */
    /* 这里简化: 假设共享内存已映射好，直接取地址 */
    extern uint64_t g_shm_base_virt;  /* virtio-scmi 共享内存基址 */
    req->msg_addr = g_shm_base_virt + (ch * SCMI_SHM_CHANNEL_SIZE);
    req->msg_size = SCMI_SHM_CHANNEL_SIZE;
    req->token = next_req_seq();

    /* 解析协议 */
    uint8_t *hdr_bytes = (uint8_t *)phys_to_virt(req->msg_addr);
    uint32_t hdr = *(uint32_t *)hdr_bytes;
    req->proto  = (hdr >> 10) & 0xFF;
    req->msg_id = (hdr >> 0)  & 0x3FF;

    /* 入请求队列 */
    spin_lock(&g_server->req_fifo_lock);
    kfifo_in(&g_server->req_fifo, req, sizeof(req));
    spin_unlock(&g_server->req_fifo_lock);

    /* 触发 worker 处理 */
    queue_work(g_server->wq, &g_server->dispatch_work);

    return 0;
}

/* ==================== Worker 处理 ==================== */

/*
 * scmi_dispatch_work - worker 回调: 从请求队列取请求，发往 mygearvm
 *
 * 流程:
 *  1. 从 req_fifo 取出请求
 *  2. 写 doorbell 触发 mygearvm
 *  3. 挂起在 pending_list 等待响应
 */
static void scmi_dispatch_work(struct work_struct *work)
{
    scmi_req_entry_t *req;

    /* 批量取出请求 (最多处理 64 个) */
    for (int i = 0; i < 64; i++) {
        if (kfifo_is_empty(&g_server->req_fifo))
            break;

        spin_lock(&g_server->req_fifo_lock);
        kfifo_out(&g_server->req_fifo, &req, sizeof(req));
        spin_unlock(&g_server->req_fifo_lock);

        /* 发送 doorbell 给 mygearvm */
        int ret = doorbell_send(req->ch, req->msg_addr, req->msg_size);
        if (ret) {
            pr_err("[scmi-epoll] doorbell_send failed: %d\n", ret);
            kfree(req);
            continue;
        }

        /* 加入 pending 队列，等待响应 */
        spin_lock(&g_server->pending_lock);
        list_add_tail(&req->node, &g_server->pending_list);
        spin_unlock(&g_server->pending_lock);
    }
}

/*
 * scmi_handle_response - 处理 mygearvm 返回的 doorbell 响应
 *
 * 由 doorbell fd 的 epoll 事件触发 (响应到达)
 */
static void scmi_handle_response(uint32_t ch)
{
    uint32_t resp_size;
    uint64_t resp_addr;
    scmi_req_entry_t *entry, *tmp;

    /* 从 doorbell 读取响应 */
    int ret = doorbell_poll_response(ch, 0, &resp_size, &resp_addr);
    if (ret)
        return;  /* 无响应或超时 */

    /* 匹配 pending 队列中的请求 */
    spin_lock(&g_server->pending_lock);
    list_for_each_entry_safe(entry, tmp, &g_server->pending_list, node) {
        if (entry->ch == ch) {
            list_del(&entry->node);
            spin_unlock(&g_server->pending_lock);

            /* 响应已在 resp_addr，通过原始 fd 写回 */
            if (entry->fd >= 0) {
                /* 写响应到 virtio P2A virtqueue 或 socket */
                /* fd_write_resp(entry->fd, resp_addr, resp_size); */
                pr_info("[scmi-epoll] resp ch=%u token=%u size=%u\n",
                        ch, entry->token, resp_size);
            }

            kfree(entry);
            return;
        }
    }
    spin_unlock(&g_server->pending_lock);
}

/* ==================== epoll 事件循环 ==================== */

/*
 * scmi_epoll_loop - 主事件循环
 * 运行在独立的 event/epoll 线程中
 */
static int scmi_epoll_loop(void)
{
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int ret;

    pr_info("[scmi-epoll] event loop started, epoll_fd=%d\n", g_server->epoll_fd);

    while (!kthread_should_stop()) {
        /* 等待事件 */
        ret = epoll_wait(g_server->epoll_fd, events, MAX_EPOLL_EVENTS, 1000);
        if (ret < 0) {
            if (signal_pending(current))
                break;
            continue;
        }

        for (int i = 0; i < ret; i++) {
            struct epoll_event *ev = &events[i];
            uint32_t events_data = ev->events;
            void *priv = ev->data.ptr;

            if (events_data & (EPOLLERR | EPOLLHUP)) {
                pr_warn("[scmi-epoll] fd=%p error\n", priv);
                continue;
            }

            if (events_data & EPOLLIN) {
                /* 判断事件源 */
                if (priv == g_server) {
                    /* doorbell 响应事件 */
                    /* 遍历所有 channel 检查响应 */
                    for (uint32_t ch = 0; ch < SCMI_MAX_CHANNELS; ch++) {
                        scmi_handle_response(ch);
                    }
                } else {
                    /* virtio scmi client fd */
                    /* int fd = *(int *)priv; */
                    /* handle_virtio_scmi_req(fd, channel_id_from_fd(fd)); */
                }
            }
        }
    }

    pr_info("[scmi-epoll] event loop exited\n");
    return 0;
}

/* ==================== 初始化 ==================== */

static int __init scmi_epoll_server_init(void)
{
    struct epoll_event ev;
    int ret;

    pr_info("[scmi-epoll] initializing...\n");

    /* 分配全局 server */
    g_server = kzalloc(sizeof(*g_server), GFP_KERNEL);
    if (!g_server)
        return -ENOMEM;

    atomic_set(&g_server->req_seq, 0);
    INIT_LIST_HEAD(&g_server->clients);
    spin_lock_init(&g_server->clients_lock);
    INIT_LIST_HEAD(&g_server->pending_list);
    spin_lock_init(&g_server->pending_lock);
    init_waitqueue_head(&g_server->resp_wq);
    INIT_WORK(&g_server->dispatch_work, scmi_dispatch_work);

    /* 初始化 kfifo */
    ret = kfifo_alloc(&g_server->req_fifo, REQ_FIFO_SIZE, GFP_KERNEL);
    if (ret) {
        pr_err("[scmi-epoll] kfifo_alloc failed: %d\n", ret);
        goto err_free;
    }

    /* 创建 epoll fd */
    g_server->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_server->epoll_fd < 0) {
        ret = g_server->epoll_fd;
        pr_err("[scmi-epoll] epoll_create1 failed: %d\n", ret);
        goto err_kfifo;
    }

    /* 注册 doorbell fd 到 epoll (用于响应通知) */
    if (g_server->doorbell_fd >= 0) {
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = g_server;
        ret = epoll_ctl(g_server->epoll_fd, EPOLL_CTL_ADD,
                        g_server->doorbell_fd, &ev);
        if (ret) {
            pr_err("[scmi-epoll] epoll_ctl add doorbell failed\n");
            goto err_epoll;
        }
    }

    /* 创建 worker 线程池 */
    g_server->wq = create_workqueue("scmi-dispatch");
    if (!g_server->wq) {
        ret = -ENOMEM;
        goto err_epoll;
    }

    /* 启动 epoll 事件线程 */
    /* kthread_run(scmi_epoll_loop, NULL, "scmi-epoll"); */

    pr_info("[scmi-epoll] initialized: epoll_fd=%d doorbell_fd=%d workers=%d\n",
            g_server->epoll_fd, g_server->doorbell_fd, MAX_WORKERS);
    return 0;

err_epoll:
    close(g_server->epoll_fd);
err_kfifo:
    kfifo_free(&g_server->req_fifo);
err_free:
    kfree(g_server);
    return ret;
}

static void __exit scmi_epoll_server_exit(void)
{
    if (g_server) {
        if (g_server->wq)
            destroy_workqueue(g_server->wq);
        if (g_server->epoll_fd >= 0)
            close(g_server->epoll_fd);
        kfifo_free(&g_server->req_fifo);
        kfree(g_server);
        g_server = NULL;
    }
    pr_info("[scmi-epoll] exited\n");
}

/* ==================== 注册 virtio scmi client fd ==================== */

/*
 * scmi_epoll_register_client - 将 virtio scmi fd 注册到 epoll
 *
 * 被 virtio-scmi char device driver 调用
 */
int scmi_epoll_register_client(int fd, uint32_t channel_id)
{
    struct epoll_event ev;
    int ret;

    if (!g_server)
        return -ENODEV;

    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.u32 = channel_id;  /* 用 channel_id 标识 */

    ret = epoll_ctl(g_server->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (ret)
        pr_err("[scmi-epoll] epoll_ctl_add client fd=%d failed: %d\n", fd, ret);
    else
        pr_info("[scmi-epoll] registered client fd=%d ch=%u\n", fd, channel_id);

    return ret;
}
EXPORT_SYMBOL(scmi_epoll_register_client);

module_init(scmi_epoll_server_init);
module_exit(scmi_epoll_server_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("PVM Linux SCMI epoll server - routes Android virtio-scmi requests to mygearvm via doorbell");