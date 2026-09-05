/*
 * wifi.h — WiFi STA 连接管理（事件驱动 + 自动重连）
 *
 * 职责：InitWiFi 按"优先 NVS 已保存凭据"连 WiFi；断线自动重连，
 * 连续失败(4 次/8 秒)转投配网热点(wifi_config)；连上后启动 SNTP 校时。
 * 另提供：WaitForWiFi 阻塞等待、WiFiWaitForTimeSync（TLS 需要正确时钟）、
 * WiFiPowerSave 开关省电、WiFiIP/状态查询。
 * 配网热点 SoftAP/DNS/网页本身在 wifi_config.cc，不在这里。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 WiFi STA 并开始连接 (非阻塞) */
bool InitWiFi(void);

/** 阻塞等待 WiFi 获取 IP, 超时返回 false */
bool WaitForWiFi(int timeout_sec);

/** 等待 SNTP 将系统时间同步到可用于 TLS 证书校验的范围 */
bool WiFiWaitForTimeSync(int timeout_ms);

/** 获取当前 STA IP 字符串 */
const char *WiFiIP(void);

/** 获取当前已连接 AP 的 SSID 和信号强度 (dBm)。返回 true 表示已连接。 */
bool WifiConnectedApInfo(char *ssid, size_t ssid_sz, int *rssi);

/** 禁用/启用 WiFi 省电模式 (下载大文件时需关掉, 防止断连) */
void WiFiPowerSave(bool on);

/** 用给定凭据重连 STA（配网保存后调用），并关闭 AP 模式。 */
void WifiReconnectSta(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
