# SCMI VM Framework

ARM SCMI 跨虚拟机通信框架：pvm (Linux) ↔ mygearvm (ARM Trusted Firmware) via doorbell, pvm ↔ Android via virtio-scmi.

## 架构

```
                    ┌─────────────────────┐
                    │   Android Guest VM   │
                    │  (virtio-scmi driver)│
                    └──────────┬──────────┘
                               │ virtio-scmi (shared memory + virtqueue)
                    ┌──────────┴──────────┐
                    │   Linux PVM (Host)   │
                    │  ┌───────────────┐  │
                    │  │ scmi-dispatcher│  │
                    │  └───────┬───────┘  │
                    │     ┌────┴────┐     │
                    │     │ doorbell│     │
                    │     │ (scmi-xlnx)│   │
                    └──┬──┴─────────┴──┬──┘
                       │                │
            ┌───────────┘                └───────────┐
            │ ARM scmi门铃 (内存共享+中断)          │  virtio-scmi (virtqueue)
            │                                     │
┌───────────┴───────────┐              ┌───────────┴───────────┐
│   mygearvm (TF-A)     │              │   (future: 其他Guest) │
│  ┌─────────────────┐  │              └────────────────────────┘
│  │ scmi-firmware  │
│  │ (platform/MCP) │
│  └─────────────────┘  │
└──────────────────────┘
```

## 模块说明

| 模块 | 路径 | 说明 |
|------|------|------|
| 共享头文件 | `include/common/` | 协议定义、结构体、枚举 |
| PVM Linux SCMI | `pvm/linux/drivers/scmi/` | Linux 端 SCMI 驱动 |
| Doorbell 驱动 | `pvm/linux/drivers/scmi/doorbell/` | scmi-xlnx 风格 doorbell |
| mygearvm Core | `mygearvm/core/` | ARM Trusted Firmware 核心 |
| mygearvm Firmware | `mygearvm/firmware/` | SCMI Server (platform/scmi) |
| Android virtio-scmi | `android/virtio-scmi/` | virtio-scmi guest 驱动 |

## 通信通道

### 1. pvm ↔ mygearvm (Doorbell / scmi-xlnx)

```
[Linux PVM]  ───共享内存 (SCMI_SHM) ───► [mygearvm]
              ████████████████████
              ◄─── doorbell中断 ─────
```

- 共享内存区域：SCMI message + response buffer
- 触发方式：写 doorbell register (GPFR_EL3 or hypervisor trap)
- 典型实现：Xilinx ZynqMP scmi-xlnx 驱动

### 2. pvm ↔ Android (virtio-scmi)

```
[Android]  ───virtqueue (TX/RX) ───► [Linux PVM]
            ◄─── virtio中断 ─────────
```

- 标准 virtio-scmi 设备（virtio 1.1+，支持packed virtqueue）
- Android 为 virtio guest，Linux 为 virtio host

## SCMI 协议支持

- [x] SCMI Base Protocol (必选)
- [ ] SCMI Clock Protocol
- [ ] SCMI Reset Domain Protocol
- [ ] SCMI Power Domain Protocol
- [ ] SCMI System Power Protocol
- [ ] SCMI Perf Protocol
- [ ] SCMI Sensor Protocol

## 编译

```bash
# mygearvm (ARM Trusted Firmware)
make -C mygearvm/firmware PLAT=<soc>

# pvm Linux (Yocto / kernel build)
# 放入 kernel 源码目录后编译
# CONFIG_SCMI_XLNX=m

# Android virtio-scmi
# 放入 Android kernel 或 vendor 分支编译
# CONFIG_VIRTIO_SCMI=y
```

## TODO

- [ ] Doorbell 驱动完整实现
- [ ] mygearvm SCMI firmware 服务端
- [ ] virtio-scmi host 端 (Linux kernel driver)
- [ ] Android virtio-scmi guest 驱动
- [ ] SCMI 协议族完整实现
- [ ] 测试框架 / QEMU 仿真

## 参考

- ARM SCMI spec: [DEN0056](https://developer.arm.com/documentation/den0056/)
- Linux kernel scmi driver: `drivers/firmware/arm_scmi/`
- scmi-xlnx: Xilinx Linux kernel SCMI doorbell driver
- virtio-scmi: Linux kernel `drivers/virtio/virtio_scmi.c`