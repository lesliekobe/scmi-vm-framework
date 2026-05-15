/*
 * scmi_mygearvm_core.c - mygearvm 核心：SCMI 中断入口 & 分发
 *
 * 运行在 ARM Trusted Firmware (ATF) / EL3
 * 接收来自 PVM Linux 的 doorbell 中断，从共享内存读取 SCMI 消息，
 * 分发给对应的 SCMI server (platform/MCP/firmware)
 */

#include <stdint.h>
#include <stdbool.h>

/* ATF 提供的基础头文件 */
#include <common/bl_common.h>
#include <lib/mmio.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <drivers/arm/gicv3.h>

/* 本地定义 */
#include "scmi_defs.h"
#include "scmi_doorbell.h"
#include "scmi_clock.h"
#include "scmi_power.h"
#include "scmi_perf.h"
#include "scmi_i2c.h"
#include "scmi_spi.h"

/* ==================== 寄存器定义 ==================== */

/* 门铃寄存器 (映射到固定地址，由ATF在启动时映射) */
#define DOORBELL_BASE     0xFF5E0000UL  /* 示例，需适配平台 */

/* ==================== SCMI 服务端注册表 ==================== */

typedef int (*scmi_handler_t)(uint32_t proto, uint32_t msg_id,
                               const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t *out_len);

static scmi_handler_t g_scmi_handlers[256];  /* protocol -> handler */
static void *g_scmi_ctx[256];

/* 注册一个 protocol 的 handler */
void scmi_register_handler(uint32_t proto_id, scmi_handler_t h, void *ctx)
{
    if (proto_id < 256) {
        g_scmi_handlers[proto_id] = h;
        g_scmi_ctx[proto_id] = ctx;
    }
}

/* ==================== 共享内存访问 ==================== */

/* SCMI_SHM 在 ATF 中直接映射到已知虚拟地址 */
static volatile uint8_t *g_shm_base = (volatile uint8_t *)SCMI_SHM_BASE;

static inline void *shm_ptr(uint64_t offset)
{
    return (void *)(g_shm_base + offset);
}

/* ==================== Doorbell 中断处理 ==================== */

/*
 * doorbell_irq_handler - mygearvm 收到 PVM 发来的 doorbell
 *
 * ATF 中注册为 FIQ handler
 */
static void doorbell_irq_handler(uint32_t channel_id)
{
    uint32_t ch_offset = channel_id * 0x20;

    /* 读 doorbell 状态 */
    uint32_t db = mmio_read_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_DOORBELL);

    /* 检查 VALID 和 FROM_PVM 位 */
    if ((db & (1U << DOORBELL_BIT_VALID)) && !(db & (1U << DOORBELL_BIT_FROM_PVM))) {
        /* 读取消息地址和大小 */
        uint32_t msg_lo  = mmio_read_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_MESSAGE_LO);
        uint32_t msg_hi  = mmio_read_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_MESSAGE_HI);
        uint32_t msg_sz  = mmio_read_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_MESSAGE_SIZE);

        uint64_t msg_addr = ((uint64_t)msg_hi << 32) | msg_lo;

        /* 从共享内存读取 SCMI 消息 */
        uint8_t msg_buf[SCMI_MAX_PAYLOAD + 4];
        for (uint32_t i = 0; i < msg_sz && i < sizeof(msg_buf); i++) {
            msg_buf[i] = mmio_read_8(msg_addr + i);
        }

        /* 解析消息头 */
        uint32_t *hdr32 = (uint32_t *)msg_buf;
        uint32_t hdr = hdr32[0];
        uint32_t proto_id = (hdr >> 10) & 0xFF;
        uint32_t msg_id   = (hdr >> 0)  & 0x3FF;

        /* 分发给对应 protocol handler */
        uint8_t resp_buf[SCMI_MAX_PAYLOAD + 4];
        size_t resp_len = sizeof(resp_buf);
        int32_t status = SCMI_SUCCESS;

        if (g_scmi_handlers[proto_id]) {
            status = g_scmi_handlers[proto_id](proto_id, msg_id,
                                                msg_buf + 4, msg_sz - 4,
                                                resp_buf + 4, &resp_len);
        } else {
            status = SCMI_NOT_SUPPORTED;
            resp_len = 0;
        }

        /* 组响应: 状态码 + payload */
        hdr32[0] = (uint32_t)status;  /* 响应中放返回码 */
        resp_len += 4;

        /* 将响应写入共享内存 (P2A 区域) */
        uint64_t resp_offset = SCMI_SHM_P2A_OFFSET + (channel_id * SCMI_SHM_CHANNEL_SIZE);
        volatile uint8_t *resp_shm = shm_ptr(resp_offset);
        for (uint32_t i = 0; i < resp_len && i < SCMI_MAX_PAYLOAD; i++) {
            mmio_write_8((uint64_t)resp_shm + i, resp_buf[i]);
        }

        /* 清除 doorbell VALID 位 */
        mmio_write_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_DOORBELL,
                      db & ~(1U << DOORBELL_BIT_VALID));

        /* 触发反向 doorbell -> 通知 PVM */
        mmio_write_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_MESSAGE_SIZE, (uint32_t)resp_len);

        uint32_t resp_db = (1U << DOORBELL_BIT_VALID) | (1U << DOORBELL_BIT_FROM_PVM);
        mmio_write_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_DOORBELL, resp_db);

        /* 内存屏障 */
        dsb();
        sev();
    }
}

/* ==================== EL3 中断入口 ==================== */

/*
 * scmi_el3_fiq_handler - 注册到 ATF 的 FIQ 处理向量
 *
 * 在 BL31 中通过 set_interrupt_handler 配置
 */
void scmi_el3_fiq_handler(uint32_t channel_id)
{
    /* 实际应该通过 GIC 获取 channel，这里简化 */
    (void)channel_id;

    /* 遍历所有 channel 检查挂起的中断 */
    for (uint32_t ch = 0; ch < SCMI_MAX_CHANNELS; ch++) {
        uint32_t ch_offset = ch * 0x20;
        uint32_t irq_stat = mmio_read_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_IRQ_STATUS);

        if (irq_stat & DOORBELL_IRQ_MASK) {
            /* 清中断 */
            mmio_write_32(DOORBELL_BASE + ch_offset + DOORBELL_REG_IRQ_CLEAR, DOORBELL_IRQ_MASK);
            /* 处理 doorbell */
            doorbell_irq_handler(ch);
        }
    }
}

/* ==================== 初始化 ==================== */

void scmi_core_init(void)
{
    /* TODO: 映射 doorbell 寄存器 */
    /* g_doorbell_base = mmio_map(SCMI_DOORBELL_BASE, 0x1000); */

    /* 注册 Base protocol handler (必须) */
    extern int scmi_base_handler(uint32_t proto, uint32_t msg_id,
                                  const uint8_t *in, size_t in_len,
                                  uint8_t *out, size_t *out_len);
    scmi_register_handler(SCMI_BASE_PROTO_ID, scmi_base_handler, NULL);

    /* 注册 Clock protocol handler */
    scmi_register_handler(0x11, scmi_clock_handler, NULL);

    /* 注册 Power protocol handler */
    scmi_register_handler(0x12, scmi_power_handler, NULL);

    /* 注册 Perf protocol handler */
    scmi_register_handler(0x13, scmi_perf_handler, NULL);

    /* 注册 I2C protocol handler */
    scmi_register_handler(0x16, scmi_i2c_handler, NULL);

    /* 注册 SPI protocol handler */
    scmi_register_handler(0x17, scmi_spi_handler, NULL);

    /* 启用 doorbell 中断路由到 EL3 FIQ */
    /* gicv3_enable_sgi(SCMI_DOORBELL_SGI, SECURE); */
}