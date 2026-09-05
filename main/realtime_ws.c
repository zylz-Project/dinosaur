/*
 * realtime_ws.c — WebSocket 实时音频通道（连接层，见 realtime_ws.h 协议说明）
 *
 * 本文件负责连接生命周期：初始化 esp_websocket_client（带 cookie/Origin/
 * 证书配置）、连接任务（先等时间同步→握手→发 hello）、断线重连、断开清理、
 * 会话缓冲清理（ws_clear_session_buffers）与事件回调。
 * 收包后的 JSON/base64 解析在 realtime_ws_protocol.c，TTS 流控与发送在
 * realtime_ws_tts.c —— 三者共享的内部声明见 realtime_ws_internal.h。
 * 收到的消息经 g_msg_queue 交给 ws_proc_task 分发。
 */

#include "realtime_ws.h"
#include "realtime_ws_internal.h"
#include "display.h"
#include "ws_auth.h"
#include "chat_cert.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static const char *TAG = "REALTIME_WS";

/* ===================================================================
 *  State
 * =================================================================== */
esp_websocket_client_handle_t ws_client = NULL;
static char *ws_url = NULL;
/* 连接前门控（time_sync 等待），由 chat.cc 通过 realtime_ws_init 传入。 */
static bool (*g_time_sync_wait)(int timeout_ms) = NULL;
volatile bool ws_connected = false;
volatile bool g_ready = false;
volatile TickType_t g_last_rx_tick = 0;  /* 最后收到数据的时间 */
static SemaphoreHandle_t ws_connect_sem = NULL;
static SemaphoreHandle_t g_conn_mutex = NULL;
static bool ws_connecting = false;
static bool ws_want_connected = false;
static bool ws_reconnect_pending = false;
static TickType_t ws_connected_tick = 0;
static void ws_reconnect_task(void *arg);
/* Message queue (event handler → processing task) */
QueueHandle_t g_msg_queue = NULL;
static bool g_msg_queue_with_caps = false;
static uint32_t g_msg_queue_drop_count = 0;
/* esp_websocket_client may emit one payload in several DATA events. */
static uint8_t *g_rx_payload = NULL;
static int g_rx_payload_len = 0;
static int g_rx_payload_received = 0;
static bool g_rx_payload_binary = false;
bool realtime_ws_is_ready(void) { return g_ready; }
int realtime_ws_ms_since_last_rx(void) {
    if (g_last_rx_tick == 0) return 0;
    return (int)((xTaskGetTickCount() - g_last_rx_tick) * portTICK_PERIOD_MS);
}
/* destroy() 之前会把 ws_client 置空以屏蔽迟到的旧 client 事件，因此不能
 * 只依赖 DISCONNECTED 回调做清理。停止/重连都显式调用这一份清理逻辑。 */
static void ws_clear_session_buffers(void)
{
    if (g_rx_payload) { free(g_rx_payload); g_rx_payload = NULL; }
    g_rx_payload_len = 0;
    g_rx_payload_received = 0;
    if (g_tts_buf) { free(g_tts_buf); g_tts_buf = NULL; }
    g_tts_len = 0;
    g_tts_active = false;
    if (g_json_buf) { free(g_json_buf); g_json_buf = NULL; }
    g_json_len = 0;
    if (g_llm_buf) { free(g_llm_buf); g_llm_buf = NULL; }

    if (g_msg_queue) {
        ws_msg_t msg;
        while (xQueueReceive(g_msg_queue, &msg, 0) == pdTRUE) free(msg.data);
    }
    if (g_tts_queue) {
        tts_audio_item_t item;
        while (tts_queue_receive(&item)) free(item.data);
    }
    if (g_tx_queue) {
        ws_tx_msg_t msg;
        while (xQueueReceive(g_tx_queue, &msg, 0) == pdTRUE) free(msg.json);
    }

    portENTER_CRITICAL(&g_tts_flow_mux);
    memset(g_tts_flows, 0, sizeof(g_tts_flows));
    g_tts_cur_flow = -1;
    g_tts_queued_bytes = 0;
    portEXIT_CRITICAL(&g_tts_flow_mux);
}
/* ===================================================================
 *  Event handler
 * =================================================================== */
static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    /* destroy() may leave already queued events behind; never let an old client
     * change the state of a newer connection (or a user-requested disconnect). */
    if (!data || data->client != ws_client) return;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "CONNECTED"); ws_connected = true; g_ready = false;
        ws_connected_tick = xTaskGetTickCount();
        g_ctrl_tx_ok = 0; g_ctrl_tx_failed = 0;
        g_buffer_tx_ok = 0; g_buffer_tx_failed = 0;
        display_set_ws(true, false);
        realtime_ws_send_hello();
        xSemaphoreGive(ws_connect_sem); break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        {
            tts_flow_state_t flow_snapshot[TTS_FLOW_SLOTS];
            size_t queued_bytes;
            int tx_queue_depth = g_tx_queue ? (int)uxQueueMessagesWaiting(g_tx_queue) : 0;
            portENTER_CRITICAL(&g_tts_flow_mux);
            memcpy(flow_snapshot, g_tts_flows, sizeof(flow_snapshot));
            queued_bytes = g_tts_queued_bytes;
            portEXIT_CRITICAL(&g_tts_flow_mux);
            ESP_LOGW(TAG,
                     "DISCONNECTED: connected_for=%dms, audio_queue=%dms, ctrl_queue=%d, "
                     "ctrl_tx=%lu/%lu, buffer_tx=%lu/%lu",
                     ws_connected_tick
                         ? (int)((xTaskGetTickCount() - ws_connected_tick) * portTICK_PERIOD_MS)
                         : 0,
                     tts_queued_ms(queued_bytes), tx_queue_depth,
                     (unsigned long)g_ctrl_tx_ok, (unsigned long)g_ctrl_tx_failed,
                     (unsigned long)g_buffer_tx_ok, (unsigned long)g_buffer_tx_failed);
            for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
                tts_flow_state_t *f = &flow_snapshot[i];
                if (!f->valid || f->playback_finished) continue;
                ESP_LOGW(TAG,
                         "  flow[%d] seq=%d end=%d start=%d finish=%d "
                         "dirty=%d pending=%dms",
                         i, f->seq, f->stream_ended, f->playback_started,
                         f->playback_finished, f->buffer_dirty,
                         f->pending_queued_ms);
            }
        }
        ws_connected = false; g_ready = false; g_last_rx_tick = 0;
        ws_connected_tick = 0;
        display_set_ws(false, false);
        ws_clear_session_buffers();
        /* esp_websocket_client 不能在自己的事件回调中销毁。关闭组件内部
         * 自动重连后交给独立任务重建，确保重新登录并建立全新的TLS连接。 */
        if (g_conn_mutex) {
            bool schedule = false;
            xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
            if (ws_want_connected && !ws_reconnect_pending) {
                ws_reconnect_pending = true;
                schedule = true;
            }
            xSemaphoreGive(g_conn_mutex);
            if (schedule &&
                xTaskCreate(ws_reconnect_task, "ws_reconnect", 4096,
                            data->client, 3, NULL) != pdPASS) {
                xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
                ws_reconnect_pending = false;
                xSemaphoreGive(g_conn_mutex);
                ESP_LOGE(TAG, "Failed to create WS reconnect task");
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG,
                 "WS err: type=%d tls_esp=0x%x tls_stack=0x%x errno=%d http=%d",
                 data->error_handle.error_type,
                 data->error_handle.esp_tls_last_esp_err,
                 data->error_handle.esp_tls_stack_err,
                 data->error_handle.esp_transport_sock_errno,
                 data->error_handle.esp_ws_handshake_status_code);
        ws_connected = false; break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WS close frame: status=%d", data->close_status_code);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!data->data_ptr || data->data_len <= 0) break;
        if (data->op_code != 0x01 && data->op_code != 0x02) break;
        {
            int payload_len = data->payload_len > 0 ? data->payload_len : data->data_len;
            int offset = data->payload_offset;
            bool is_binary = (data->op_code == 0x02);

            if (offset == 0) {
                if (g_rx_payload) free(g_rx_payload);
                g_rx_payload = heap_caps_malloc((size_t)payload_len + 1, MALLOC_CAP_SPIRAM);
                if (!g_rx_payload) g_rx_payload = malloc((size_t)payload_len + 1);
                g_rx_payload_len = payload_len;
                g_rx_payload_received = 0;
                g_rx_payload_binary = is_binary;
            }

            if (!g_rx_payload || payload_len != g_rx_payload_len ||
                is_binary != g_rx_payload_binary || offset < 0 ||
                offset + data->data_len > g_rx_payload_len) {
                ESP_LOGW(TAG, "Invalid WS fragment: off=%d len=%d total=%d",
                         offset, data->data_len, payload_len);
                if (g_rx_payload) { free(g_rx_payload); g_rx_payload = NULL; }
                g_rx_payload_len = 0; g_rx_payload_received = 0;
                break;
            }

            memcpy(g_rx_payload + offset, data->data_ptr, data->data_len);
            int end = offset + data->data_len;
            if (end > g_rx_payload_received) g_rx_payload_received = end;

            if (g_rx_payload_received >= g_rx_payload_len) {
                g_rx_payload[g_rx_payload_len] = '\0';
                ws_msg_t msg = {
                    .data = g_rx_payload,
                    .len = g_rx_payload_len,
                    .is_binary = g_rx_payload_binary,
                };
                g_rx_payload = NULL;
                g_rx_payload_len = 0;
                g_rx_payload_received = 0;
                if (xQueueSend(g_msg_queue, &msg, 0) != pdTRUE) {
                    g_msg_queue_drop_count++;
                    ESP_LOGE(TAG, "WS message queue full: dropped=%lu, payload=%d bytes",
                             (unsigned long)g_msg_queue_drop_count, msg.len);
                    free(msg.data);
                }
            }
        }
        break;
    default: break;
    }
}
/* ===================================================================
 *  Connection management
 * =================================================================== */
int realtime_ws_init(const char *url, const realtime_ws_hooks_t *hooks) {
    if (!url) return -1;
    g_time_sync_wait = hooks ? hooks->time_sync_wait : NULL;
    g_resp_mutex = xSemaphoreCreateMutex();
    g_conn_mutex = xSemaphoreCreateMutex();
    g_ws_tx_mutex = xSemaphoreCreateMutex();
    ws_connect_sem = xSemaphoreCreateBinary();
    /* 普通 xQueueCreate() 在 ESP-IDF 中强制使用内部 RAM。两个大队列合计
     * 约 35KB，会挤掉 mbedTLS 握手需要的连续内部内存。显式放入 PSRAM；
     * PSRAM 不可用时退回原来的小队列，仍保证设备能够连接。 */
    g_msg_queue = xQueueCreateWithCaps(WS_MSG_QUEUE_LEN, sizeof(ws_msg_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_msg_queue) {
        g_msg_queue_with_caps = true;
    } else {
        ESP_LOGW(TAG, "PSRAM WS queue allocation failed, falling back to 256 entries");
        g_msg_queue = xQueueCreate(256, sizeof(ws_msg_t));
    }
    g_tts_queue = xQueueCreateWithCaps(TTS_QUEUE_LEN, sizeof(tts_audio_item_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_tts_queue) {
        g_tts_queue_with_caps = true;
    } else {
        ESP_LOGW(TAG, "PSRAM TTS queue allocation failed, falling back to 256 entries");
        g_tts_queue = xQueueCreate(256, sizeof(tts_audio_item_t));
    }
    g_tx_queue = xQueueCreate(WS_TX_QUEUE_LEN, sizeof(ws_tx_msg_t));
    if (!g_resp_mutex || !g_conn_mutex || !g_ws_tx_mutex || !ws_connect_sem ||
        !g_msg_queue || !g_tts_queue || !g_tx_queue) return -1;
    ws_url = strdup(url);
    /* 整机还运行舵机、Web和电源任务。TTS到达后优先完成JSON/base64解码，
     * 避免低优先级接收任务被挤成突发；播放阶段麦克风上传已经暂停。 */
    xTaskCreate(ws_proc_task, "ws_proc", 8192, NULL, 6, NULL);
    /* 播放水位/finished反馈也要及时送出，否则服务端会暂停后续TTS窗口。 */
    xTaskCreate(ws_tx_task, "ws_tx", 8192, NULL, 5, &g_ws_tx_task_handle);
    return ws_url ? 0 : -1;
}
/* 鉴权+连接独立任务: esp_http_client 的 TLS+JSON 吃栈, 不能在 main 任务里跑 */
static void ws_connect_task(void *arg)
{
    /* TLS 证书校验依赖系统时间。time_sync_wait 由调用方注入
     * （chat.cc 传 WiFiWaitForTimeSync）；NULL = 跳过等待直接连。 */
    if (g_time_sync_wait && !g_time_sync_wait(10000)) {
        ESP_LOGW(TAG, "TLS connect deferred until system time is synchronized");
        goto done;
    }

    /* 鉴权: 登录换取会话 cookie, 拼进 WS 握手头 (cookie + Origin 缺一不可) */
    char cookie[WS_AUTH_COOKIE_MAX_LEN] = "";
    char headers[WS_AUTH_COOKIE_MAX_LEN + 64] = "";
    for (int attempt = 0; attempt < 3; attempt++) {
        xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
        bool wanted = ws_want_connected;
        xSemaphoreGive(g_conn_mutex);
        if (!wanted) goto done;

        if (ws_auth_get_cookie(cookie, sizeof(cookie)) == 0) break;
        ESP_LOGW(TAG, "Auth failed, retry %d/3...", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (cookie[0] == '\0') {
        ESP_LOGE(TAG, "Authentication failed; skip unauthenticated WS connection");
        goto done;
    }

    snprintf(headers, sizeof(headers),
             "Origin: https://www.mmemoryy.xyz\r\nCookie: %s\r\n", cookie);
    ESP_LOGI(TAG, "WS handshake with cookie+origin auth");

    esp_websocket_client_config_t c = {
        .uri = ws_url, .task_stack = 6144, .task_prio = 4,
        .buffer_size = 16384, .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 10000, .disable_auto_reconnect = true,
        .keep_alive_enable = true, .keep_alive_idle = 15,
        .keep_alive_interval = 3, .keep_alive_count = 3,
        /* 服务端不回 PONG: 若启用 WS PING/PONG 会在数据仍在收时误断连。
         * 注意: ping_interval_sec=0 会被组件替换成默认间隔, 并不会真正禁用;
         * 这里用 60s 拉长间隔, 并靠 disable_pingpong_discon 避免缺 PONG 断连。 */
        .ping_interval_sec = 60, .pingpong_timeout_sec = 20,
        .disable_pingpong_discon = true,
        .headers = headers[0] != '\0' ? headers : NULL,
        .crt_bundle_attach = NULL,
        .cert_pem = CHAT_TLS_CA_PEM, .skip_cert_common_name_check = true,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&c);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        goto done;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    /* Publish and start the client while holding the connection lock. This
     * closes the stop-vs-start race without keeping the lock during TLS/auth. */
    xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
    if (!ws_want_connected || ws_client) {
        ws_connecting = false;
        xSemaphoreGive(g_conn_mutex);
        esp_websocket_client_destroy(client);
        vTaskDelete(NULL);
        return;
    }
    ws_client = client;
    esp_err_t start_err = esp_websocket_client_start(client);
    if (start_err != ESP_OK) ws_client = NULL;
    ws_connecting = false;
    xSemaphoreGive(g_conn_mutex);

    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(start_err));
        esp_websocket_client_destroy(client);
    }
    vTaskDelete(NULL);
    return;

done:
    xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
    ws_connecting = false;
    xSemaphoreGive(g_conn_mutex);
    vTaskDelete(NULL);
}

/* 对端断开后的完整恢复。事件回调运行在WebSocket任务自身，不能在那里
 * 调用destroy；等待回调退出后再串行化发送、销毁、重新鉴权和连接。 */
static void ws_reconnect_task(void *arg)
{
    esp_websocket_client_handle_t old_client =
        (esp_websocket_client_handle_t)arg;
    vTaskDelay(pdMS_TO_TICKS(200));

    if (g_ws_tx_mutex) xSemaphoreTake(g_ws_tx_mutex, portMAX_DELAY);

    bool owns_client = false;
    xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
    if (ws_client == old_client && ws_want_connected) {
        ws_client = NULL;
        ws_connecting = true; /* 阻止chat_feed在销毁期间重复创建 */
        owns_client = true;
    }
    xSemaphoreGive(g_conn_mutex);

    if (owns_client) {
        esp_websocket_client_destroy(old_client);
        ws_auth_invalidate_cookie();
    }

    xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
    bool reconnect = owns_client && ws_want_connected;
    ws_connecting = false;
    ws_reconnect_pending = false;
    xSemaphoreGive(g_conn_mutex);

    if (reconnect) {
        ESP_LOGI(TAG, "Re-authenticating after remote disconnect");
        /* 保持TX锁直到新连接任务已登记；ChatStop随后仍可把wanted置false，
         * 鉴权任务会检查该状态并安全退出。 */
        realtime_ws_connect();
    }

    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
    vTaskDelete(NULL);
}
void realtime_ws_connect(void) {
    if (!ws_url || !g_conn_mutex) return;

    xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
    ws_want_connected = true;
    if (ws_client || ws_connecting) {
        xSemaphoreGive(g_conn_mutex);
        return;
    }
    ws_connecting = true;
    BaseType_t created = xTaskCreate(ws_connect_task, "ws_connect", 12288, NULL, 3, NULL);
    if (created != pdPASS) {
        ws_connecting = false;
        ESP_LOGE(TAG, "Failed to create WebSocket connect task");
    }
    xSemaphoreGive(g_conn_mutex);
}

void realtime_ws_disconnect(void) {
    esp_websocket_client_handle_t client = NULL;
    /* 先等正在进行的发送完成, 再销毁 client, 避免 use-after-free 崩溃 */
    if (g_ws_tx_mutex) xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(2000));
    if (g_conn_mutex) {
        xSemaphoreTake(g_conn_mutex, portMAX_DELAY);
        ws_want_connected = false;
        client = ws_client;
        ws_client = NULL;
        xSemaphoreGive(g_conn_mutex);
    } else {
        client = ws_client;
        ws_client = NULL;
    }
    if (client) esp_websocket_client_destroy(client);
    ws_connected = false; g_ready = false;
    ws_clear_session_buffers();
    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
}

bool realtime_ws_is_connected(void) { return ws_connected && ws_client; }
bool realtime_ws_wait_connected(int to_ms) {
    if (ws_connected) return true;
    if (!ws_connect_sem) return false;
    xSemaphoreTake(ws_connect_sem, to_ms > 0 ? pdMS_TO_TICKS(to_ms) : portMAX_DELAY);
    return ws_connected;
}

void realtime_ws_deinit(void) {
    realtime_ws_disconnect();
    realtime_ws_clear_response();
    if (ws_connect_sem) vSemaphoreDelete(ws_connect_sem);
    if (g_resp_mutex) vSemaphoreDelete(g_resp_mutex);
    if (g_msg_queue) {
        if (g_msg_queue_with_caps) vQueueDeleteWithCaps(g_msg_queue);
        else vQueueDelete(g_msg_queue);
    }
    if (g_tts_queue) {
        tts_audio_item_t item;
        while (tts_queue_receive(&item)) free(item.data);
        if (g_tts_queue_with_caps) vQueueDeleteWithCaps(g_tts_queue);
        else vQueueDelete(g_tts_queue);
    }
    if (g_tx_queue) { ws_tx_msg_t m; while (xQueueReceive(g_tx_queue, &m, 0) == pdTRUE) free(m.json); vQueueDelete(g_tx_queue); }
    if (g_rx_payload) { free(g_rx_payload); g_rx_payload = NULL; }
    g_rx_payload_len = 0; g_rx_payload_received = 0;
    if (ws_url) free(ws_url);
}
