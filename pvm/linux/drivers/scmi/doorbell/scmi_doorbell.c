/*
 * scmi_doorbell.c - Linux PVM doorbell 驱动
 *
 * 实现 pvm <-> mygearvm 之间的 ARM SCMI 门铃通信
 * 参考: Xilinx ZynqMP scmi-xlnx
 */

#include "scmi_doorbell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>

/* ==================== 内部状态 ==================== */

static uint64_t  g_shm_base;
static uint64_t  g_shm_phys;
static int       g_irq_num = -1;
static int       g_fd = -1;          /* /dev/mem 或 doorbell 设备 fd */
static _Atomic int g_channel_busy[SCMI_MAX_CHANNELS];

/* 门铃寄存器映射 (mmap) */
static volatile uint32_t *g_doorbell_base = NULL;

/* ==================== 门铃寄存器读写 ==================== */

static inline uint32_t doorbell_read32(uint32_t offset)
{
    return g_doorbell_base ? g_doorbell_base[offset / 4] : 0;
}

static inline void doorbell_write32(uint32_t offset, uint32_t val)
{
    if (g_doorbell_base) {
        g_doorbell_base[offset / 4] = val;
    }
}

/* ==================== 初始化 ==================== */

/*
 * doorbell_init - 初始化门铃驱动
 * @shm_phys_base: 共享内存物理地址 (Linux侧映射用)
 * @shm_size: 共享内存大小
 *
 * 注：实际Linux实现中，共享内存由ATF在启动阶段建立，
 * 这里仅做映射。irq由设备树或平台数据传入。
 */
int doorbell_init(uint64_t shm_phys_base, uint64_t shm_size)
{
    (void)shm_size;

    g_shm_phys = shm_phys_base;
    g_shm_base = shm_phys_base;  /* TODO: mmap to virtual addr */

    printf("[doorbell] Init: shm_phys=0x%lx, doorbell_regs at %p\n",
           shm_phys_base, (void*)g_doorbell_base);

    return 0;
}

void doorbell_deinit(void)
{
    if (g_doorbell_base) {
        /* munmap */
        g_doorbell_base = NULL;
    }
    printf("[doorbell] Deinit\n");
}

/* ==================== 发送消息 ==================== */

/*
 * doorbell_send - 发送消息到 mygearvm
 * @channel_id: 通道号
 * @msg_addr: 消息缓冲区地址 (物理地址，共享内存内)
 * @msg_size: 消息大小
 *
 * 流程:
 *  1. 检查 channel 忙闲
 *  2. 写 DOORBELL_REG_MESSAGE_LO/HI/SIZE
 *  3. 写 DOORBELL_REG_DOORBELL 触发门铃
 */
int doorbell_send(uint32_t channel_id, uint64_t msg_addr, uint32_t msg_size)
{
    if (channel_id >= SCMI_MAX_CHANNELS) return -EINVAL;
    if (atomic_load(&g_channel_busy[channel_id])) {
        return -EBUSY;
    }

    /* 获取channel对应的寄存器基址 (每channel占一个独立寄存器组) */
    uint32_t ch_offset = channel_id * 0x20;

    /* 写消息地址和大小 */
    doorbell_write32(ch_offset + DOORBELL_REG_MESSAGE_LO, (uint32_t)msg_addr);
    doorbell_write32(ch_offset + DOORBELL_REG_MESSAGE_HI, (uint32_t)(msg_addr >> 32));
    doorbell_write32(ch_offset + DOORBELL_REG_MESSAGE_SIZE, msg_size);

    /* 写doorbell触发: 写 VALID=1, FROM_PVM=0 */
    uint32_t db_val = (1U << DOORBELL_BIT_VALID) | (0U << DOORBELL_BIT_FROM_PVM);
    doorbell_write32(ch_offset + DOORBELL_REG_DOORBELL, db_val);

    /* 内存屏障 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    atomic_store(&g_channel_busy[channel_id], 1);

    printf("[doorbell] SEND ch=%u addr=0x%lx size=%u\n", channel_id, msg_addr, msg_size);
    return 0;
}

/* ==================== 轮询响应 ==================== */

int doorbell_poll_response(uint32_t channel_id, uint32_t timeout_ms,
                           uint32_t *out_msg_size, uint64_t *out_resp_addr)
{
    if (channel_id >= SCMI_MAX_CHANNELS) return -EINVAL;

    uint32_t ch_offset = channel_id * 0x20;
    uint32_t elapsed = 0;

    /* 轮询 DOORBELL 寄存器 VALID 位 */
    while (elapsed < timeout_ms) {
        uint32_t db = doorbell_read32(ch_offset + DOORBELL_REG_DOORBELL);

        /* 检查 VALID 位为0 -> mygearvm已处理完，响应可读 */
        if (!(db & (1U << DOORBELL_BIT_VALID))) {
            /* 读响应地址 */
            uint32_t resp_lo = doorbell_read32(ch_offset + DOORBELL_REG_MESSAGE_LO);
            uint32_t resp_hi = doorbell_read32(ch_offset + DOORBELL_REG_MESSAGE_HI);
            uint32_t resp_sz = doorbell_read32(ch_offset + DOORBELL_REG_MESSAGE_SIZE);

            if (out_resp_addr) *out_resp_addr = ((uint64_t)resp_hi << 32) | resp_lo;
            if (out_msg_size)  *out_msg_size  = resp_sz;

            atomic_store(&g_channel_busy[channel_id], 0);

            printf("[doorbell] RESP ch=%u addr=0x%lx size=%u\n", channel_id, *out_resp_addr, resp_sz);
            return 0;
        }

        usleep(1000);
        elapsed += 1;
    }

    printf("[doorbell] TIMEOUT ch=%u\n", channel_id);
    return -ETIMEDOUT;
}

/* ==================== 中断处理 ==================== */

/*
 * doorbell_irq_handler - PVM收到mygearvm的门铃中断
 *
 * 在Linux中断处理线程中调用
 */
void doorbell_irq_handler(void)
{
    /* 遍历所有channel检查中断状态 */
    for (uint32_t ch = 0; ch < SCMI_MAX_CHANNELS; ch++) {
        uint32_t ch_offset = ch * 0x20;
        uint32_t irq_stat = doorbell_read32(ch_offset + DOORBELL_REG_IRQ_STATUS);

        if (irq_stat & DOORBELL_IRQ_MASK) {
            /* 清中断 */
            doorbell_write32(ch_offset + DOORBELL_REG_IRQ_CLEAR, DOORBELL_IRQ_MASK);

            /* 通知上层 scmi-core (回调) */
            printf("[doorbell] IRQ on channel %u\n", ch);
            /* TODO: 调用 scmi_core_notify_response(ch) */
        }
    }
}