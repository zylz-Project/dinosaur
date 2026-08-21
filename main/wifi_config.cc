/*
 * wifi_config.cc — WiFi 配对（参考小智 esp-wifi-connect 思路）
 *
 * 配网网页由 main 的 http_server 提供（/api/wifi/ 系列端点），本文件只负责：
 *   - NVS 凭据读写
 *   - SoftAP "Panda-XXXX" 启停
 *   - DNS 劫持（把任意域名解析到 192.168.4.1，手机连上 AP 自动跳转配网页）
 *   - 保存凭据后切回 STA 连接
 */

#include "wifi_config.h"
#include "config.h"
#include "wifi.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

static const char *TAG = "wifi_config";

#define WIFI_NVS_NS "wifi"

static volatile bool s_portal_running = false;
static TaskHandle_t s_dns_task = nullptr;
static volatile bool s_dns_running = false;
static int s_dns_sock = -1;

/* ===================================================================
 *  NVS 凭据
 * =================================================================== */
bool WifiConfigHasCredentials(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[33] = {}, pass[65] = {};
    size_t l1 = sizeof(ssid), l2 = sizeof(pass);
    bool ok = (nvs_get_str(h, "ssid", ssid, &l1) == ESP_OK && ssid[0] &&
               nvs_get_str(h, "password", pass, &l2) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool WifiConfigGetCredentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    if (!ssid || !pass || ssid_sz == 0 || pass_sz == 0) return false;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l1 = ssid_sz, l2 = pass_sz;
    esp_err_t r1 = nvs_get_str(h, "ssid", ssid, &l1);
    esp_err_t r2 = nvs_get_str(h, "password", pass, &l2);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK);
}

bool WifiConfigSaveCredentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass || !ssid[0]) return false;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t r1 = nvs_set_str(h, "ssid", ssid);
    esp_err_t r2 = nvs_set_str(h, "password", pass);
    nvs_commit(h);
    nvs_close(h);
    bool ok = (r1 == ESP_OK && r2 == ESP_OK);
    ESP_LOGI(TAG, "Saved credentials for '%s' (%s)", ssid, ok ? "OK" : "FAIL");
    return ok;
}

/* ===================================================================
 *  SoftAP
 * =================================================================== */
static esp_netif_t *s_ap_netif = nullptr;

static void make_ap_ssid(char *buf, size_t n)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, n, "Dino-%02X%02X", mac[4], mac[5]);
}

static bool start_softap(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "Failed to create SoftAP network interface");
            return false;
        }
    }

    wifi_config_t wc = {};
    make_ap_ssid((char *)wc.ap.ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = (uint8_t)strlen((const char *)wc.ap.ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.ssid_hidden = 0;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set APSTA mode failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Configure SoftAP failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Stop attempts using stale credentials. The disconnect callback sees
     * s_portal_running and deliberately does not start another STA attempt. */
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT)
        ESP_LOGW(TAG, "Stop stale STA connection failed: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "SoftAP '%s' started (192.168.4.1)", (const char *)wc.ap.ssid);
    return true;
}

static void stop_softap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/* ===================================================================
 *  DNS 劫持（任意 A 查询 → AP IP，实现 captive portal 跳转）
 * =================================================================== */
#define DNS_PORT 53

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t qclass;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

static uint32_t get_ap_ip(void)
{
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &info) == ESP_OK)
        return info.ip.addr;
    return 0;
}

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_ANY);
    dest.sin_port = htons(DNS_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    if (bind(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = sock;

    uint32_t ap_ip = get_ap_ip();
    ESP_LOGI(TAG, "DNS server listening, answering -> " IPSTR, IP2STR((ip4_addr_t *)&ap_ip));

    uint8_t rx[256];
    while (s_dns_running) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &sl);
        if (!s_dns_running) break;  /* socket closed by stop_dns */
        if (len <= 0) continue;
        if (len < (int)sizeof(dns_header_t)) continue;

        uint8_t reply[512];
        memcpy(reply, rx, (size_t)len);
        dns_header_t *h = (dns_header_t *)reply;
        h->flags = htons(ntohs(h->flags) | 0x8000); /* QR=response */
        uint16_t qd = ntohs(h->qd_count);
        uint16_t answers = 0;
        h->an_count = 0;
        h->ns_count = 0;
        h->ar_count = 0;

        int off = (int)sizeof(dns_header_t);
        for (uint16_t i = 0; i < qd; i++) {
            int question_name_offset = off;
            /* 跳过 question name（标签序列） */
            while (off < len && rx[off] != 0) {
                int label_len = rx[off];
                if (label_len > 63 || off + label_len + 1 >= len) {
                    off = len;
                    break;
                }
                off += label_len + 1;
            }
            if (off >= len) break;
            off++; /* 终止符 */
            if (off + 4 > len) break;
            /* question: type(2) + class(2) */
            int q_type = (rx[off] << 8) | rx[off + 1];
            off += 4;

            if (q_type == 1 && off + (int)sizeof(dns_answer_t) <= (int)sizeof(reply)) {
                dns_answer_t *a = (dns_answer_t *)(reply + off);
                /* 指针指向对应 question 的 name 起始。 */
                a->ptr_offset = htons((uint16_t)(0xC000 | question_name_offset));
                a->type = htons(1);
                a->qclass = htons(1);
                a->ttl = htonl(300);
                a->addr_len = htons(4);
                a->ip_addr = ap_ip;
                off += (int)sizeof(dns_answer_t);
                answers++;
            }
        }
        h->an_count = htons(answers);
        sendto(sock, reply, (size_t)off, 0, (struct sockaddr *)&src, sl);
    }
    close(sock);
    vTaskDelete(NULL);
}

static void start_dns(void)
{
    if (s_dns_running) return;
    s_dns_running = true;
    xTaskCreate(dns_task, "wifi_dns", 3072, nullptr, 5, &s_dns_task);
}

static void stop_dns(void)
{
    s_dns_running = false;
    if (s_dns_sock >= 0) {
        close(s_dns_sock);       /* 使 recvfrom 立即返回并退出任务 */
        s_dns_sock = -1;
    }
    if (s_dns_task) { s_dns_task = nullptr; }
}

/* ===================================================================
 *  公共 API
 * =================================================================== */
void WifiConfigStartPortal(void)
{
    if (s_portal_running) return;
    s_portal_running = true;
    if (!start_softap()) {
        s_portal_running = false;
        ESP_LOGE(TAG, "Config portal failed to start");
        return;
    }
    start_dns();
    ESP_LOGI(TAG, "Config portal running: connect to Dino-XXXX, browse http://192.168.4.1");
}

void WifiConfigStopPortal(void)
{
    if (!s_portal_running) return;
    s_portal_running = false;
    stop_dns();
    stop_softap();

    /* 用刚保存的凭据重连 STA */
    char ssid[33] = {}, pass[65] = {};
    if (!WifiConfigGetCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
#ifdef WIFI_STA_SSID
        strncpy(ssid, WIFI_STA_SSID, sizeof(ssid) - 1);
#endif
#ifdef WIFI_STA_PASSWORD
        strncpy(pass, WIFI_STA_PASSWORD, sizeof(pass) - 1);
#endif
    }
    WifiReconnectSta(ssid, pass);
}

bool WifiConfigPortalRunning(void)
{
    return s_portal_running;
}

void WifiConfigEnterFromWeb(void)
{
    WifiConfigStartPortal();
}

int WifiConfigScanAps(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 100;
    scan.scan_time.active.max = 300;

    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        snprintf(out, out_sz, "[]");
        return (int)strlen(out);
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 16) count = 16;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(count, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_scan_get_ap_records(&count, nullptr); snprintf(out, out_sz, "[]"); return (int)strlen(out); }

    esp_err_t err = esp_wifi_scan_get_ap_records(&count, recs);
    int used = 0;
    if (err == ESP_OK) {
        used = snprintf(out, out_sz, "[");
        for (uint16_t i = 0; i < count && used < (int)out_sz - 64; i++) {
            int a = used;
            used += snprintf(out + used, out_sz - (size_t)used,
                             "%s{\"ssid\":\"%.32s\",\"rssi\":%d,\"auth\":%d}",
                             i ? "," : "", recs[i].ssid, recs[i].rssi, recs[i].authmode);
            if (used >= (int)out_sz - 1) { used = a; break; }
        }
        used += snprintf(out + used, out_sz - (size_t)used, "]");
    } else {
        used = snprintf(out, out_sz, "[]");
    }
    free(recs);
    return used;
}
