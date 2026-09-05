/*
 * sync_audio.h — Audio Hub 音频同步客户端（服务端 → 本机 Flash）
 *
 * 职责：sync_audio_files(token) 拉取服务端音频清单，与本地 TOC 比对后
 * 增量下载（流式直写 Flash，不占 RAM）、删除多余文件。由 main.cc 在
 * 设备激活后以后台任务调用；WiFi 省电的开/关也收在这里（下载前关省电，
 * 下载完恢复）。
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  从服务端同步音频文件到 SPI Flash。
 *
 * 流程:
 *   1. GET /api/device/v1/files → 获取服务端文件清单 (JSON)
 *   2. 与本地 TOC 对比，找出差异
 *   3. 删除本地多余文件
 *   4. 下载服务端新增/变更的文件
 *
 * 前置条件: WiFi 已连接, flash_audio_init() 已完成
 *
 * 服务端不可达时安全返回 ESP_FAIL, 不修改 flash 内容。
 *
 * @return ESP_OK  同步完成 (可能无需变更)
 *         ESP_FAIL 同步失败 (服务器不可达/网络错误), flash 保持原样
 */
esp_err_t sync_audio_files(const char *api_token);

#ifdef __cplusplus
}
#endif
