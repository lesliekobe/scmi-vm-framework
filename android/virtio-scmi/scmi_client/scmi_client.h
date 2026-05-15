/*
 * scmi_client.h - Android SCMI Client 库头文件
 *
 * 给 Android 驱动提供的 SCMI 访问接口
 * 底层通过 virtio-scmi 传输
 */

#ifndef __SCMI_CLIENT_H
#define __SCMI_CLIENT_H

#include <stdint.h>
#include <linux/wait.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 公共API ==================== */

/* 初始化/销毁 */
int  scmi_client_init(void);
void scmi_client_deinit(void);

/* 绑定传输层 (virtio-scmi 驱动调用) */
int scmi_client_set_transport(int (*send_fn)(uint32_t proto, uint32_t msg_id,
                                             uint32_t token,
                                             const uint8_t *in, uint32_t in_len));

/* 响应回调 (由 virtio transport 在收到响应时调用) */
void scmi_rx_complete(uint32_t token, int32_t status, const void *data, size_t len);

/* ==================== Base Protocol ==================== */

int scmi_base_get_version(uint32_t *version);                /* msg 0x0 */
int scmi_base_get_attributes(uint32_t *num_protocols, uint32_t *impl_version); /* msg 0x1 */
int scmi_base_vendor_ident(char *vendor_id, size_t max_len); /* msg 0x3 */

/* ==================== Clock Protocol ==================== */

int scmi_clock_get_num_clocks(uint32_t *num);                /* msg 0x0 */
int scmi_clock_get_attributes(uint32_t clock_id, uint32_t *attributes); /* msg 0x1 */
int scmi_clock_get_rate(uint32_t clock_id, uint64_t *rate);   /* msg 0x2 */
int scmi_clock_set_rate(uint32_t clock_id, uint64_t rate,     /* msg 0x3 */
                        uint64_t *actual_rate);
int scmi_clock_enable_notifications(uint32_t clock_id, bool enable); /* msg 0x4 */

/* ==================== 配置 ==================== */

#define SCMI_MAX_PENDING  16   /* 并发事务数上限 */

#ifdef __cplusplus
}
#endif

#endif /* __SCMI_CLIENT_H */