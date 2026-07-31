#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 WiFi STA 并开始连接 (非阻塞) */
bool InitWiFi(void);

/** 阻塞等待 WiFi 获取 IP, 超时返回 false */
bool WaitForWiFi(int timeout_sec);

/** 获取当前 STA IP 字符串 */
const char *WiFiIP(void);

/** 禁用/启用 WiFi 省电模式 (下载大文件时需关掉, 防止断连) */
void WiFiPowerSave(bool on);

#ifdef __cplusplus
}
#endif
