/*
 * device_registry.h — Audio Hub 设备注册/激活/心跳
 *
 * 职责：用出厂 MAC 生成设备 ID，向音频服务端注册并轮询激活状态
 * （串口打印 6 位激活码，管理员在服务端后台绑定），激活后拿 API token
 * 给 sync_audio 用，之后每 60s 上报心跳。token 保存在 NVS。
 */
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
