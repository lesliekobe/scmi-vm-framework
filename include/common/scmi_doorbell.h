/*
 * scmi_doorbell.h - scmi-xlnx 风格 doorbell 定义
 *
 * pvm(Linux) <-> mygearvm 之间的 ARM SCMI 门铃通信机制
 *
 * 原理：
 *   Linux PVM 侧通过写 doorbell register 触发 mygearvm (ATF/TF-A) 中的
 *   SIP (Software Interrupt to EL3) 中断或 FIQ/IRQ，
 *   双方通过共享内存 (SCMI_SHM) 交换 SCMI 命令和响应。
 *
 * 典型实现参考：Xilinx ZynqMP scmi-xlnx 驱动
 */

#ifndef __SCMI_DOORBELL_H
#define __SCMI_DOORBELL_H

#include <stdint.h>

/* ==================== 门铃寄存器偏移 ==================== */

#define DOORBELL_REG_IRQ_STATUS   0x00  /* RO: 中断状态 */
#define DOORBELL_REG_IRQ_CLEAR    0x04  /* WO: 清除中断 */
#define DOORBELL_REG_IRQ_ENABLE   0x08  /* RW: 中断使能 */
#define DOORBELL_REG_DOORBELL     0x0C  /* RW: 门铃触发/状态 */
#define DOORBELL_REG_MESSAGE_HI   0x10  /* RW: 消息地址高32位 */
#define DOORBELL_REG_MESSAGE_LO   0x14  /* RW: 消息地址低32位 */
#define DOORBELL_REG_MESSAGE_SIZE 0x18 /* RW: 消息大小 */

/* Doorbell 标志位 */
#define DOORBELL_BIT_VALID       31    /* 消息有效位 */
#define DOORBELL_BIT_FROM_PVM    30    /* 方向: 0=从pvm来, 1=从mygearvm来 */
#define DOORBELL_IRQ_MASK        (1 << 0)

/* ==================== 共享内存布局 ==================== */

/*
 * SCMI_SHM: Linux PVM 和 mygearvm 共享的内存区域
 * 放置在已知固定地址（或通过ATF获取）
 */
#define SCMI_SHM_BASE             0xFF800000UL  /* 示例地址，需适配 */
#define SCMI_SHM_SIZE            (64 * 1024)   /* 64KB */

/* 共享内存中各通道的偏移 */
#define SCMI_SHM_A2P_OFFSET      0x0          /* Agent→Platform 通道 */
#define SCMI_SHM_P2A_OFFSET      (SCMI_SHM_A2P_OFFSET + SCMI_SHM_CHANNEL_SIZE)
#define SCMI_SHM_CHANNEL_SIZE    (32 * 1024)  /* 每通道32KB */

/* ==================== 通道状态 ==================== */

/* doorbell channel state - 在共享内存中 */
typedef struct {
    volatile uint32_t status;          /* 通道状态 */
    volatile uint32_t flags;
    volatile uint64_t request_addr;    /* 请求缓冲区地址 */
    volatile uint64_t response_addr;   /* 响应缓冲区地址 */
    volatile uint32_t request_size;
    volatile uint32_t response_size;
    volatile uint32_t doorbell;        /* 对端doorbell寄存器镜像 */
} scmi_doorbell_channel_t;

/* 状态值 */
#define CHANNEL_FREE    0
#define CHANNEL_BUSY    1
#define CHANNEL_RING    2

/* ==================== Doorbell 中断源 ==================== */

/* 中断路由：PVM 收到来自 mygearvm 的门铃中断 */
#define PVM_IRQ_SCMI_DOORBELL  96  /* 中断号(示例) */

/* ==================== 辅助宏 ==================== */

#define DOORBELL_SET_ADDR_HI(shm_base, addr_hi) \
    ((uint32_t)((addr_hi) - ((shm_base) & 0xFFFFFFFF00000000ULL)))

#define DOORBELL_SET_ADDR_LO(addr_lo) ((uint32_t)(addr_lo))

/* ==================== API ==================== */

/* Linux PVM 驱动导出给 scmi-core 用的 channel 操作 */
typedef int (*doorbell_send_fn)(uint32_t channel_id, uint64_t msg_addr, uint32_t msg_size);
typedef int (*doorbell_poll_fn)(uint32_t channel_id, uint32_t *msg_size, uint64_t *response_addr);

/* doorbell驱动初始化 */
int doorbell_init(uint64_t shm_phys_base, uint64_t shm_size);
void doorbell_deinit(void);

/* 发送消息给 mygearvm */
int doorbell_send(uint32_t channel_id, uint64_t msg_addr, uint32_t msg_size);

/* 轮询/等待响应 (PVM端) */
int doorbell_poll_response(uint32_t channel_id, uint32_t timeout_ms,
                           uint32_t *out_msg_size, uint64_t *out_resp_addr);

/* 中断处理 (PVM端收到mygearvm的doorbell) */
void doorbell_irq_handler(void);

#endif /* __SCMI_DOORBELL_H */