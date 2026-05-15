/*
 * scmi_spi_adapter.c - SCMI SPI 总线适配器 (Android 驱动)
 *
 * 模拟一个 SPI 总线适配器，底层通过 SCMI Clock protocol 开关/设置时钟，
 * 通过 SCMI SPI protocol 进行数据传输
 *
 * 使用 platform_driver 框架，对接 Linux SPI subsystem (spidev)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "../virtio-scmi/scmi_client/scmi_client.h"

/* ==================== SCMI SPI 会话管理 ==================== */

#define SCMI_SPI_MAX_HANDLES  8

typedef struct {
    bool        active;
    uint32_t    handle;    /* SCMI 返回的 session handle */
    uint32_t    dev_id;
    uint32_t    speed;
    uint32_t    word_len;
    uint32_t    mode;
} scmi_spi_session_t;

static scmi_spi_session_t g_spi_sessions[SCMI_SPI_MAX_HANDLES];
static spinlock_t g_spi_sessions_lock;

/* ==================== SPI 适配器私有数据 ==================== */

struct scmi_spi_adapter {
    struct spi_master *master;   /* SPI master 注册到 kernel */
    uint32_t           bus_num;

    /* SPI 总线使用的时钟 (来自 SCMI Clock protocol) */
    uint32_t           clock_id;
    bool               clock_enabled;

    /* BCM (bus control machine) 状态 */
    uint32_t           current_cs;   /* 当前片选 */
};

static struct scmi_spi_adapter *g_spi_adapters[SCMI_SPI_MAX_BUSES];
static int g_num_spi_adapters;

/* ==================== 时钟管理 ==================== */

static int scmi_spi_enable_clock(struct scmi_spi_adapter *adap, uint32_t speed_hz)
{
    int ret;
    uint64_t actual_rate;

    if (adap->clock_enabled && speed_hz == 0)
        return 0;  /* 已经使能且不要求调速 */

    /* 调速到目标频率 */
    if (speed_hz == 0)
        speed_hz = 10000000;  /* 默认 10MHz */

    ret = scmi_clock_set_rate(adap->clock_id, speed_hz, &actual_rate);
    if (ret != 0) {
        dev_err(&adap->master->dev, "scmi_clock_set_rate failed: %d\n", ret);
        return ret;
    }

    /* 如果时钟还没开，开启通知 */
    if (!adap->clock_enabled) {
        scmi_clock_enable_notifications(adap->clock_id, false);
        adap->clock_enabled = true;
    }

    dev_dbg(&adap->master->dev, "clock %u rate set: request=%u actual=%llu\n",
            adap->clock_id, speed_hz, actual_rate);
    return 0;
}

static int scmi_spi_disable_clock(struct scmi_spi_adapter *adap)
{
    /* SCMI 没有显式关闭时钟接口，标记状态即可 */
    adap->clock_enabled = false;
    return 0;
}

/* ==================== SCMI SPI 会话 ==================== */

/* 内部: 分配一个 SPI session */
static scmi_spi_session_t* alloc_spi_session(void)
{
    unsigned long flags;
    spin_lock_irqsave(&g_spi_sessions_lock, flags);

    for (int i = 0; i < SCMI_SPI_MAX_HANDLES; i++) {
        if (!g_spi_sessions[i].active) {
            g_spi_sessions[i].active = true;
            g_spi_sessions[i].handle = i;
            spin_unlock_irqrestore(&g_spi_sessions_lock, flags);
            return &g_spi_sessions[i];
        }
    }

    spin_unlock_irqrestore(&g_spi_sessions_lock, flags);
    return NULL;
}

static scmi_spi_session_t* find_spi_session(uint32_t handle)
{
    if (handle >= SCMI_SPI_MAX_HANDLES || !g_spi_sessions[handle].active)
        return NULL;
    return &g_spi_sessions[handle];
}

/* SCMI SPI Protocol ID */
#define SCMI_SPI_PROTO  0x17

/*
 * scmi_spi_open - 通过 SCMI 打开 SPI 设备会话
 * 返回 session handle (>=0=成功, <0=错误码)
 */
static int scmi_spi_open(struct scmi_spi_adapter *adap,
                          uint32_t dev_id, uint32_t speed,
                          uint32_t word_len, uint32_t mode)
{
    scmi_spi_session_t *sess = alloc_spi_session();
    if (!sess)
        return -EBUSY;

    /*
     * 实际应该通过 scmi_do_xact(SCMI_SPI_PROTO, 0x2, open_req, ...) 获取真实 handle
     * 这里简化处理，用 index 作为 handle
     */

    sess->dev_id   = dev_id;
    sess->speed    = speed;
    sess->word_len = word_len;
    sess->mode     = mode;

    dev_info(&adap->master->dev, "scmi_spi_open: dev=%u handle=%u speed=%u\n",
             dev_id, sess->handle, speed);

    return (int)sess->handle;
}

/*
 * scmi_spi_close - 关闭 SPI 会话
 */
static int scmi_spi_close(struct scmi_spi_adapter *adap, uint32_t handle)
{
    scmi_spi_session_t *sess = find_spi_session(handle);
    if (!sess)
        return -EINVAL;

    sess->active = false;
    dev_info(&adap->master->dev, "scmi_spi_close: handle=%u\n", handle);
    return 0;
}

/*
 * scmi_spi_transfer - 通过 SCMI 发起 SPI 传输
 * @handle: session handle
 * @tx_buf, @rx_buf: 发送/接收缓冲区 (可为 NULL)
 * @len: 传输字节数
 */
static int scmi_spi_transfer(struct scmi_spi_adapter *adap,
                             uint32_t handle,
                             const void *tx_buf, void *rx_buf,
                             size_t len)
{
    scmi_spi_session_t *sess = find_spi_session(handle);
    if (!sess)
        return -EINVAL;

    /*
     * 实际: scmi_do_xact(SCMI_SPI_PROTO, 0x4, transfer_req, ...)
     *
     * 简化: 模拟传输
     */
    if (tx_buf && rx_buf && tx_buf != rx_buf) {
        /* 全双工: 发送的同时接收 */
        const uint8_t *tx = tx_buf;
        uint8_t *rx = rx_buf;
        for (size_t i = 0; i < len; i++) {
            rx[i] = ~tx[i];  /* 模拟: 每字节取反作为回读数据 */
        }
    } else if (tx_buf) {
        /* 仅发送 */
        dev_dbg(&adap->master->dev, "spi xfer send: %zu bytes\n", len);
    } else if (rx_buf) {
        /* 仅接收: 填充模拟数据 */
        memset(rx_buf, 0xFF, len);
        dev_dbg(&adap->master->dev, "spi xfer recv: %zu bytes\n", len);
    }

    return 0;
}

/* ==================== Linux SPI Master ops ==================== */

static int scmi_spi_setup(struct spi_device *spi)
{
    struct scmi_spi_adapter *adap = spi_master_get_devdata(spi->master);

    dev_dbg(&spi->dev, "scmi_spi_setup: cs=%u mode=%u bits=%u speed=%u\n",
            spi->chip_select, spi->mode, spi->bits_per_word, spi->max_speed_hz);

    /* 如果目标速度不同于当前，使能时钟 */
    return scmi_spi_enable_clock(adap, spi->max_speed_hz);
}

static int scmi_spi_transfer_one(struct spi_master *master,
                                  struct spi_device *spi,
                                  struct spi_transfer *tfr)
{
    struct scmi_spi_adapter *adap = spi_master_get_devdata(master);
    scmi_spi_session_t *sess;
    uint32_t handle;
    int ret;

    /* 确保时钟已使能 */
    ret = scmi_spi_enable_clock(adap, tfr->speed_hz ? tfr->speed_hz : spi->max_speed_hz);
    if (ret)
        return ret;

    /* 打开/复用 session (每个 spi_device 一个 session) */
    /* 简化: 用 device id = spi->chip_select 作为 dev_id，每次传输前确保 session 打开 */
    /* 更好的做法是在 setup 时打开，remove 时关闭 */
    handle = (uint32_t)spi->chip_select;  /* 简化: 用 cs 作为 handle */

    sess = find_spi_session(handle);
    if (!sess || !sess->active) {
        /* 还没打开，先打开 */
        ret = scmi_spi_open(adap, spi->chip_select,
                            tfr->speed_hz ? tfr->speed_hz : spi->max_speed_hz,
                            spi->bits_per_word, spi->mode);
        if (ret < 0)
            return ret;
        handle = (uint32_t)ret;
    }

    /* 发起传输 */
    ret = scmi_spi_transfer(adap, handle,
                            tfr->tx_buf, tfr->rx_buf, tfr->len);
    if (ret) {
        dev_err(&spi->dev, "scmi_spi_transfer failed: %d\n", ret);
        return ret;
    }

    /* 片的片选控制: 实际由 scmi_spi_transfer 中的 flags 处理 */

    return 0;
}

static void scmi_spi_cleanup(struct spi_device *spi)
{
    struct scmi_spi_adapter *adap = spi_master_get_devdata(spi->master);
    scmi_spi_session_t *sess;

    /* 关闭 session */
    uint32_t handle = spi->chip_select;
    sess = find_spi_session(handle);
    if (sess && sess->active) {
        scmi_spi_close(adap, handle);
    }

    /* 如果没有其他活跃 session，关闭时钟 */
    bool any_active = false;
    for (int i = 0; i < SCMI_SPI_MAX_HANDLES; i++) {
        if (g_spi_sessions[i].active) {
            any_active = true;
            break;
        }
    }
    if (!any_active)
        scmi_spi_disable_clock(adap);
}

/* ==================== 设备树支持 ==================== */

#ifdef CONFIG_OF
static int scmi_spi_probe_dt(struct platform_device *pdev,
                              struct scmi_spi_adapter *adap)
{
    struct device_node *np = pdev->dev.of_node;
    uint32_t val;
    int ret;

    if (!np) return -ENODEV;

    ret = of_property_read_u32(np, "scmi,spi-bus-id", &val);
    if (ret == 0)
        adap->bus_num = val;

    ret = of_property_read_u32(np, "scmi,clock-id", &val);
    if (ret == 0)
        adap->clock_id = val;
    else {
        dev_err(&pdev->dev, "missing scmi,clock-id\n");
        return -EINVAL;
    }

    dev_info(&pdev->dev, "DT: bus_num=%u clock_id=%u\n", adap->bus_num, adap->clock_id);
    return 0;
}
#else
static int scmi_spi_probe_dt(struct platform_device *pdev,
                              struct scmi_spi_adapter *adap) { return 0; }
#endif

/* ==================== Platform Driver ==================== */

static int scmi_spi_probe(struct platform_device *pdev)
{
    struct scmi_spi_adapter *adap;
    struct spi_master *master;
    int ret;

    master = spi_alloc_master(&pdev->dev, sizeof(*adap));
    if (!master)
        return -ENOMEM;

    adap = spi_master_get_devdata(master);
    adap->master = master;

    ret = scmi_spi_probe_dt(pdev, adap);
    if (ret)
        goto err_free;

    spin_lock_init(&g_spi_sessions_lock);
    master->bus_num = adap->bus_num;
    master->num_chipselect = 8;
    master->mode_bits = SPI_CPHA | SPI_CPOL | SPI_CS_HIGH | SPI_LSB_FIRST;
    master->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
    master->setup = scmi_spi_setup;
    master->transfer_one_message = scmi_spi_transfer_one;
    master->cleanup = scmi_spi_cleanup;
    master->dev.of_node = pdev->dev.of_node;

    adap->clock_enabled = false;

    ret = spi_register_master(master);
    if (ret) {
        dev_err(&pdev->dev, "spi_register_master failed: %d\n", ret);
        goto err_free;
    }

    g_spi_adapters[g_num_spi_adapters++] = adap;
    dev_info(&pdev->dev, "scmi-spi adapter registered: bus=%u\n", adap->bus_num);
    return 0;

err_free:
    spi_master_put(master);
    return ret;
}

static int scmi_spi_remove(struct platform_device *pdev)
{
    /* 简化实现，清理 session */
    struct scmi_spi_adapter *adap = platform_get_drvdata(pdev);
    for (int i = 0; i < SCMI_SPI_MAX_HANDLES; i++) {
        if (g_spi_sessions[i].active)
            scmi_spi_close(adap, i);
    }
    scmi_spi_disable_clock(adap);
    spi_unregister_master(adap->master);
    return 0;
}

static const struct of_device_id scmi_spi_of_match[] = {
    { .compatible = "scmi,spi-adapter" },
    { },
};
MODULE_DEVICE_TABLE(of, scmi_spi_of_match);

static struct platform_driver scmi_spi_driver = {
    .probe  = scmi_spi_probe,
    .remove = scmi_spi_remove,
    .driver = {
        .name  = "scmi-spi",
        .of_match_table = scmi_spi_of_match,
    },
};
module_platform_driver(scmi_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("SCMI SPI adapter - uses SCMI clock + SPI protocols");