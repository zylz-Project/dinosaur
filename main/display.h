/*
 * display.h — 占位垫片 (display shim)
 *
 * 移植自 chat_ws_espidf 的 realtime_ws.c 依赖小熊猫没有的 OLED 显示层。
 * 这里提供与真实 display 相同签名的空实现（仅打日志），
 * 让 realtime_ws.c 无需改动即可编译运行。
 */
#pragma once

#include <stdbool.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void display_set_ws(bool connected, bool ready)
{
    ESP_LOGI("display", "WS connected=%d ready=%d", connected ? 1 : 0, ready ? 1 : 0);
}

static inline void display_set_state(const char *state)
{
    ESP_LOGI("display", "state=%s", state ? state : "?");
}

static inline void display_set_status(const char *status)
{
    ESP_LOGI("display", "status=%s", status ? status : "?");
}

static inline void display_set_wifi(bool ok)
{
    ESP_LOGI("display", "wifi=%d", ok ? 1 : 0);
}

static inline void display_set_ip(const char *ip)
{
    ESP_LOGI("display", "ip=%s", ip ? ip : "?");
}

#ifdef __cplusplus
}
#endif
