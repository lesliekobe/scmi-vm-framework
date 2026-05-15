/*
 * scmi_i2c_adapter.c - SCMI I2C 总线适配器 (Android 驱动)
 *
 * 模拟一个 I2C 总线适配器，底层通过 SCMI Clock protocol 开关时钟，
 * 通过 SCMI I2C protocol 进行数据传输
 *
 * 使用 platform_driver 框架，对接 Linux I2C subsystem (i2c-dev)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include "../virtio-scmi/scmi_client/scmi_client.h"

/* ==================== I2C 总线适配器私有数据 ==================== */

struct scmi_i2c_adapter {
    struct i2c_adapter   adap;       /* 适配器注册到 i2c-core */
    uint32_t             bus_id;     /* 总线 ID */

    /* I2C 设备列表 (从 DT 或 ACPI 获取) */
    uint32_t             num_devices;
    struct i2c_board_info devices[SCMI_I2C_MAX_DEVICES];

    /* 时钟 */
    uint32_t             clock_id;   /* I2C 总线使用的时钟 */
    bool                 clock_enabled;
};

static struct scmi_i2c_adapter *g_adapters[SCMI_I2C_MAX_BUSES];
static int g_num_adapters;

/* ==================== 时钟管理 (调用 scmi_client) ==================== */

static int scmi_i2c_enable_clock(struct scmi_i2c_adapter *adap)
{
    int ret;
    uint64_t rate;

    if (adap->clock_enabled)
        return 0;

    /* 先尝试从 SCMI 获取时钟速率 */
    ret = scmi_clock_get_rate(adap->clock_id, &rate);
    if (ret != 0) {
        dev_warn(&adap->adap.dev, "scmi_clock_get_rate failed: %d\n", ret);
        /* 继续尝试设置使能 */
    }

    /* 设置到 I2C 标准速率 400kHz */
    ret = scmi_clock_set_rate(adap->clock_id, 400000ULL, &rate);
    if (ret != 0) {
        dev_err(&adap->adap.dev, "scmi_clock_set_rate failed: %d\n", ret);
        return ret;
    }

    /* 使能时钟通知 (可选，用于调试) */
    scmi_clock_enable_notifications(adap->clock_id, false);

    adap->clock_enabled = true;
    dev_info(&adap->adap.dev, "clock %u enabled, rate=%llu Hz\n", adap->clock_id, rate);
    return 0;
}

static int scmi_i2c_disable_clock(struct scmi_i2c_adapter *adap)
{
    if (!adap->clock_enabled)
        return 0;

    /* SCMI 没有专门的时钟关闭接口，简化处理 */
    adap->clock_enabled = false;
    dev_info(&adap->adap.dev, "clock %u disabled\n", adap->clock_id);
    return 0;
}

/* ==================== I2C 传输实现 (通过 SCMI I2C Protocol) ==================== */

/*
 * 通过 virtio-scmi -> pvm -> mygearvm -> 实际 I2C 控制器
 * 这里用 scmi_client_do_xact 内部调用 (需导出或通过其他方式)
 *
 * 简化版：直接调用 scmi_i2c_* 函数 (需要服务端支持)
 * 实际应该: scmi_client_do_xact -> virtio_tx -> ...
 */

/* SCMI I2C Protocol IDs */
#define SCMI_I2C_PROTO  0x16

static int scmi_i2c_transfer_internal(struct scmi_i2c_adapter *adap,
                                       uint32_t dev_id,
                                       uint32_t flags,
                                       uint8_t *data, uint32_t len)
{
    /*
     * 实际应该通过 scmi_client 发起 SCMI I2C DEVICE_TRANSFER 消息
     *
     * struct i2c_transfer_msg {
     *     uint32_t dev_id;
     *     uint32_t dev_addr;
     *     uint32_t flags;
     *     uint32_t data_len;
     *     uint8_t  data[];
     * };
     *
     * return scmi_do_xact(SCMI_I2C_PROTO, 0x2, &msg, sizeof(msg),
     *                     resp, &resp_len);
     */

    /* 模拟实现：直接返回成功，实际是阻塞等待 SCMI 响应 */
    uint8_t req[8 + SCMI_I2C_MAX_DATA_LEN];
    uint32_t req_len = 8 + len;
    *(uint32_t *)(req + 0) = dev_id;
    *(uint32_t *)(req + 4) = flags;
    if (data && len)
        memcpy(req + 8, data, len);

    /* TODO: 调用 scmi_do_xact(SCMI_I2C_PROTO, 0x2, req, req_len, resp, &resp_len) */
    dev_dbg(&adap->adap.dev, "scmi_i2c_transfer: dev=%u flags=0x%x len=%u\n",
            dev_id, flags, len);

    return 0;  /* 简化：总是返回成功 */
}

/* ==================== Linux I2C Adapter_ops ==================== */

static int scmi_i2c_master_xfer(struct i2c_adapter *adap,
                                 struct i2c_msg *msgs,
                                 int num)
{
    struct scmi_i2c_adapter *scmi_adap = i2c_get_adapdata(adap);
    int ret;
    int i;

    /* 使能时钟 */
    ret = scmi_i2c_enable_clock(scmi_adap);
    if (ret)
        return ret;

    /* 遍历每个 i2c_msg */
    for (i = 0; i < num; i++) {
        struct i2c_msg *msg = &msgs[i];
        uint32_t flags = 0;

        if (msg->flags & I2C_M_RD)
            flags |= I2C_FLAG_READ;
        else
            flags |= I2C_FLAG_WRITE;

        if (i < num - 1 && (msgs[i + 1].flags & I2C_M_NOSTART) == 0)
            flags |= I2C_FLAG_STOP;

        ret = scmi_i2c_transfer_internal(scmi_adap, scmi_adap->bus_id,
                                        flags, msg->buf, msg->len);
        if (ret) {
            dev_err(&adap->dev, "transfer failed at msg %d: %d\n", i, ret);
            scmi_i2c_disable_clock(scmi_adap);
            return ret;
        }
    }

    /* 传输完成后可以考虑关闭时钟以省电 */
    scmi_i2c_disable_clock(scmi_adap);
    return num;
}

static u32 scmi_i2c_func(struct i2c_adapter *adap)
{
    (void)adap;
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_10BIT_ADDR;
}

static const struct i2c_algorithm scmi_i2c_algo = {
    .master_xfer    = scmi_i2c_master_xfer,
    .functionality = scmi_i2c_func,
};

/* ==================== 设备树支持 ==================== */

#ifdef CONFIG_OF
static int scmi_i2c_probe_dt(struct platform_device *pdev,
                              struct scmi_i2c_adapter *adap)
{
    struct device_node *np = pdev->dev.of_node;
    uint32_t val;
    int ret;

    if (!np)
        return -ENODEV;

    /* bus-id */
    ret = of_property_read_u32(np, "scmi,i2c-bus-id", &val);
    if (ret == 0)
        adap->bus_id = val;

    /* clock-id (SCMI clock ID for this I2C bus) */
    ret = of_property_read_u32(np, "scmi,clock-id", &val);
    if (ret == 0) {
        adap->clock_id = val;
    } else {
        dev_err(&pdev->dev, "missing scmi,clock-id property\n");
        return -EINVAL;
    }

    /* 可选: i2c device count */
    of_property_read_u32(np, "scmi,num-devices", &adap->num_devices);

    dev_info(&pdev->dev, "DT: bus_id=%u clock_id=%u num_devices=%u\n",
             adap->bus_id, adap->clock_id, adap->num_devices);

    return 0;
}
#else
static int scmi_i2c_probe_dt(struct platform_device *pdev,
                              struct scmi_i2c_adapter *adap) { return 0; }
#endif

/* ==================== Platform Driver ==================== */

static int scmi_i2c_probe(struct platform_device *pdev)
{
    struct scmi_i2c_adapter *adap;
    int ret;

    adap = devm_kzalloc(&pdev->dev, sizeof(*adap), GFP_KERNEL);
    if (!adap)
        return -ENOMEM;

    adap->adap.owner = THIS_MODULE;
    adap->adap.algo = &scmi_i2c_algo;
    adap->adap.retries = 3;
    adap->adap.dev.parent = &pdev->dev;
    adap->adap.class = I2C_CLASS_HWMON | I2C_CLASS_DEPRECATED;

    /* 名称格式: scmi-i2c-%d */
    snprintf(adap->adap.name, sizeof(adap->adap.name),
             "scmi-i2c-%u", g_num_adapters);

    platform_set_drvdata(pdev, adap);

    /* 从设备树获取配置 */
    ret = scmi_i2c_probe_dt(pdev, adap);
    if (ret)
        return ret;

    /* 初始化时钟状态 */
    adap->clock_enabled = false;

    /* 注册到 i2c-core */
    ret = i2c_add_adapter(&adap->adap);
    if (ret) {
        dev_err(&pdev->dev, "i2c_add_adapter failed: %d\n", ret);
        return ret;
    }

    g_adapters[g_num_adapters++] = adap;
    dev_info(&pdev->dev, "scmi-i2c adapter registered: %s\n", adap->adap.name);
    return 0;
}

static int scmi_i2c_remove(struct platform_device *pdev)
{
    struct scmi_i2c_adapter *adap = platform_get_drvdata(pdev);
    i2c_del_adapter(&adap->adap);
    return 0;
}

static const struct of_device_id scmi_i2c_of_match[] = {
    { .compatible = "scmi,i2c-adapter" },
    { },
};
MODULE_DEVICE_TABLE(of, scmi_i2c_of_match);

static struct platform_driver scmi_i2c_driver = {
    .probe  = scmi_i2c_probe,
    .remove = scmi_i2c_remove,
    .driver = {
        .name  = "scmi-i2c",
        .of_match_table = scmi_i2c_of_match,
    },
};
module_platform_driver(scmi_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("SCMI I2C adapter - uses SCMI clock + I2C protocols");