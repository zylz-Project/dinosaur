#pragma once

#include <freertos/FreeRTOS.h>

#include <cstddef>

#define DEVICE_API_TOKEN_SIZE 96

/**
 * 启动 Audio Hub 设备注册、激活轮询和心跳后台任务。
 * 调用前必须已连接 Wi-Fi；重复调用不会创建重复任务。
 */
void device_registry_start(void);

bool device_registry_wait_for_activation(TickType_t timeout_ticks);
bool device_registry_get_api_token(char *output, size_t output_size);
