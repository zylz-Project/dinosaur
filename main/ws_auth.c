/*
 * ws_auth.c — 鉴权: 登录换取会话 cookie, 供 WebSocket 握手使用
 *
 * 实测(2026-08-11): 服务端实时对话接口改为 cookie + Origin 鉴权:
 *   - /auth/login 响应 Set-Cookie: mem_dialog_session=<v> (30天有效)
 *   - WS 握手必须带 "Cookie: mem_dialog_session=<v>" 和 "Origin: ..."
 *   - 只带 Bearer token 会被服务端 CLOSE(1008) 或回 AUTH_REQUIRED
 * cookie 只存内存, 重启后重新登录获取。
 */

#include "ws_auth.h"
#include "chat_cert.h"

#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

static const char *TAG = "WS_AUTH";

/* 缓存的会话 cookie: "mem_dialog_session=<value>" */
static char g_auth_cookie[WS_AUTH_COOKIE_MAX_LEN] = "";

void ws_auth_invalidate_cookie(void)
{
    g_auth_cookie[0] = '\0';
    ESP_LOGI(TAG, "Cached session cookie invalidated");
}

/* HTTP_EVENT_ON_HEADER 回调中捕获 Set-Cookie 的值 */
static esp_err_t auth_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->header_key || !evt->header_value)
        return ESP_OK;
    if (strncasecmp(evt->header_key, "Set-Cookie", 10) != 0)
        return ESP_OK;

    /* 只取 "mem_dialog_session=<value>; ..." 的第一段 */
    const char *v = evt->header_value;
    if (strncmp(v, "mem_dialog_session=", 19) != 0)
        return ESP_OK;
    v += 19;
    size_t len = 0;
    while (v[len] != '\0' && v[len] != ';' && len < WS_AUTH_COOKIE_MAX_LEN - 21)
        len++;
    if (len == 0)
        return ESP_OK;

    snprintf(g_auth_cookie, sizeof(g_auth_cookie),
             "mem_dialog_session=%.*s", (int)len, v);
    ESP_LOGI(TAG, "Session cookie captured (%d chars)", (int)len);
    return ESP_OK;
}

int ws_auth_get_cookie(char *cookie_out, int cookie_size)
{
    if (cookie_out && cookie_size > 0)
        cookie_out[0] = '\0';

    /* 已登录 — 复用缓存 */
    if (g_auth_cookie[0] != '\0')
    {
        if (cookie_out && cookie_size > 0)
            snprintf(cookie_out, (size_t)cookie_size, "%s", g_auth_cookie);
        return 0;
    }

    /* ---- 拼 JSON 请求体 ---- */
    int body_len = snprintf(NULL, 0,
                            "{\"username\":\"%s\",\"password\":\"%s\"}",
                            AUTH_USERNAME, AUTH_PASSWORD);
    char *body = (char *)malloc((size_t)body_len + 1);
    if (!body)
    {
        ESP_LOGE(TAG, "Failed to allocate auth body");
        return -1;
    }
    snprintf(body, (size_t)body_len + 1,
             "{\"username\":\"%s\",\"password\":\"%s\"}",
             AUTH_USERNAME, AUTH_PASSWORD);

    ESP_LOGI(TAG, "TLS heap before auth: internal=%lu, largest=%lu, psram=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- POST 登录接口, 捕获 Set-Cookie ---- */
    esp_http_client_config_t config = {
        .url = AUTH_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .keep_alive_enable = false,
        .crt_bundle_attach = NULL,
        .cert_pem = CHAT_TLS_CA_PEM,
        .event_handler = auth_event_handler,
        .user_data = NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to init HTTP client for auth");
        free(body);
        return -1;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    ESP_LOGI(TAG, "POST auth %s", AUTH_API_URL);

    /* 用 open/write/fetch_headers/read 而非 perform():
       perform() 会自己消费响应体, 拿不到 Set-Cookie 头 */
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Auth open failed: %s (err=0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "TLS heap after failure: internal=%lu, largest=%lu, psram=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_http_client_cleanup(client);
        free(body);
        return -1;
    }

    int w = esp_http_client_write(client, body, body_len);
    free(body);
    if (w < 0)
    {
        ESP_LOGE(TAG, "Auth write failed: %d", w);
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Auth HTTP %d, content-length=%d", status, content_len);

    /* 读掉响应体 (事件回调已在期间捕获 cookie) */
    int ret = -1;
    if (status == 200 || status == 201)
    {
        char resp_buf[256];
        int total_read = 0;
        while (total_read < (int)sizeof(resp_buf) - 1)
        {
            int r = esp_http_client_read(client,
                                         resp_buf + total_read,
                                         (int)sizeof(resp_buf) - 1 - total_read);
            if (r <= 0)
                break;
            total_read += r;
            if (content_len > 0 && total_read >= content_len)
                break;
        }
        if (g_auth_cookie[0] != '\0')
        {
            if (cookie_out && cookie_size > 0)
                snprintf(cookie_out, (size_t)cookie_size, "%s", g_auth_cookie);
            ret = 0;
        }
        else
        {
            ESP_LOGE(TAG, "Login OK but no mem_dialog_session cookie");
        }
    }
    else
    {
        ESP_LOGE(TAG, "Auth failed, HTTP %d", status);
    }

    esp_http_client_cleanup(client);
    return ret;
}
