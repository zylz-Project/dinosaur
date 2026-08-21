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
