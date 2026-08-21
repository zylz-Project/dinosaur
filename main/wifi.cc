#include "wifi.h"
#include "config.h"
#include "wifi_config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static const char *TAG = "wifi";
static char g_ip[16] = "0.0.0.0";
static char g_cur_ssid[33] = {};
static char g_cur_pass[65] = {};
static EventGroupHandle_t g_evt = nullptr;
static int g_disconnect_streak = 0;
static TickType_t g_first_disconnect_tick = 0;
static volatile bool g_portal_task_pending = false;

#define BIT_CONNECTED  BIT0
#define PORTAL_FALLBACK_DISCONNECTS 4
#define PORTAL_FALLBACK_MS 8000

static void portal_fallback_task(void *arg)
{
    (void)arg;
    /* Leave the Wi-Fi event callback before changing STA/AP mode. */
    vTaskDelay(pdMS_TO_TICKS(100));
    bool connected = g_evt && (xEventGroupGetBits(g_evt) & BIT_CONNECTED);
    if (!connected && !WifiConfigPortalRunning()) {
        ESP_LOGW(TAG,
                 "STA unavailable after %d disconnects — starting config portal",
                 g_disconnect_streak);
        WifiConfigStartPortal();
    }
    g_portal_task_pending = false;
    vTaskDelete(nullptr);
}

static void schedule_portal_fallback(void)
{
    if (g_portal_task_pending || WifiConfigPortalRunning()) return;
    g_portal_task_pending = true;
    if (xTaskCreate(portal_fallback_task, "wifi_portal", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        g_portal_task_pending = false;
        ESP_LOGE(TAG, "Failed to create config portal fallback task");
    }
}

static void start_time_sync(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP time sync started");
    }
}

/* ---- event handler ---- */
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!WifiConfigPortalRunning()) esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Config SoftAP is active; connect to Panda-XXXX at 192.168.4.1");

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        auto *evt = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Provisioning client joined, aid=%d", evt->aid);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        int reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        strlcpy(g_ip, "0.0.0.0", sizeof(g_ip));
        if (g_evt) xEventGroupClearBits(g_evt, BIT_CONNECTED);

        /* 配网模式下停止用旧凭据循环连接。否则重连会与扫描争用 STA，
         * 导致 SoftAP/配网页面不稳定甚至手机看不到热点。 */
        if (WifiConfigPortalRunning()) {
            ESP_LOGI(TAG, "STA disconnected (reason=%d), config portal remains active",
                     reason);
            return;
        }

        TickType_t now = xTaskGetTickCount();
        if (g_first_disconnect_tick == 0) g_first_disconnect_tick = now;
        g_disconnect_streak++;
        int offline_ms = (int)((now - g_first_disconnect_tick) * portTICK_PERIOD_MS);
        // reason 2  = router redirect (normal)
        // reason 200 = beacon timeout (WiFi PS too aggressive)
        if (reason != 2)
            ESP_LOGW(TAG,
                     "Disconnected (reason=%d), reconnecting... (%d/%d, %dms)",
                     reason, g_disconnect_streak, PORTAL_FALLBACK_DISCONNECTS,
                     offline_ms);

        if (g_disconnect_streak >= PORTAL_FALLBACK_DISCONNECTS ||
            offline_ms >= PORTAL_FALLBACK_MS) {
            schedule_portal_fallback();
        } else {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK)
                ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *evt = (ip_event_got_ip_t *)data;
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Connected: %s", g_ip);
        g_disconnect_streak = 0;
        g_first_disconnect_tick = 0;
        if (g_evt) xEventGroupSetBits(g_evt, BIT_CONNECTED);
        start_time_sync();

        /* 成功连接后自动把当前凭据保存到 NVS, 下次开机自动连 */
        if (g_cur_ssid[0]) {
            char saved_ssid[33] = {}, saved_pass[65] = {};
            if (!WifiConfigGetCredentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) ||
                strcmp(saved_ssid, g_cur_ssid) != 0) {
                if (WifiConfigSaveCredentials(g_cur_ssid, g_cur_pass))
                    ESP_LOGI(TAG, "Saved connected WiFi '%s' to NVS", g_cur_ssid);
            }
        }
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

    /* 优先使用 NVS 已保存凭据，否则回退 config.h 默认值(若已定义) */
    wifi_config_t wcfg = {};
    char ssid[33] = {}, pass[65] = {};
    if (WifiConfigGetCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
        strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    } else {
#ifdef WIFI_STA_SSID
        strcpy((char *)wcfg.sta.ssid, WIFI_STA_SSID);
#endif
#ifdef WIFI_STA_PASSWORD
        strcpy((char *)wcfg.sta.password, WIFI_STA_PASSWORD);
#endif
    }
    /* 记录本次使用的凭据, 用于连接成功后自动存入 NVS */
    strncpy(g_cur_ssid, (const char *)wcfg.sta.ssid, sizeof(g_cur_ssid) - 1);
    strncpy(g_cur_pass, (const char *)wcfg.sta.password, sizeof(g_cur_pass) - 1);
    /* Allow open/WPA/WPA2/WPA3 networks selected from the provisioning page. */
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to %s...", (const char *)wcfg.sta.ssid);
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

bool WiFiWaitForTimeSync(int timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 0);

    do {
        time_t now = time(nullptr);
        struct tm utc = {};
        gmtime_r(&now, &utc);
        if (utc.tm_year + 1900 >= 2024) {
            ESP_LOGI(TAG, "System time ready for TLS: %04d-%02d-%02d %02d:%02d:%02d UTC",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (xTaskGetTickCount() - start < timeout);

    ESP_LOGW(TAG, "SNTP time sync timeout (%d ms)", timeout_ms);
    return false;
}

const char *WiFiIP(void)
{
    return g_ip;
}

bool WifiConnectedApInfo(char *ssid, size_t ssid_sz, int *rssi)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid && ssid_sz) strlcpy(ssid, (const char *)ap.ssid, ssid_sz);
    if (rssi) *rssi = ap.rssi;
    return true;
}

void WiFiPowerSave(bool on)
{
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    ESP_LOGI(TAG, "Power save: %s", on ? "ON" : "OFF");
}

void WifiReconnectSta(const char *ssid, const char *password)
{
    if (!ssid) return;
    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    if (password) strncpy((char *)wcfg.sta.password, password, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    strlcpy(g_cur_ssid, ssid, sizeof(g_cur_ssid));
    strlcpy(g_cur_pass, password ? password : "", sizeof(g_cur_pass));
    strlcpy(g_ip, "0.0.0.0", sizeof(g_ip));
    if (g_evt) xEventGroupClearBits(g_evt, BIT_CONNECTED);
    g_disconnect_streak = 0;
    g_first_disconnect_tick = 0;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Reconnect STA '%s' failed: %s", ssid, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "Reconnecting STA to %s...", ssid);
}
