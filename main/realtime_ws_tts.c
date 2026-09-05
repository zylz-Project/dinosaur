/*
 * realtime_ws_tts.c — WebSocket 实时音频通道（TTS 流控与发送层）
 *
 * 从 realtime_ws.c 拆出的"发送/反馈"半边：16 槽 TTS 流状态（g_tts_flows，
 * 一个 turn 被服务端拆成多个 seq 流，各自独立跟踪反馈）+ playback_started/
 * buffer/finished 反馈 + WS_TX 异步发送队列（播放线程只入队不阻塞网络），
 * 以及 send_hello/send_audio/send_text 的同步发送入口。
 * 流控阈值、反馈 JSON 格式与发送重试策略逐字保留（拆文件≠改逻辑）。
 * 连接生命周期在 realtime_ws.c，收包解析在 realtime_ws_protocol.c。
 */

#include "realtime_ws.h"
#include "realtime_ws_internal.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "REALTIME_WS";

/* TTS audio queue (proc_task → response_task) — prevents overwrite during playback.
 * 每个队列条目携带所属流的 ID，播放侧据此回传正确的流反馈。
 * 与参考工程保持 1024 深度；队列控制块由已启用的 PSRAM malloc 承担，
 * 防止长回复或网络突发时丢 chunk。 */
QueueHandle_t g_tts_queue = NULL;
bool g_tts_queue_with_caps = false;
static int  g_tts_play_sr = 0, g_tts_play_bits = 0;
/* 服务端使用和网页播放器相同的反馈消息控制TTS下发窗口。
 * 一个 turn 会被服务端拆成多个 TTS 流 (seq=1,2,3...)。每个流独立跟踪状态，
 * 新的 tts_audio_start 不能覆盖旧流尚未完成的 playback_finished 反馈，
 * 否则服务端会认为缓冲容量未释放，停止下发并断连。 */
tts_flow_state_t g_tts_flows[TTS_FLOW_SLOTS] = {0};
int g_tts_cur_flow = -1;   /* 最近一次 tts_audio_start 的槽位 */
size_t g_tts_queued_bytes = 0;
portMUX_TYPE g_tts_flow_mux = portMUX_INITIALIZER_UNLOCKED;
/* 断连诊断计数：区分“反馈未发出”和“服务端收到反馈后仍关闭”。 */
volatile uint32_t g_ctrl_tx_ok = 0;
volatile uint32_t g_ctrl_tx_failed = 0;
volatile uint32_t g_buffer_tx_ok = 0;
volatile uint32_t g_buffer_tx_failed = 0;
/* ---- 反馈消息异步发送: 播放线程只入队, 不能阻塞在网络上 ---- */
QueueHandle_t g_tx_queue = NULL;
TaskHandle_t g_ws_tx_task_handle = NULL;
static void ws_tx_flush_tts_buffers(void);
static bool ws_sendable(void);
/* 发送互斥锁: 所有 esp_websocket_client_send_* 调用与 realtime_ws_disconnect()
 * 串行化, 避免发送中途 client 被 destroy 造成 use-after-free 崩溃。 */
SemaphoreHandle_t g_ws_tx_mutex = NULL;
static void ws_tx_send_queued(ws_tx_msg_t *m)
{
    if (!m->json) return;
    /* 关键控制消息(playback_finished/tts_playback_started)重试, 普通水位只发一次,
     * 避免 tts_playback_buffer 连续重试占锁阻塞更重要的反馈。 */
    bool critical = (strstr(m->json, "playback_finished") != NULL) ||
                    (strstr(m->json, "tts_playback_started") != NULL);
    int max_attempt = critical ? 3 : 1;
    int sent = -1;
    if (g_ws_tx_mutex && xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "WS control TX lock timeout, dropping: %.48s", m->json);
        free(m->json);
        return;
    }
    for (int attempt = 1; attempt <= max_attempt && ws_sendable(); attempt++) {
        sent = esp_websocket_client_send_text(ws_client, m->json, strlen(m->json),
                                              pdMS_TO_TICKS(1000));
        if (sent > 0) break;
        if (attempt < max_attempt) {
            ESP_LOGW(TAG, "WS control TX retry %d/%d: %.48s", attempt, max_attempt, m->json);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
    if (sent <= 0) {
        g_ctrl_tx_failed++;
        ESP_LOGE(TAG, "WS control TX failed: %.80s", m->json);
    } else {
        g_ctrl_tx_ok++;
    }
    free(m->json);
}

void ws_tx_task(void *arg)
{
    ws_tx_msg_t m;
    ws_tx_msg_t msgs[32];
    while (1) {
        /* Audio receive/playback paths only mark the latest water level and
         * wake this task.  They never call the websocket sender themselves. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TTS_BUFFER_DRAIN_REPORT_INTERVAL_MS));

        /* 一次性清空队列, 然后按优先级发送:
         * 关键控制消息(playback_finished/tts_playback_started)优先,
         * 普通 tts_playback_buffer 最后, 避免被旧水位阻塞。 */
        int n = 0;
        if (xQueueReceive(g_tx_queue, &m, 0) == pdTRUE) msgs[n++] = m;
        while (n < 32 && xQueueReceive(g_tx_queue, &msgs[n], 0) == pdTRUE) n++;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n; i++) {
                if (!msgs[i].json) continue;
                bool critical = (strstr(msgs[i].json, "playback_finished") != NULL) ||
                                (strstr(msgs[i].json, "tts_playback_started") != NULL);
                if ((pass == 0 && critical) || (pass == 1 && !critical)) {
                    ws_tx_send_queued(&msgs[i]);
                    msgs[i].json = NULL;
                }
            }
        }
        ws_tx_flush_tts_buffers();
    }
    vTaskDelete(NULL);
}

static void ws_tx_enqueue(const char *json)
{
    if (!g_tx_queue) return;
    ws_tx_msg_t m = { .json = strdup(json) };
    if (!m.json) {
        ESP_LOGE(TAG, "WS control TX allocation failed");
        return;
    }
    if (xQueueSend(g_tx_queue, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
        /* playback_finished 丢失会导致服务端认为缓冲未释放 → Connection reset by peer。
         * 队列满时宁可短暂阻塞播放线程, 也不能丢关键反馈。 */
        ESP_LOGE(TAG, "WS control TX queue full (blocked 500ms): %.80s", m.json);
        free(m.json);
    } else if (g_ws_tx_task_handle) {
        xTaskNotifyGive(g_ws_tx_task_handle);
    }
}
/* ---- TTS 流状态槽位 (调用方需持有 g_tts_flow_mux) ---- */
tts_flow_state_t *flow_find(int session_id, int generation_id, int seq)
{
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *f = &g_tts_flows[i];
        if (f->valid && f->seq == seq && f->generation_id == generation_id &&
            (session_id == 0 || f->session_id == session_id))
            return f;
    }
    return NULL;
}

tts_flow_state_t *flow_alloc(int session_id, int generation_id, int seq)
{
    tts_flow_state_t *f = flow_find(session_id, generation_id, seq);
    if (f) {
        /* 跨回合 seq/session/generation 会复用旧槽位, 必须同步当前流指针,
         * 否则 flow_cur() 仍指向上回合的旧流: 音频条目打错 seq、预缓冲被
         * 旧流的 stream_ended 提前打断、反馈发错流 → 服务端认为缓冲未释放。 */
        g_tts_cur_flow = (int)(f - g_tts_flows);
        return f;
    }
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *c = &g_tts_flows[i];
        if (!c->valid || c->playback_finished) {
            memset(c, 0, sizeof(*c));
            g_tts_cur_flow = i;
            return c;
        }
    }
    /* 超过槽位数时覆盖旧流会丢掉 playback_finished。扩到16个槽位后
     * 正常回复不应再进入此分支；保留显式日志便于发现异常长回复。 */
    ESP_EARLY_LOGE(TAG, "TTS flow slots exhausted (%d), overwriting seq=%d",
                   TTS_FLOW_SLOTS, g_tts_flows[0].seq);
    memset(&g_tts_flows[0], 0, sizeof(g_tts_flows[0]));
    g_tts_cur_flow = 0;
    return &g_tts_flows[0];
}

tts_flow_state_t *flow_cur(void)
{
    return (g_tts_cur_flow >= 0 && g_tts_flows[g_tts_cur_flow].valid)
               ? &g_tts_flows[g_tts_cur_flow] : NULL;
}

void flow_mark_all_ended(void)
{
    for (int i = 0; i < TTS_FLOW_SLOTS; i++)
        if (g_tts_flows[i].valid) g_tts_flows[i].stream_ended = true;
}

bool tts_queue_send(tts_audio_item_t *item)
{
    /* 先记账再入队，避免消费者恰好在xQueueSend返回后抢占导致字节数残留。 */
    portENTER_CRITICAL(&g_tts_flow_mux);
    size_t len = (size_t)item->len;
    g_tts_queued_bytes += len;
    portEXIT_CRITICAL(&g_tts_flow_mux);
    if (xQueueSend(g_tts_queue, item, 0) != pdTRUE) {
        portENTER_CRITICAL(&g_tts_flow_mux);
        g_tts_queued_bytes = g_tts_queued_bytes >= len ? g_tts_queued_bytes - len : 0;
        portEXIT_CRITICAL(&g_tts_flow_mux);
        return false;
    }
    /* 网页AudioWorklet每收到chunk就报告全局待播水位，即使该seq尚未起播。
     * 服务端依靠此反馈管理browser TTS容量，不能等playback_started后再报。 */
    portENTER_CRITICAL(&g_tts_flow_mux);
    tts_flow_state_t *flow = flow_find(item->session_id,
                                       item->generation_id, item->seq);
    if (flow && !flow->playback_finished) {
        flow->pending_queued_ms = tts_queued_ms(g_tts_queued_bytes);
        flow->buffer_dirty = true;
        flow->buffer_rx_ack = true;
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);
    if (g_ws_tx_task_handle) xTaskNotifyGive(g_ws_tx_task_handle);
    return true;
}

bool tts_queue_receive(tts_audio_item_t *item)
{
    if (xQueueReceive(g_tts_queue, item, 0) != pdTRUE)
        return false;
    portENTER_CRITICAL(&g_tts_flow_mux);
    size_t len = (size_t)item->len;
    g_tts_queued_bytes = g_tts_queued_bytes >= len ? g_tts_queued_bytes - len : 0;
    portEXIT_CRITICAL(&g_tts_flow_mux);
    return true;
}
bool realtime_ws_get_tts_audio(uint8_t **out_data, int *out_len,
                               int *out_session_id, int *out_generation_id, int *out_seq) {
    tts_audio_item_t item;
    if (!tts_queue_receive(&item)) return false;
    *out_data = item.data;
    *out_len = item.len;
    /* Format belongs to this queue item.  Do not rely on producer-side globals:
     * the WebSocket task may already be decoding a later TTS message. */
    g_tts_play_sr = item.sample_rate;
    g_tts_play_bits = item.bits;
    if (out_session_id)   *out_session_id   = item.session_id;
    if (out_generation_id)*out_generation_id = item.generation_id;
    if (out_seq)          *out_seq          = item.seq;
    return true;
}

int realtime_ws_get_tts_queue_depth(void) {
    return g_tts_queue ? (int)uxQueueMessagesWaiting(g_tts_queue) : 0;
}
bool realtime_ws_get_tts_stream_format(int *sr, int *bits) {
    if (g_tts_play_sr <= 0 || g_tts_play_bits <= 0) return false;
    *sr = g_tts_play_sr; *bits = g_tts_play_bits;
    return true;
}
/* 权威连接检查: 用客户端内部状态, 避免断连后缓存标志 stale 导致反复调发送 */
static bool ws_sendable(void)
{
    return ws_connected && ws_client &&
           esp_websocket_client_is_connected(ws_client);
}
int realtime_ws_send_text(const char *text) {
    if (!text) return -1;
    int r = -1;
    if (g_ws_tx_mutex && xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    if (ws_sendable())
        r = esp_websocket_client_send_text(ws_client, text, strlen(text), pdMS_TO_TICKS(5000));
    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
    return r > 0 ? 0 : -1;
}
int tts_queued_ms(size_t queued_bytes)
{
    int sample_rate = g_tts_play_sr > 0 ? g_tts_play_sr : g_tts_wav_sr;
    int bits = g_tts_play_bits > 0 ? g_tts_play_bits : g_tts_wav_bits;
    if (sample_rate <= 0) sample_rate = 48000;
    if (bits <= 0) bits = 16;
    uint64_t bytes_per_second = (uint64_t)sample_rate * (uint64_t)(bits / 8);
    return bytes_per_second > 0 ? (int)((uint64_t)queued_bytes * 1000ULL / bytes_per_second) : 0;
}

/* 与网页 AudioWorklet 的反馈语义一致：每个接收 chunk 立即确认，播放消耗
 * 造成的水位变化则每200ms补报一次。只保留最新水位，不把历史反馈排队。
 * 服务端可能按一次 buffer 反馈释放下一块音频；将接收确认节流到80ms会把
 * 40ms/chunk 的流稳定限制在约0.5x。 */
static void ws_tx_flush_tts_buffers(void)
{
    tts_flow_state_t flows[TTS_FLOW_SLOTS];
    int queued_ms[TTS_FLOW_SLOTS];
    bool first_report[TTS_FLOW_SLOTS];
    int n = 0;
    TickType_t now = xTaskGetTickCount();

    portENTER_CRITICAL(&g_tts_flow_mux);
    int current_queued_ms = tts_queued_ms(g_tts_queued_bytes);
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *f = &g_tts_flows[i];
        int elapsed_ms = (int)((now - f->last_buffer_sent_tick) * portTICK_PERIOD_MS);
        if (f->valid && f->buffer_dirty && !f->playback_finished &&
            (f->last_buffer_sent_tick == 0 ||
             f->buffer_rx_ack ||
             elapsed_ms >= TTS_BUFFER_DRAIN_REPORT_INTERVAL_MS ||
             f->pending_queued_ms == 0)) {
            flows[n] = *f;
            queued_ms[n] = current_queued_ms;
            first_report[n] = (f->last_buffer_sent_tick == 0);
            n++;
            f->buffer_dirty = false;
            f->buffer_rx_ack = false;
            f->last_buffer_sent_tick = now;
        }
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);

    char json[416];
    bool locked = (!g_ws_tx_mutex) ||
                  (xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(200)) == pdTRUE);
    for (int i = 0; i < n && locked; i++) {
        tts_flow_state_t *f = &flows[i];
        snprintf(json, sizeof(json),
                 "{\"type\":\"tts_playback_buffer\",\"session_id\":%d,"
                 "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"generation_id\":%d,\"seq\":%d,\"queued_ms\":%d}",
                 f->session_id, f->realtime_session_id, f->turn_id,
                 f->generation_id, f->seq, queued_ms[i]);
        int sent = -1;
        if (ws_sendable())
            sent = esp_websocket_client_send_text(ws_client, json, strlen(json),
                                                  pdMS_TO_TICKS(1000));
        if (sent <= 0) {
            g_buffer_tx_failed++;
            portENTER_CRITICAL(&g_tts_flow_mux);
            tts_flow_state_t *current = flow_find(f->session_id, f->generation_id, f->seq);
            if (current && !current->playback_finished)
                current->buffer_dirty = true;
            portEXIT_CRITICAL(&g_tts_flow_mux);
        } else {
            g_buffer_tx_ok++;
            if (first_report[i]) {
                ESP_LOGI(TAG, "TTS flow: first buffer seq=%d, queued=%dms, started=%d",
                         f->seq, queued_ms[i], f->playback_started);
            }
        }
    }
    if (g_ws_tx_mutex && locked) xSemaphoreGive(g_ws_tx_mutex);
}

int realtime_ws_tts_queued_ms(void)
{
    size_t bytes;
    portENTER_CRITICAL(&g_tts_flow_mux);
    bytes = g_tts_queued_bytes;
    portEXIT_CRITICAL(&g_tts_flow_mux);
    return tts_queued_ms(bytes);
}
bool realtime_ws_tts_current_stream_ended(void)
{
    bool ended = false;
    portENTER_CRITICAL(&g_tts_flow_mux);
    tts_flow_state_t *f = flow_cur();
    if (f) ended = f->stream_ended;
    portEXIT_CRITICAL(&g_tts_flow_mux);
    return ended;
}

/* 是否还有尚未结束的音频流在途(服务端仍在合成/下发后续句子)。
 * 有的话播放结束后不能恢复录音, 否则会把句间停顿误判为回合结束。 */
bool realtime_ws_tts_has_pending_stream(void)
{
    bool pending = false;
    portENTER_CRITICAL(&g_tts_flow_mux);
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *f = &g_tts_flows[i];
        if (f->valid && !f->playback_finished && !f->stream_ended) {
            pending = true;
            break;
        }
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);
    return pending;
}

/* ---- 反馈消息: 每个流按 seq 独立上报, 播放线程只入队不阻塞 ---- */
void realtime_ws_tts_playback_started(int session_id, int generation_id, int seq)
{
    tts_flow_state_t flow;
    size_t queued_bytes = 0;
    bool should_send = false;

    portENTER_CRITICAL(&g_tts_flow_mux);
    tts_flow_state_t *f = flow_find(session_id, generation_id, seq);
    if (!f) f = flow_cur();
    if (f && f->valid && !f->playback_started && !f->playback_finished) {
        f->playback_started = true;
        flow = *f;
        queued_bytes = g_tts_queued_bytes;
        should_send = true;
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);
    if (!should_send) return;

    char json[384];
    snprintf(json, sizeof(json),
             "{\"type\":\"tts_playback_started\",\"session_id\":%d,"
             "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
             "\"generation_id\":%d,\"seq\":%d}",
             flow.session_id, flow.realtime_session_id, flow.turn_id,
             flow.generation_id, flow.seq);
    ws_tx_enqueue(json);
    ESP_LOGI(TAG, "TTS flow: playback started seq=%d, queued=%dms",
             flow.seq, tts_queued_ms(queued_bytes));
}

void realtime_ws_tts_playback_buffer(int session_id, int generation_id, int seq)
{
    portENTER_CRITICAL(&g_tts_flow_mux);
    tts_flow_state_t *f = flow_find(session_id, generation_id, seq);
    if (!f) f = flow_cur();
    if (f && f->valid && f->playback_started && !f->playback_finished) {
        f->pending_queued_ms = tts_queued_ms(g_tts_queued_bytes);
        f->buffer_dirty = true;
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);
    if (g_ws_tx_task_handle) xTaskNotifyGive(g_ws_tx_task_handle);
}

/* 队列已经播放到下一seq，说明前一seq的最后一个采样已交给I2S。
 * 即使总队列中还有后续seq，也必须立刻回传前一流的playback_finished。 */
bool realtime_ws_tts_finish_stream(int session_id, int generation_id, int seq)
{
    tts_flow_state_t flow;
    size_t queued_bytes = 0;
    bool should_send = false;

    portENTER_CRITICAL(&g_tts_flow_mux);
    tts_flow_state_t *f = flow_find(session_id, generation_id, seq);
    if (f && f->valid && f->stream_ended && f->playback_started &&
        !f->playback_finished) {
        f->playback_finished = true;
        f->buffer_dirty = false;
        f->buffer_rx_ack = false;
        flow = *f;
        queued_bytes = g_tts_queued_bytes;
        should_send = true;
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);
    if (!should_send) return false;

    char json[448];
    snprintf(json, sizeof(json),
             "{\"type\":\"tts_playback_buffer\",\"session_id\":%d,"
             "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
             "\"generation_id\":%d,\"seq\":%d,\"queued_ms\":%d}",
             flow.session_id, flow.realtime_session_id, flow.turn_id,
             flow.generation_id, flow.seq, tts_queued_ms(queued_bytes));
    ws_tx_enqueue(json);
    snprintf(json, sizeof(json),
             "{\"type\":\"playback_finished\",\"session_id\":%d,"
             "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
             "\"generation_id\":%d,\"seq\":%d,\"last_played_seq\":%d}",
             flow.session_id, flow.realtime_session_id, flow.turn_id,
             flow.generation_id, flow.seq, flow.seq);
    ws_tx_enqueue(json);
    ESP_LOGI(TAG, "TTS flow: playback finished seq=%d (next seq ready)", flow.seq);
    return true;
}

/* 队列已空时, 为所有已结束且已开始播放的流补发 playback_finished。
 * 流之间切换时旧流的 state 不能被新流覆盖, 否则服务端等不到缓冲释放。 */
bool realtime_ws_tts_finish_if_drained(void)
{
    tts_flow_state_t flows[TTS_FLOW_SLOTS];
    int n = 0;

    portENTER_CRITICAL(&g_tts_flow_mux);
    if (g_tts_queued_bytes == 0) {
        for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
            tts_flow_state_t *f = &g_tts_flows[i];
            if (f->valid && f->playback_started && !f->playback_finished && f->stream_ended) {
                f->playback_finished = true;
                f->buffer_dirty = false;
                f->buffer_rx_ack = false;
                flows[n++] = *f;
            }
        }
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);

    char json[448];
    for (int i = 0; i < n; i++) {
        tts_flow_state_t *f = &flows[i];
        snprintf(json, sizeof(json),
                 "{\"type\":\"tts_playback_buffer\",\"session_id\":%d,"
                 "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"generation_id\":%d,\"seq\":%d,\"queued_ms\":0}",
                 f->session_id, f->realtime_session_id, f->turn_id,
                 f->generation_id, f->seq);
        ws_tx_enqueue(json);
        snprintf(json, sizeof(json),
                 "{\"type\":\"playback_finished\",\"session_id\":%d,"
                 "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"generation_id\":%d,\"seq\":%d,\"last_played_seq\":%d}",
                 f->session_id, f->realtime_session_id, f->turn_id,
                 f->generation_id, f->seq, f->seq);
        ws_tx_enqueue(json);
        ESP_LOGI(TAG, "TTS flow: playback finished seq=%d", f->seq);
    }
    return n > 0;
}

void realtime_ws_tts_playback_interrupted(void)
{
    tts_flow_state_t flows[TTS_FLOW_SLOTS];
    int n = 0;

    portENTER_CRITICAL(&g_tts_flow_mux);
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *f = &g_tts_flows[i];
        if (f->valid && f->playback_started && !f->playback_finished) {
            f->playback_finished = true;
            f->buffer_dirty = false;
            f->buffer_rx_ack = false;
            flows[n++] = *f;
        }
    }
    portEXIT_CRITICAL(&g_tts_flow_mux);

    char json[448];
    for (int i = 0; i < n; i++) {
        tts_flow_state_t *f = &flows[i];
        snprintf(json, sizeof(json),
                 "{\"type\":\"tts_playback_buffer\",\"session_id\":%d,"
                 "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"generation_id\":%d,\"seq\":%d,\"queued_ms\":0}",
                 f->session_id, f->realtime_session_id, f->turn_id,
                 f->generation_id, f->seq);
        ws_tx_enqueue(json);
        snprintf(json, sizeof(json),
                 "{\"type\":\"playback_finished\",\"session_id\":%d,"
                 "\"realtime_session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"generation_id\":%d,\"seq\":%d,\"last_played_seq\":%d,"
                 "\"interrupted\":true}",
                 f->session_id, f->realtime_session_id, f->turn_id,
                 f->generation_id, f->seq, f->seq);
        ws_tx_enqueue(json);
        ESP_LOGI(TAG, "TTS flow: playback finished seq=%d (interrupted)", f->seq);
    }
}

int realtime_ws_send_hello(void) {
    int r = realtime_ws_send_text("{\"type\":\"hello\",\"mime_type\":\"audio/pcm;rate=16000;channels=1\",\"sample_rate\":16000,\"channels\":1,\"frame_duration_ms\":40}");
    if (r != 0) return r;
    return realtime_ws_send_text("{\"type\":\"reply_settings\",\"route\":null,\"agent_thinking\":null}");
}

int realtime_ws_send_audio(const int16_t *pcm, int n) {
    if (!pcm || n <= 0) return -1;
    int r = -1;
    /* 有界超时: 不能无限阻塞占着发送锁, 否则控制反馈(playback_started等)
     * 发不出去, 且停止对话时 destroy 会被堵死。超时丢弃这一帧即可。 */
    if (g_ws_tx_mutex && xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    if (ws_sendable())
        r = esp_websocket_client_send_bin(ws_client, (const char*)pcm, n*2, pdMS_TO_TICKS(1000));
    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
    return r > 0 ? 0 : -1;
}
