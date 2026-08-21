/*
 * wifi_config.h — WiFi 配对（参考小智 esp-wifi-connect 思路）
 *
 * 流程：
 *   1. 启动时从 NVS 读取已保存的 SSID/密码（没有则回退 config.h 默认值）。
 *   2. 若没有任何已保存凭据且连接失败 → 自动进入配网模式：
 *      开启 SoftAP "Panda-XXXX"，手机连上后访问 192.168.4.1（DNS 劫持跳转），
 *      网页选择/输入 WiFi 并提交 → 保存到 NVS → 切回 STA 连接。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** NVS 中是否已保存 WiFi 凭据 */
bool WifiConfigHasCredentials(void);

/** 读取已保存的 SSID/密码。返回 true 且填充缓冲区。 */
bool WifiConfigGetCredentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

/** 保存 SSID/密码到 NVS。 */
bool WifiConfigSaveCredentials(const char *ssid, const char *pass);

/** 开启配网模式（SoftAP + 配网网页 + DNS 劫持）。非阻塞。 */
void WifiConfigStartPortal(void);

/** 关闭配网模式（停止 DNS/HTTP/AP），并切回 STA 连接已保存凭据。 */
void WifiConfigStopPortal(void);

/** 配网模式是否在运行。 */
bool WifiConfigPortalRunning(void);

/** 从网页触发：进入配网模式（当前 STAs 仍在时切到 APSTA）。 */
void WifiConfigEnterFromWeb(void);

/** 扫描附近 AP，把结果写成 JSON 数组到 out。返回写入长度（0 表示失败）。 */
int WifiConfigScanAps(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
