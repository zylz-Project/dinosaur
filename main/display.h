/*
 * display.h — 仅日志垫片 (display shim)
 *
 * 移植自 chat_ws_espidf 的 realtime_ws.c 原本依赖 OLED 显示层；
 * 小熊猫硬件没有屏幕，这里只保留 display_set_ws 一个空实现（打日志），
 * 作为连接状态的日志输出口。如果未来加屏幕，把这里换成真显示驱动即可。
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

#ifdef __cplusplus
}
#endif
