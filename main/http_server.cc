/*
 * http_server.cc — Web 面板与本地 API 实现
 *
 * 路由：/ 控制面板页（内嵌 HTML）；/api/servo 舵机滑条、/api/action 动作、
 * /api/autoplay 开关、/api/battery 电量、/api/wifi* 配网扫描/连接、
 * /api/chat* 对话开关。404 全部 302 回首页（配合 DNS 劫持 captive portal）。
 * 对话进行中拒绝动作/滑条请求（舵机归 chat_motion 管）。
 * Flash 管理页与上传 API 在 flash_upload_server.cc。
 */
#include "http_server.h"
#include "audio.h"
#include "auto_run.h"
#include "chat.h"
#include "config.h"
#include "flash_audio.h"
#include "flash_upload_server.h"
#include "power.h"
#include "servo.h"
#include "wifi.h"
#include "wifi_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "dino_http";

httpd_handle_t g_http_server = nullptr;

httpd_handle_t HttpServerHandle(void) { return g_http_server; }

// ======================== Embedded Web UI ========================
// 面板 HTML 单独存放在 web_assets/panel.html，通过 EMBED_FILES 链入固件
//（改网页只需改 html 文件，不再改 C 代码）。链接期符号见 CMakeLists.txt。
extern const unsigned char panel_html_start[] asm("_binary_panel_html_start");
extern const unsigned char panel_html_end[]   asm("_binary_panel_html_end");
#define kHtml ((const char *)panel_html_start)
#define kHtmlLen ((size_t)(panel_html_end - panel_html_start))

// ======================== HTTP Handlers ========================
static esp_err_t HandleRoot(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, kHtml, kHtmlLen);
  return ESP_OK;
}

static esp_err_t HandleServo(httpd_req_t *req)
{
  // 聊天进行中时 chat_motion 任务持有舵机（情绪驱动）——这里复活写舵机会打架
  if (ChatIsActive()) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\"}");
    return ESP_OK;
  }

  char buf[512] = {};
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  buf[ret] = 0;

  int angles[5] = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                   SERVO_HEAD_TURN_DEFAULT, SERVO_TAIL_UD_DEFAULT,
                   SERVO_TAIL_LR_DEFAULT};
  cJSON *root = cJSON_Parse(buf);
  if (root) {
    const cJSON *arr = cJSON_GetObjectItem(root, "angles");
    if (cJSON_IsArray(arr)) {
      for (int i = 0; i < 5 && i < cJSON_GetArraySize(arr); i++)
        angles[i] = cJSON_GetArrayItem(arr, i)->valueint;
    }
    cJSON_Delete(root);
  }
  for (int i = 0; i < 5; i++)
    SetServoAngle(i, angles[i]);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

static esp_err_t HandleBattery(httpd_req_t *req)
{
  char resp[128];
  snprintf(resp, sizeof(resp), "{\"voltage_mv\":%d,\"level\":%d}",
           GetBatteryVoltageMv(), GetBatteryLevel());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

#if ENABLE_AUTO_RUN
static esp_err_t HandleAction(httpd_req_t *req)
{
    // 同 HandleServo：聊天中不允许手动触发动作，避免与 chat_motion 抢舵机
    if (ChatIsActive()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\"}");
        return ESP_OK;
    }

    char buf[96] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[received] = 0;
    int action = -1;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
      action = cJSON_GetObjectItem(root, "action")->valueint;
      cJSON_Delete(root);
    }
    bool ok = action >= 0 && action < DINO_ACTION_COUNT;
    if (ok) {
        SetAutoRunRunning(true);
        ok = TriggerDinoActionWithAutoSound(static_cast<DinoAction>(action));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t HandleAutoPlay(httpd_req_t *req)
{
    // POST 会复活 auto_run 任务（SetAutoRunRunning(true)）——聊天中拒绝；
    // GET 只读状态，放行
    if (req->method == HTTP_POST && ChatIsActive()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\",\"autoplay\":false}");
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        char buf[64] = {};
        httpd_req_recv(req, buf, sizeof(buf) - 1);
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *enable = cJSON_GetObjectItem(root, "enable");
            const cJSON *hard = cJSON_GetObjectItem(root, "hard_swing");
            if (enable) SetAutoRunRunning(cJSON_IsTrue(enable));
            else if (!hard) SetAutoRunRunning(!IsAutoRunRunning());
            if (hard) SetAutoRunHardSwing(cJSON_IsTrue(hard));
            cJSON_Delete(root);
        }
    }
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"autoplay\":%s,\"hard_swing\":%s}",
             IsAutoRunRunning() ? "true" : "false",
             IsAutoRunHardSwing() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
#endif

static esp_err_t HandleWifiStatus(httpd_req_t *req)
{
    char buf[320];
    bool online = (strcmp(WiFiIP(), "0.0.0.0") != 0);
    bool portal = WifiConfigPortalRunning();

    char cur_ssid[33] = {};
    int rssi = -1;
    if (online) WifiConnectedApInfo(cur_ssid, sizeof(cur_ssid), &rssi);

    char saved_ssid[33] = {};
    {
        char p[65] = {};
        if (WifiConfigGetCredentials(saved_ssid, sizeof(saved_ssid), p, sizeof(p)) && saved_ssid[0]) {
            /* NVS 已保存的实际凭据 */
        }
#ifdef WIFI_STA_SSID
        else strlcpy(saved_ssid, WIFI_STA_SSID, sizeof(saved_ssid));
#else
        else strlcpy(saved_ssid, "none", sizeof(saved_ssid));
#endif
    }

    const char *mode = online ? "connected" : (portal ? "portal" : "offline");
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"mode\":\"%s\",\"ssid\":\"%s\",\"saved_ssid\":\"%s\","
             "\"ip\":\"%s\",\"rssi\":%d,\"portal\":%s}",
             online ? "true" : "false", mode, cur_ssid, saved_ssid,
             WiFiIP(), rssi, portal ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiPortal(httpd_req_t *req)
{
    httpd_req_recv(req, nullptr, 0); /* discard body */
    WifiConfigEnterFromWeb();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "portal started");
    return ESP_OK;
}

static esp_err_t HandleWifiScan(httpd_req_t *req)
{
    char buf[1200];
    WifiConfigScanAps(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiConfigure(httpd_req_t *req)
{
    char body[512] = {};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = 0;

    char ssid[33] = {}, pass[65] = {};
    cJSON *root = cJSON_Parse(body);
    if (root) {
        const cJSON *s = cJSON_GetObjectItem(root, "ssid");
        const cJSON *p = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(s)) {
            strncpy(ssid, s->valuestring, sizeof(ssid) - 1);
        }
        if (cJSON_IsString(p)) {
            strncpy(pass, p->valuestring, sizeof(pass) - 1);
        }
        cJSON_Delete(root);
    }
    if (!ssid[0]) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "no ssid"); return ESP_OK; }

    WifiConfigSaveCredentials(ssid, pass);

    /* 先回响应, 让手机收到"已保存"再关热点, 避免页面加载中断乱跳 */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");

    WifiConfigStopPortal();  /* 保存后切回 STA 连接 */
    return ESP_OK;
}

static esp_err_t HandleChatStatus(httpd_req_t *req)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"active\":%s,\"ready\":%s}",
             ChatIsActive() ? "true" : "false",
             ChatIsReady() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleChatToggle(httpd_req_t *req)
{
    httpd_req_recv(req, nullptr, 0); /* discard body */
    ChatToggle();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

void StartHttpServer()
{
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 20;
  httpd_start(&g_http_server, &cfg);

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &root);

  httpd_uri_t servo = {.uri = "/api/servo", .method = HTTP_POST, .handler = HandleServo, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &servo);

  httpd_uri_t batt = {.uri = "/api/battery", .method = HTTP_GET, .handler = HandleBattery, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &batt);

  httpd_uri_t wifi_status = {.uri = "/api/wifi", .method = HTTP_GET, .handler = HandleWifiStatus, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_status);
  httpd_uri_t wifi_portal = {.uri = "/api/wifi/portal", .method = HTTP_POST, .handler = HandleWifiPortal, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_portal);
  httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = HandleWifiScan, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_scan);
  httpd_uri_t wifi_cfg = {.uri = "/api/wifi/configure", .method = HTTP_POST, .handler = HandleWifiConfigure, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_cfg);

  httpd_uri_t chat_status = {.uri = "/api/chat", .method = HTTP_GET, .handler = HandleChatStatus, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &chat_status);
  httpd_uri_t chat_toggle = {.uri = "/api/chat/toggle", .method = HTTP_POST, .handler = HandleChatToggle, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &chat_toggle);

  /* 全局 404 → 重定向到首页: 配合 DNS 劫持实现 captive portal,
   * 手机连上 Dino-XXXX 后系统探测任何 URL 都会落到首页, 触发"登录网络"弹窗 */
  httpd_register_err_handler(g_http_server, HTTPD_404_NOT_FOUND, [](httpd_req_t *req, httpd_err_code_t err) -> esp_err_t {
      httpd_resp_set_status(req, "302 Found");
      httpd_resp_set_hdr(req, "Location", "/");
      httpd_resp_sendstr(req, "<html><body>redirecting...</body></html>");
      return ESP_OK;
  });

#if ENABLE_AUTO_RUN
  httpd_uri_t action = {.uri = "/api/action", .method = HTTP_POST, .handler = HandleAction, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &action);

  httpd_uri_t auto_play = {.uri = "/api/autoplay", .method = HTTP_GET, .handler = HandleAutoPlay, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &auto_play);
  {
    httpd_uri_t post_auto = {.uri = "/api/autoplay", .method = HTTP_POST, .handler = HandleAutoPlay, .user_ctx = nullptr};
    httpd_register_uri_handler(g_http_server, &post_auto);
  }
#endif

  // Register flash upload endpoints
  flash_upload_server_register();

  ESP_LOGI(TAG, "HTTP server started");
}
