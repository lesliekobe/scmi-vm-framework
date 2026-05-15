# SCMI VM Framework

ARM SCMI 跨虚拟机通信框架：pvm (Linux) ↔ mygearvm (ARM Trusted Firmware) via doorbell, pvm ↔ Android via virtio-scmi.

## 架构

```
                    ┌─────────────────────┐
                    │   Android Guest VM   │
                    │  (scmi_i2c_adapter)  │
                    │  (scmi_spi_adapter)  │
                    └──────────┬──────────┘
                               │ scmi_client
                    ┌──────────┴──────────┐
                    │  virtio-scmi driver │
                    │  (virtqueue A2P/P2A)│
                    └──────────┬──────────┘
                               │ virtio-scmi
                    ┌──────────┴──────────┐
                    │   Linux PVM (Host)   │
                    │  ┌───────────────┐  │
                    │  │ scmi-dispatcher│  │
                    │  └───────┬───────┘  │
                    │     ┌────┴────┐     │
                    │     │ doorbell│     │
                    │     │ (scmi-xlnx)│  │
                    └──┬──┴─────────┴──┬──┘
                       │                │
            ┌───────────┘                └───────────┐
            │ ARM scmi门铃 (内存共享+中断)          │
            │                                     │
┌───────────┴───────────┐
│   mygearvm (TF-A)     │
│  ┌─────────────────┐ │
│  │ scmi-firmware   │ │
│  │ Base/Clock/     │ │
│  │ Power/Perf/     │ │
│  │ I2C/SPI server  │ │
│  └─────────────────┘ │
└──────────────────────┘
```

## 模块说明

| 模块 | 路径 | 说明 |
|------|------|------|
| 共享头文件 | `include/common/` | 协议定义、结构体、枚举 |
| PVM Linux SCMI | `pvm/linux/drivers/scmi/` | Linux 端 SCMI 驱动 |
| Doorbell 驱动 | `pvm/linux/drivers/scmi/doorbell/` | scmi-xlnx 风格 doorbell |
| mygearvm Core | `mygearvm/core/` | ARM Trusted Firmware 核心 |
| mygearvm Firmware | `mygearvm/firmware/` | SCMI Server 实现 (Base/Clock/Power/Perf/I2C/SPI) |
| Android SCMI Client | `android/virtio-scmi/scmi_client/` | SCMI 客户端库 (同步事务、token管理) |
| Android virtio-scmi | `android/virtio-scmi/` | virtio-scmi guest 驱动 (transport层) |
| Android I2C Adapter | `android/drivers/i2c/` | Linux I2C 子系统适配器，通过 SCMI Clock+I2C |
| Android SPI Adapter | `android/drivers/spi/` | Linux SPI 子系统适配器，通过 SCMI Clock+SPI |

## SCMI 协议支持

| Protocol | ID | 状态 | 说明 |
|----------|------|------|------|
| Base | 0x10 | ✅ 完整 | 版本/属性/厂商识别 |
| Clock | 0x11 | ✅ 完整 | 频率 Get/Set/Notify |
| Power | 0x12 | ✅ 完整 | 电源域 On/Off/Suspend/Notify |
| Perf | 0x13 | ✅ 完整 | DVFS + Limits |
| I2C | 0x16 | ✅ 完整 | 总线读写传输 |
| SPI | 0x17 | ✅ 完整 | Open/Transfer/Close |
| Sensors | 0x15 | 🔲 待添加 | 传感器协议 |
| SysPower | 0x14 | 🔲 待添加 | 系统电源管理 |

## 编译

```bash
# mygearvm (ARM Trusted Firmware)
make -C mygearvm/firmware PLAT=<soc>

# pvm Linux (Yocto / kernel build)
# 放入 kernel 源码目录后编译
# CONFIG_SCMI_XLNX=m

# Android virtio-scmi + 驱动
# 放入 Android kernel 或 vendor 分支编译
# CONFIG_VIRTIO_SCMI=y
# CONFIG_SCMI_I2C_ADAPTER=m
# CONFIG_SCMI_SPI_ADAPTER=m
```

## Android 驱动使用流程

```
应用/用户空间
    │
    ▼
/dev/i2c-X  (i2c-dev)
    │
    ▼
scmi_i2c_adapter.ko   ◄─── SCMI Clock protocol (开关/调速时钟)
    │                  ◄─── SCMI I2C protocol (实际 I2C 传输)
    │
    ▼
virtio_scmi.ko  ◄───── scmi_client.ko (同步事务管理)
    │                    │
    ▼                    ▼
    ───────── virtqueue (A2P/P2A) ──────────►
                   pvm Linux host
```

## TODO

- [ ] Doorbell 驱动完整实现 (绑定真实寄存器地址)
- [ ] mygearvm SCMI firmware 服务端完整实现
- [ ] virtio-scmi host 端 (pvm Linux kernel driver)
- [ ] Android virtio-scmi guest 驱动 (完成 virtqueue 集成)
- [ ] 测试框架 / QEMU 仿真
- [ ] Sensors / SysPower 协议

## 参考

- ARM SCMI spec: [DEN0056](https://developer.arm.com/documentation/den0056/)
- Linux kernel scmi driver: `drivers/firmware/arm_scmi/`
- scmi-xlnx: Xilinx Linux kernel SCMI doorbell driver
- virtio-scmi: Linux kernel `drivers/virtio/virtio_scmi.c`