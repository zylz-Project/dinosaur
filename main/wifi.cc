#include "wifi.h"
#include "config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <cstring>

static const char *TAG = "wifi";
static char g_ip[16] = "0.0.0.0";
static EventGroupHandle_t g_evt = nullptr;

#define BIT_CONNECTED  BIT0

/* ---- event handler ---- */
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        int reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        // reason 2  = router redirect (normal)
        // reason 200 = beacon timeout (WiFi PS too aggressive)
        if (reason != 2)
            ESP_LOGW(TAG, "Disconnected (reason=%d), reconnecting...", reason);
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *evt = (ip_event_got_ip_t *)data;
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Connected: %s", g_ip);
        if (g_evt) xEventGroupSetBits(g_evt, BIT_CONNECTED);
    }
}

/* ---- public ---- */
bool InitWiFi(void)
{
    g_evt = xEventGroupCreate();
    if (!g_evt) return false;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t h;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr, &h);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr, &h);

    wifi_config_t wcfg = {};
    strcpy((char *)wcfg.sta.ssid, WIFI_STA_SSID);
    strcpy((char *)wcfg.sta.password, WIFI_STA_PASSWORD);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_STA_SSID);
    return true;
}

bool WaitForWiFi(int timeout_sec)
{
    if (!g_evt) return false;

    EventBits_t bits = xEventGroupWaitBits(g_evt, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_sec * 1000));
    if (bits & BIT_CONNECTED) return true;

    ESP_LOGW(TAG, "WiFi timeout (%ds), continue offline", timeout_sec);
    return false;
}

const char *WiFiIP(void)
{
    return g_ip;
}

void WiFiPowerSave(bool on)
{
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    ESP_LOGI(TAG, "Power save: %s", on ? "ON" : "OFF");
}
