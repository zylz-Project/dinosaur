#include "config.h"
#include "audio.h"
#include "auto_run.h"
#include "chat.h"
#include "device_registry.h"
#include "flash_audio.h"
#include "http_server.h"
#include "power.h"
#include "servo.h"
#include "sync_audio.h"
#include "wifi.h"
#include "wifi_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "dino_main";

extern "C" void app_main()
{
    nvs_flash_init();
    InitPower();
    PowerSetButtonCallback(ChatToggle);  // 双击电源键 → 切换 AI 对话
    InitAudio();
    InitServos();
    PlayBootTone();  // immediate "power on" chime, before the slower WiFi/sync path

#if !OFFLINE_DEMO
    // --- WiFi (chat depends on it; 配网: 连不上就进配网热点) ---
    InitWiFi();
    bool has_creds = WifiConfigHasCredentials();
#ifdef WIFI_STA_SSID
    if (!has_creds) has_creds = (WIFI_STA_SSID[0] != '\0');  // config.h 默认值也算有凭据
#endif
    bool online = false;
    if (has_creds) {
        online = WaitForWiFi(WIFI_STA_TIMEOUT_S);
    }
    // 无任何凭据: 不等, 立刻开热点; 有凭据但连不上(如在外无信号): 超时后也开热点,
    // 这样任何情况下都能重新配网。
    if (!online) {
        ESP_LOGI(TAG, "WiFi offline%s — starting config portal",
                 has_creds ? " (credentials unreachable)" : " (no saved credentials)");
        WifiConfigStartPortal();
    }
#endif

    flash_audio_init();

#if !OFFLINE_DEMO
    // Start HTTP early (available during sync)
    StartHttpServer();

#if AUDIO_SYNC_ENABLE
    // --- Sync audio files from server (background task; never blocks chat) ---
    // 音频服务端和大模型互不干涉: 未激活/连不上音频服务端时跳过即可,
    // 不阻塞主流程, 对话随时可用。
    if (online) {
        device_registry_start();
        xTaskCreate([](void *) {
            if (!device_registry_wait_for_activation(pdMS_TO_TICKS(30000))) {
                ESP_LOGW(TAG, "设备未激活(超时)，跳过音频同步");
                vTaskDelete(nullptr);
                return;
            }
            char api_token[DEVICE_API_TOKEN_SIZE] = {};
            if (device_registry_get_api_token(api_token, sizeof(api_token))) {
                sync_audio_files(api_token);   // 省电开关由 sync_audio 内部管理
            } else {
                ESP_LOGE(TAG, "无法读取已验证的设备令牌，跳过同步");
            }
            vTaskDelete(nullptr);
        }, "audio_sync", 12288, nullptr, 3, nullptr);
    } else {
        ESP_LOGW(TAG, "Offline — using existing flash content");
    }
#else
    ESP_LOGI(TAG, "Audio sync disabled (AUDIO_SYNC_ENABLE=0) — using existing flash content");
#endif
#else  // !OFFLINE_DEMO
    ESP_LOGI(TAG, "Offline demo — skip WiFi, use flash audio");
#endif  // !OFFLINE_DEMO

#if ENABLE_AUTO_RUN
    InitAutoRun();
    TriggerDinoGreeting();
#endif

    // --- LLM chat (double-click power button to toggle) ---
    ChatInit();
}
