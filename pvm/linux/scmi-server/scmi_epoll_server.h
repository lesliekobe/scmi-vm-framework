/*
 * scmi_epoll_server.h - SCMI epoll server 头文件
 */

#ifndef __SCMI_EPOLL_SERVER_H
#define __SCMI_EPOLL_SERVER_H

#include <linux/types.h>

/* ==================== 公共 API ==================== */

/* 注册一个 virtio scmi client fd 到 epoll */
int scmi_epoll_register_client(int fd, uint32_t channel_id);

#endif /* __SCMI_EPOLL_SERVER_H */