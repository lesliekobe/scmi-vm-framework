/*
 * scmi_xlnx.c - scmi-xlnx (doorbell) 设备驱动 (Linux kernel风格)
 *
 * 兼容 Xilinx ZynqMP 的 scmi-xlnx 驱动框架
 * 注册到 Linux SCMI core，底层通过 doorbell 与 mygearvm 通信
 *
 * 编译:
 *   <Linux kernel>/drivers/firmware/arm_scmi/ $ make M=drivers/firmware/arm_scmi/
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/interrupt.h>
#include <linux/firmware/xlnx/scmi_xlnx.h>  /* Xilinx SCMI platform hook */

#define DRIVER_NAME "scmi-xlnx"

/* ==================== 私有数据结构 ==================== */

struct scmi_xlnx_priv {
    struct device *dev;
    void __iomem *db_base;        /* doorbell 寄存器基址 */
    uint32_t db_stride;           /* channel间寄存器步进 */
    uint32_t num_channels;
    uint64_t shm_phys;

    /* 复用的 channel list */
    struct list_head free_list;
    struct list_head busy_list;
    spinlock_t lock;
};

static struct scmi_xlnx_priv *g_priv;

/* ==================== 硬件操作 ==================== */

/* 读/写 doorbell 寄存器 */
#define db_readl(priv, off)     readl((priv)->db_base + (off))
#define db_writel(priv, off, v) writel((v), (priv)->db_base + (off))

static int xlnx_db_send(struct scmi_xlnx_priv *priv,
                        uint32_t ch, uint64_t msg_addr, uint32_t msg_size)
{
    uint32_t offset = ch * priv->db_stride;

    /* 写消息地址 */
    db_writel(priv, offset + DOORBELL_REG_MESSAGE_LO, (uint32_t)msg_addr);
    db_writel(priv, offset + DOORBELL_REG_MESSAGE_HI, (uint32_t)(msg_addr >> 32));
    db_writel(priv, offset + DOORBELL_REG_MESSAGE_SIZE, msg_size);

    /* 写 doorbell 触发 */
    db_writel(priv, offset + DOORBELL_REG_DOORBELL,
              (1U << DOORBELL_BIT_VALID) | (0U << DOORBELL_BIT_FROM_PVM));

    mb();  /* memory barrier */
    dev_dbg(priv->dev, "ch%u doorbell sent\n", ch);
    return 0;
}

/* ==================== SCMI 平台回调 ==================== */

/*
 * xlnx_scmi_send - SCMI core 调用此函数发送消息到底层
 * 实现: 将消息放入共享内存，触发 doorbell
 */
static int xlnx_scmi_send(struct scmi_fwk_dev *sfdev,
                          uint32_t channel_id,
                          uint8_t protocol_id,
                          uint8_t msg_id,
                          uint32_t in_len,
                          void *in_payload,
                          uint32_t *out_len,
                          void *out_payload)
{
    struct scmi_xlnx_priv *priv = dev_get_drvdata(sfdev->dev);
    uint64_t shm_addr = priv->shm_phys + (channel_id * SCMI_SHM_CHANNEL_SIZE);

    /* 组帧: 消息头 + payload */
    struct scmi_msg_hdr {
        uint32_t hdr;
        uint8_t payload[SCMI_MAX_PAYLOAD];
    } __attribute__((packed));

    struct scmi_msg_hdr *msg = (struct scmi_msg_hdr *)shm_addr;
    msg->hdr = (protocol_id << 10) | (msg_id << 0);
    memcpy(msg->payload, in_payload, in_len);

    /* 触发 doorbell */
    return xlnx_db_send(priv, channel_id, shm_addr,
                        sizeof(msg->hdr) + in_len);
}

/* ==================== 中断处理 ==================== */

static irqreturn_t xlnx_scmi_irq(int irq, void *arg)
{
    struct scmi_xlnx_priv *priv = arg;
    uint32_t ch;

    for (ch = 0; ch < priv->num_channels; ch++) {
        uint32_t offset = ch * priv->db_stride;
        uint32_t irq_stat = db_readl(priv, offset + DOORBELL_REG_IRQ_STATUS);

        if (irq_stat & DOORBELL_IRQ_MASK) {
            db_writel(priv, offset + DOORBELL_REG_IRQ_CLEAR, DOORBELL_IRQ_MASK);

            dev_dbg(priv->dev, "doorbell irq ch%u\n", ch);

            /* 通知 SCMI core: channel ch 有响应 */
            scmi_rx_callback(priv->dev, ch);
        }
    }

    return IRQ_HANDLED;
}

/* ==================== 驱动probe ==================== */

static int xlnx_scmi_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct resource *res;
    int ret, irq;

    g_priv = devm_kzalloc(dev, sizeof(*g_priv), GFP_KERNEL);
    if (!g_priv)
        return -ENOMEM;

    g_priv->dev = dev;
    spin_lock_init(&g_priv->lock);

    /* doorbell 寄存器映射 */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    g_priv->db_base = devm_ioremap_resource(dev, res);
    if (IS_ERR(g_priv->db_base))
        return PTR_ERR(g_priv->db_base);

    /* 获取 stride 和 channel 数 (device tree) */
    if (of_property_read_u32(dev->of_node, "xlnx,db-stride", &g_priv->db_stride))
        g_priv->db_stride = 0x20;

    if (of_property_read_u32(dev->of_node, "xlnx,num-channels", &g_priv->num_channels))
        g_priv->num_channels = 2;

    if (of_property_read_u64(dev->of_node, "xlnx,shm-phys", &g_priv->shm_phys))
        g_priv->shm_phys = 0xFF800000UL;

    /* 中断 */
    irq = platform_get_irq(pdev, 0);
    if (irq > 0) {
        ret = devm_request_irq(dev, irq, xlnx_scmi_irq,
                               IRQF_SHARED, DRIVER_NAME, g_priv);
        if (ret)
            dev_warn(dev, "failed to request irq %d\n", irq);
    }

    /* TODO: 注册到 SCMI core */
    /* scmi_xlnx_register(scmi_xlnx_ops, g_priv); */

    platform_set_drvdata(pdev, g_priv);
    dev_info(dev, "scmi-xlnx probed: %u channels, shm=0x%lx\n",
             g_priv->num_channels, g_priv->shm_phys);

    return 0;
}

static const struct of_device_id xlnx_scmi_of_match[] = {
    { .compatible = "xlnx,scmi-xlnx-1.0" },
    { },
};
MODULE_DEVICE_TABLE(of, xlnx_scmi_of_match);

static struct platform_driver xlnx_scmi_driver = {
    .probe  = xlnx_scmi_probe,
    .driver = {
        .name  = DRIVER_NAME,
        .of_match_table = xlnx_scmi_of_match,
    },
};
module_platform_driver(xlnx_scmi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SCMI VM Framework");
MODULE_DESCRIPTION("Xilinx SCMI doorbell driver for pvm<->mygearvm communication");