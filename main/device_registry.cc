#include "device_registry.h"
#include "config.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kNamespace[] = "audio_hub";
constexpr char kApiTokenKey[] = "api_token";
constexpr char kClaimTokenKey[] = "claim_token";
constexpr char kActivationCodeKey[] = "act_code";
constexpr size_t kTokenSize = 96;
constexpr int kHeartbeatSeconds = 60;
static const char *TAG = "device_registry";
TaskHandle_t g_task = nullptr;

struct Response {
    char *data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
};

esp_err_t response_handler(esp_http_client_event_t *event)
{
    auto *response = static_cast<Response *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    const size_t required = response->length + event->data_len + 1;
    if (required > response->capacity) {
        size_t capacity = response->capacity ? response->capacity * 2 : 1024;
        while (capacity < required) capacity *= 2;
        if (capacity > 8192) return ESP_ERR_NO_MEM;
        auto *next = static_cast<char *>(realloc(response->data, capacity));
        if (!next) return ESP_ERR_NO_MEM;
        response->data = next;
        response->capacity = capacity;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

bool post_json(const char *path, const char *body, const char *api_token,
               Response *response, int *status)
{
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             SYNC_SERVER_IP, SYNC_SERVER_PORT, path);
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.event_handler = response_handler;
    config.user_data = response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (api_token && api_token[0]) {
        char authorization[128];
        snprintf(authorization, sizeof(authorization), "Bearer %s", api_token);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    esp_http_client_set_post_field(client, body, strlen(body));
    const esp_err_t result = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return result == ESP_OK && *status >= 200 && *status < 300;
}

bool load_secret(const char *key, char *output, size_t output_size)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t required = output_size;
    const esp_err_t result = nvs_get_str(handle, key, output, &required);
    nvs_close(handle);
    if (result != ESP_OK) {
        output[0] = '\0';
        return false;
    }
    return true;
}

bool save_secret(const char *key, const char *value)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set_result = nvs_set_str(handle, key, value);
    const esp_err_t commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
    nvs_close(handle);
    return commit_result == ESP_OK;
}

void erase_secret(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
}

void make_device_id(char *output, size_t output_size)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(output, output_size, "ZYLZ-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *json_string(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool register_device(const char *device_id, char *claim_token, char *activation_code)
{
    char request_body[256];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(request_body, sizeof(request_body),
             "{\"device_id\":\"%s\",\"product_id\":\"%s\","
             "\"firmware_version\":\"%s\"}",
             device_id, SYNC_PRODUCT_ID, app->version);
    Response response;
    int status = 0;
    const bool ok = post_json("/api/device/register", request_body, nullptr,
                              &response, &status);
    if (!ok) {
        ESP_LOGW(TAG, "注册失败: HTTP %d", status);
        free(response.data);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(response.data, response.length);
    const char *claim = root ? json_string(root, "claim_token") : nullptr;
    const char *code = root ? json_string(root, "activation_code") : nullptr;
    if (!claim || !code) {
        ESP_LOGW(TAG, "设备已登记但本机没有凭据，请在后台删除后重新注册");
        cJSON_Delete(root);
        free(response.data);
        return false;
    }
    strlcpy(claim_token, claim, kTokenSize);
    strlcpy(activation_code, code, 8);
    const bool saved = save_secret(kClaimTokenKey, claim_token)
                       && save_secret(kActivationCodeKey, activation_code);
    cJSON_Delete(root);
    free(response.data);
    if (!saved) {
        ESP_LOGE(TAG, "Claim Token 写入 NVS 失败");
        claim_token[0] = '\0';
        return false;
    }
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  Audio Hub 设备激活码: %s", activation_code);
    ESP_LOGW(TAG, "  请登录管理后台绑定此设备");
    ESP_LOGW(TAG, "========================================");
    return true;
}

bool poll_activation(const char *device_id, const char *claim_token, char *api_token,
                     int *http_status)
{
    char request_body[256];
    snprintf(request_body, sizeof(request_body),
             "{\"device_id\":\"%s\",\"claim_token\":\"%s\"}",
             device_id, claim_token);
    Response response;
    int status = 0;
    const bool ok = post_json("/api/device/activate", request_body, nullptr,
                              &response, &status);
    *http_status = status;
    if (!ok) {
        if (status != 0) ESP_LOGW(TAG, "激活查询失败: HTTP %d", status);
        free(response.data);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(response.data, response.length);
    const char *token = root ? json_string(root, "api_token") : nullptr;
    if (!token) {
        cJSON_Delete(root);
        free(response.data);
        return false;
    }
    strlcpy(api_token, token, kTokenSize);
    const bool saved = save_secret(kApiTokenKey, api_token);
    cJSON_Delete(root);
    free(response.data);
    if (!saved) {
        ESP_LOGE(TAG, "设备令牌写入 NVS 失败，将保留 Claim Token");
        api_token[0] = '\0';
        return false;
    }
    erase_secret(kClaimTokenKey);
    erase_secret(kActivationCodeKey);
    ESP_LOGI(TAG, "设备已成功绑定到 Audio Hub");
    return true;
}

void check_in(const char *api_token)
{
    char request_body[128];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(request_body, sizeof(request_body),
             "{\"firmware_version\":\"%s\"}", app->version);
    Response response;
    int status = 0;
    if (post_json("/api/device/v1/check-in", request_body, api_token,
                  &response, &status)) {
        ESP_LOGI(TAG, "心跳上报成功");
    } else if (status == 403) {
        ESP_LOGW(TAG, "设备已被后台停用");
    } else if (status == 401) {
        ESP_LOGE(TAG, "设备令牌已失效，请在后台重新生成并重新配置");
    } else {
        ESP_LOGW(TAG, "心跳上报失败: HTTP %d", status);
    }
    free(response.data);
}

void registry_task(void *)
{
    char device_id[32] = {};
    char api_token[kTokenSize] = {};
    char claim_token[kTokenSize] = {};
    char activation_code[8] = {};
    make_device_id(device_id, sizeof(device_id));
    load_secret(kApiTokenKey, api_token, sizeof(api_token));
    load_secret(kClaimTokenKey, claim_token, sizeof(claim_token));
    load_secret(kActivationCodeKey, activation_code, sizeof(activation_code));
    ESP_LOGI(TAG, "设备 ID: %s, 产品: %s", device_id, SYNC_PRODUCT_ID);

    while (!api_token[0]) {
        if (!claim_token[0]) {
            if (!register_device(device_id, claim_token, activation_code)) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        } else if (activation_code[0]) {
            ESP_LOGW(TAG, "待绑定激活码: %s", activation_code);
        }
        int activation_status = 0;
        if (!poll_activation(device_id, claim_token, api_token, &activation_status)) {
            if (activation_status == 401 || activation_status == 410) {
                ESP_LOGW(TAG, "激活凭据失效，重新发起注册");
                erase_secret(kClaimTokenKey);
                erase_secret(kActivationCodeKey);
                claim_token[0] = '\0';
                activation_code[0] = '\0';
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    while (true) {
        check_in(api_token);
        vTaskDelay(pdMS_TO_TICKS(kHeartbeatSeconds * 1000));
    }
}

}  // namespace

void device_registry_start(void)
{
    if (g_task) return;
    if (xTaskCreate(registry_task, "device_registry", 6144, nullptr, 4, &g_task)
        != pdPASS) {
        g_task = nullptr;
        ESP_LOGE(TAG, "无法创建设备注册任务");
    }
}
