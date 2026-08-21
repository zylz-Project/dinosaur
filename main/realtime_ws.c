/*
 * realtime_ws.c — WebSocket 实时音频 (使用 esp_websocket_client)
 *
 * 应用层负责：JSON 解析、TTS 累积、响应队列。
 * 使用独立的 TTS 音频队列避免播放期间 chunk 被覆盖。
 * TTS 音频完整接收后一次性解码，保证 WAV 采样对齐。
 */

#include "realtime_ws.h"
#include "display.h"
#include "ws_auth.h"
#include "chat_cert.h"
#include "wifi.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "REALTIME_WS";

/* ===================================================================
 *  State
 * =================================================================== */
static esp_websocket_client_handle_t ws_client = NULL;
static char *ws_url = NULL;
static volatile bool ws_connected = false;
static volatile bool g_ready = false;
static volatile TickType_t g_last_rx_tick = 0;  /* 最后收到数据的时间 */
static SemaphoreHandle_t ws_connect_sem = NULL;
static SemaphoreHandle_t g_conn_mutex = NULL;
static bool ws_connecting = false;
static bool ws_want_connected = false;
static bool ws_reconnect_pending = false;
static TickType_t ws_connected_tick = 0;
static void ws_reconnect_task(void *arg);

/* Response output */
static ws_resp_type_t g_resp_type = WS_RESP_NONE;
static char    *g_resp_text  = NULL;
static uint8_t *g_resp_audio = NULL;
static int      g_resp_audio_len = 0;
static char    *g_llm_buf = NULL;
static SemaphoreHandle_t g_resp_mutex = NULL;
static realtime_ws_emotion_t g_emotion = { .confidence = -1 };
static bool g_emotion_pending = false;

/* Message queue (event handler → processing task) */
typedef struct { uint8_t *data; int len; bool is_binary; } ws_msg_t;
#define WS_MSG_QUEUE_LEN 512
static QueueHandle_t g_msg_queue = NULL;
static bool g_msg_queue_with_caps = false;
static uint32_t g_msg_queue_drop_count = 0;

/* esp_websocket_client may emit one payload in several DATA events. */
static uint8_t *g_rx_payload = NULL;
static int g_rx_payload_len = 0;
static int g_rx_payload_received = 0;
static bool g_rx_payload_binary = false;

/* TTS audio queue (proc_task → response_task) — prevents overwrite during playback.
 * 每个队列条目携带所属流的 ID，播放侧据此回传正确的流反馈。
 * 与参考工程保持 1024 深度；队列控制块由已启用的 PSRAM malloc 承担，
 * 防止长回复或网络突发时丢 chunk。 */
typedef struct {
    uint8_t *data;
    int len;
    int sample_rate;
    int bits;
    int session_id;
    int generation_id;
    int seq;
} tts_audio_item_t;
#define TTS_QUEUE_LEN 1024
static QueueHandle_t g_tts_queue = NULL;
static bool g_tts_queue_with_caps = false;

/* TTS 流式解码状态 (新协议 tts_audio_start/chunk 与旧协议 WAV 共用) */
static int  g_tts_data_ofs = -1;
static int  g_tts_decoded_ofs = 0;
static bool g_tts_hdr_ok = false;
static int  g_tts_wav_sr = 0, g_tts_wav_bits = 0;
static int  g_tts_play_sr = 0, g_tts_play_bits = 0;
static int  g_tts_chunk_count = 0;
static int  g_tts_last_chunk_seq = -1;
static int  g_tts_missing_chunks = 0;
static int  g_tts_decode_failures = 0;
static size_t g_tts_stream_bytes = 0;
static TickType_t g_tts_stream_start_tick = 0;
static TickType_t g_tts_last_chunk_tick = 0;
static int g_tts_max_chunk_gap_ms = 0;
static int g_tts_slow_chunk_gaps = 0;

/* 服务端使用和网页播放器相同的反馈消息控制TTS下发窗口。
 * 一个 turn 会被服务端拆成多个 TTS 流 (seq=1,2,3...)。每个流独立跟踪状态，
 * 新的 tts_audio_start 不能覆盖旧流尚未完成的 playback_finished 反馈，
 * 否则服务端会认为缓冲容量未释放，停止下发并断连。 */
#define TTS_ID_LEN 48
static TickType_t g_last_text_delta_tick = 0;
static char g_last_text_delta_turn[TTS_ID_LEN] = "";

typedef struct {
    bool valid;
    bool stream_ended;
    bool playback_started;
    bool playback_finished;
    bool buffer_dirty;
    /* The browser reports once for every received chunk.  This flag bypasses
     * the periodic drain-report limiter for that receive-side acknowledgement. */
    bool buffer_rx_ack;
    int pending_queued_ms;
    TickType_t last_buffer_sent_tick;
    int session_id;
    char realtime_session_id[TTS_ID_LEN];
    char turn_id[TTS_ID_LEN];
    int generation_id;
    int seq;
} tts_flow_state_t;

#define TTS_FLOW_SLOTS 16
static tts_flow_state_t g_tts_flows[TTS_FLOW_SLOTS] = {0};
static int g_tts_cur_flow = -1;   /* 最近一次 tts_audio_start 的槽位 */
static size_t g_tts_queued_bytes = 0;
static portMUX_TYPE g_tts_flow_mux = portMUX_INITIALIZER_UNLOCKED;
static int tts_queued_ms(size_t queued_bytes);

/* 断连诊断计数：区分“反馈未发出”和“服务端收到反馈后仍关闭”。 */
static volatile uint32_t g_ctrl_tx_ok = 0;
static volatile uint32_t g_ctrl_tx_failed = 0;
static volatile uint32_t g_buffer_tx_ok = 0;
static volatile uint32_t g_buffer_tx_failed = 0;

/* ---- 反馈消息异步发送: 播放线程只入队, 不能阻塞在网络上 ---- */
#define WS_TX_QUEUE_LEN 64
#define TTS_BUFFER_DRAIN_REPORT_INTERVAL_MS 200
typedef struct { char *json; } ws_tx_msg_t;
static QueueHandle_t g_tx_queue = NULL;
static TaskHandle_t g_ws_tx_task_handle = NULL;
static void ws_tx_flush_tts_buffers(void);
static bool ws_sendable(void);

/* 发送互斥锁: 所有 esp_websocket_client_send_* 调用与 realtime_ws_disconnect()
 * 串行化, 避免发送中途 client 被 destroy 造成 use-after-free 崩溃。 */
static SemaphoreHandle_t g_ws_tx_mutex = NULL;

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

static void ws_tx_task(void *arg)
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
static tts_flow_state_t *flow_find(int session_id, int generation_id, int seq)
{
    for (int i = 0; i < TTS_FLOW_SLOTS; i++) {
        tts_flow_state_t *f = &g_tts_flows[i];
        if (f->valid && f->seq == seq && f->generation_id == generation_id &&
            (session_id == 0 || f->session_id == session_id))
            return f;
    }
    return NULL;
}

static tts_flow_state_t *flow_alloc(int session_id, int generation_id, int seq)
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

static tts_flow_state_t *flow_cur(void)
{
    return (g_tts_cur_flow >= 0 && g_tts_flows[g_tts_cur_flow].valid)
               ? &g_tts_flows[g_tts_cur_flow] : NULL;
}

static void flow_mark_all_ended(void)
{
    for (int i = 0; i < TTS_FLOW_SLOTS; i++)
        if (g_tts_flows[i].valid) g_tts_flows[i].stream_ended = true;
}

static bool tts_queue_send(tts_audio_item_t *item)
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

static bool tts_queue_receive(tts_audio_item_t *item)
{
    if (xQueueReceive(g_tts_queue, item, 0) != pdTRUE)
        return false;
    portENTER_CRITICAL(&g_tts_flow_mux);
    size_t len = (size_t)item->len;
    g_tts_queued_bytes = g_tts_queued_bytes >= len ? g_tts_queued_bytes - len : 0;
    portEXIT_CRITICAL(&g_tts_flow_mux);
    return true;
}

/* ===================================================================
 *  Response API
 * =================================================================== */
ws_resp_type_t realtime_ws_get_response(char **out_text, uint8_t **out_audio, int *out_audio_len) {
    ws_resp_type_t t = WS_RESP_NONE;
    if (xSemaphoreTake(g_resp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return WS_RESP_NONE;
    t = g_resp_type;
    if (out_text)      { *out_text = g_resp_text; g_resp_text = NULL; }
    if (out_audio)     { *out_audio = g_resp_audio; g_resp_audio = NULL; }
    if (out_audio_len) { *out_audio_len = g_resp_audio_len; g_resp_audio_len = 0; }
    g_resp_type = WS_RESP_NONE;
    xSemaphoreGive(g_resp_mutex);
    return t;
}
void realtime_ws_clear_response(void) {
    if (xSemaphoreTake(g_resp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_resp_type = WS_RESP_NONE;
    if (g_resp_text)  { free(g_resp_text); g_resp_text = NULL; }
    if (g_resp_audio) { free(g_resp_audio); g_resp_audio = NULL; }
    g_resp_audio_len = 0;
    if (g_llm_buf)    { free(g_llm_buf); g_llm_buf = NULL; }
    memset(&g_emotion, 0, sizeof(g_emotion));
    g_emotion.confidence = -1;
    g_emotion_pending = false;
    xSemaphoreGive(g_resp_mutex);
    tts_audio_item_t item;
    while (tts_queue_receive(&item)) free(item.data);
}
bool realtime_ws_is_ready(void) { return g_ready; }

int realtime_ws_ms_since_last_rx(void) {
    if (g_last_rx_tick == 0) return 0;
    return (int)((xTaskGetTickCount() - g_last_rx_tick) * portTICK_PERIOD_MS);
}

bool realtime_ws_get_emotion(realtime_ws_emotion_t *out) {
    if (!out || !g_resp_mutex) return false;
    if (xSemaphoreTake(g_resp_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool available = g_emotion_pending;
    if (available) {
        *out = g_emotion;
        g_emotion_pending = false;
    }
    xSemaphoreGive(g_resp_mutex);
    return available;
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

/* ===================================================================
 *  Response setters
 * =================================================================== */
static void set_resp(ws_resp_type_t t, const char *text, uint8_t *audio, int alen) {
    if (xSemaphoreTake(g_resp_mutex, portMAX_DELAY) != pdTRUE) { if (audio) free(audio); return; }
    switch (t) {
    case WS_RESP_TTS_AUDIO:
        {
            bool is_wav = audio && alen >= 4 && memcmp(audio, "RIFF", 4) == 0;
            portENTER_CRITICAL(&g_tts_flow_mux);
            tts_audio_item_t item = {
                .data = audio,
                .len = alen,
                .sample_rate = is_wav ? 0 : g_tts_wav_sr,
                .bits = is_wav ? 0 : g_tts_wav_bits,
            };
            tts_flow_state_t *f = flow_cur();
            if (f) {
                item.session_id = f->session_id;
                item.generation_id = f->generation_id;
                item.seq = f->seq;
            }
            portEXIT_CRITICAL(&g_tts_flow_mux);
            if (!tts_queue_send(&item)) {
                ESP_LOGW(TAG, "TTS queue full, dropping chunk");
                free(audio);
            }
        }
        break;
    case WS_RESP_LLM_DELTA:
        if (text) {
            if (g_llm_buf) {
                int old = strlen(g_llm_buf);
                char *nb = realloc(g_llm_buf, old + strlen(text) + 1);
                if (nb) { g_llm_buf = nb; strcat(g_llm_buf, text); }
            } else g_llm_buf = strdup(text);
        }
        g_resp_type = WS_RESP_LLM_DELTA; break;
    case WS_RESP_TURN_END:
        if (g_resp_text) free(g_resp_text);
        g_resp_text = g_llm_buf; g_llm_buf = NULL; g_resp_type = WS_RESP_TURN_END; break;
    default:
        if (g_resp_text) free(g_resp_text);
        g_resp_text = text ? strdup(text) : NULL; g_resp_type = t; break;
    }
    xSemaphoreGive(g_resp_mutex);
}

/* ===================================================================
 *  JSON parser
 * =================================================================== */
static void parse_json(const char *json, int len) {
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "cJSON(%d): %.80s", len, json); return; }
    cJSON *t = cJSON_GetObjectItem(root, "type");
    const char *ts = (t && cJSON_IsString(t)) ? t->valuestring : NULL;
    if (!ts) { cJSON_Delete(root); return; }

    if (!strcmp(ts, "ready")) { g_ready = true; display_set_ws(true, true); ESP_LOGI(TAG, "READY"); }
    else if (!strcmp(ts, "tts_audio")) {
        cJSON *d = cJSON_GetObjectItem(root, "data");
        cJSON *fmt = cJSON_GetObjectItem(root, "format");
        cJSON *sr = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *ch = cJSON_GetObjectItem(root, "channels");
        if (fmt && cJSON_IsString(fmt) && !strcmp(fmt->valuestring, "pcm_s16le") &&
            sr && cJSON_IsNumber(sr) && sr->valueint > 0 &&
            (!ch || (cJSON_IsNumber(ch) && ch->valueint == 1))) {
            g_tts_wav_sr = sr->valueint;
            g_tts_wav_bits = 16;
            g_tts_hdr_ok = true;
        }
        if (d && cJSON_IsString(d) && d->valuestring) {
            int bl = strlen(d->valuestring), dm = (bl+2)/4*3+10;
            uint8_t *dec = heap_caps_malloc(dm, MALLOC_CAP_SPIRAM); if (!dec) dec = malloc(dm);
            if (dec) { size_t ol = 0;
                if (mbedtls_base64_decode(dec, dm, &ol, (const unsigned char*)d->valuestring, bl) == 0 && ol > 0) {
                    ESP_LOGI(TAG, "TTS %d bytes %s", (int)ol,
                             fmt ? fmt->valuestring : "");
                    set_resp(WS_RESP_TTS_AUDIO, NULL, dec, (int)ol);
                } else free(dec);
            }
        }
    } else if (!strcmp(ts, "tts_audio_start")) {
        /* 新协议(2026-08): 声明音频格式, 后续 chunk 为 base64 裸 PCM (无 WAV 头) */
        cJSON *sr = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *fmt = cJSON_GetObjectItem(root, "format");
        if (sr && cJSON_IsNumber(sr))
            g_tts_wav_sr = sr->valueint;
        g_tts_wav_bits = 16;
        g_tts_hdr_ok = true;  /* 让播放侧按流式 PCM 处理, 否则会被当 16kHz 播慢 */
        cJSON *session = cJSON_GetObjectItem(root, "session_id");
        cJSON *realtime = cJSON_GetObjectItem(root, "realtime_session_id");
        cJSON *turn = cJSON_GetObjectItem(root, "turn_id");
        cJSON *generation = cJSON_GetObjectItem(root, "generation_id");
        cJSON *seq = cJSON_GetObjectItem(root, "seq");
        int sess = (session && cJSON_IsNumber(session)) ? session->valueint : 0;
        int gen = (generation && cJSON_IsNumber(generation)) ? generation->valueint : 0;
        int seqn = (seq && cJSON_IsNumber(seq)) ? seq->valueint : 0;
        portENTER_CRITICAL(&g_tts_flow_mux);
        tts_flow_state_t *f = flow_alloc(sess, gen, seqn);
        f->valid = true;
        f->stream_ended = false;
        f->playback_started = false;
        f->playback_finished = false;
        f->buffer_dirty = false;
        f->buffer_rx_ack = false;
        f->pending_queued_ms = 0;
        f->last_buffer_sent_tick = 0;
        f->session_id = sess;
        f->generation_id = gen;
        f->seq = seqn;
        if (realtime && cJSON_IsString(realtime) && realtime->valuestring)
            strlcpy(f->realtime_session_id, realtime->valuestring,
                    sizeof(f->realtime_session_id));
        if (turn && cJSON_IsString(turn) && turn->valuestring)
            strlcpy(f->turn_id, turn->valuestring, sizeof(f->turn_id));
        portEXIT_CRITICAL(&g_tts_flow_mux);
        g_tts_chunk_count = 0;
        g_tts_last_chunk_seq = -1;
        g_tts_missing_chunks = 0;
        g_tts_decode_failures = 0;
        g_tts_stream_bytes = 0;
        g_tts_stream_start_tick = xTaskGetTickCount();
        g_tts_last_chunk_tick = 0;
        g_tts_max_chunk_gap_ms = 0;
        g_tts_slow_chunk_gaps = 0;
        int server_wait_ms = -1;
        if (g_last_text_delta_tick != 0 && turn && cJSON_IsString(turn) &&
            !strcmp(g_last_text_delta_turn, turn->valuestring)) {
            server_wait_ms = (int)((g_tts_stream_start_tick - g_last_text_delta_tick) *
                                   portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "TTS stream start: %dHz %s, server_wait_after_text=%dms",
                 g_tts_wav_sr, fmt ? fmt->valuestring : "?", server_wait_ms);
    } else if (!strcmp(ts, "tts_audio_chunk")) {
        cJSON *d = cJSON_GetObjectItem(root, "data");
        cJSON *sr = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *fmt = cJSON_GetObjectItem(root, "format");
        cJSON *chunk_seq = cJSON_GetObjectItem(root, "chunk_seq");
        int chunk_seqn = (chunk_seq && cJSON_IsNumber(chunk_seq))
                             ? chunk_seq->valueint : -1;
        if (chunk_seq && cJSON_IsNumber(chunk_seq)) {
            int got = chunk_seqn;
            if (g_tts_last_chunk_seq >= 0 && got != g_tts_last_chunk_seq + 1) {
                if (got > g_tts_last_chunk_seq + 1) {
                    int missing = got - g_tts_last_chunk_seq - 1;
                    g_tts_missing_chunks += missing;
                    ESP_LOGE(TAG, "TTS chunk gap: expected=%d got=%d, missing=%d (total=%d)",
                             g_tts_last_chunk_seq + 1, got, missing,
                             g_tts_missing_chunks);
                } else {
                    ESP_LOGW(TAG, "TTS chunk duplicate/out-of-order: last=%d got=%d",
                             g_tts_last_chunk_seq, got);
                }
            }
            if (got > g_tts_last_chunk_seq)
                g_tts_last_chunk_seq = got;
        }
        if (sr && cJSON_IsNumber(sr) && sr->valueint > 0 &&
            fmt && cJSON_IsString(fmt) && !strcmp(fmt->valuestring, "pcm_s16le")) {
            g_tts_wav_sr = sr->valueint;
            g_tts_wav_bits = 16;
            g_tts_hdr_ok = true;
        }
        if (d && cJSON_IsString(d) && d->valuestring) {
            int bl = strlen(d->valuestring), dm = (bl+2)/4*3+10;
            uint8_t *dec = heap_caps_malloc(dm, MALLOC_CAP_SPIRAM); if (!dec) dec = malloc(dm);
            if (dec) { size_t ol = 0;
                if (mbedtls_base64_decode(dec, dm, &ol, (const unsigned char*)d->valuestring, bl) == 0 && ol > 0) {
                    TickType_t now = xTaskGetTickCount();
                    int chunk_gap_ms = g_tts_last_chunk_tick
                                           ? (int)((now - g_tts_last_chunk_tick) *
                                                   portTICK_PERIOD_MS)
                                           : 0;
                    g_tts_last_chunk_tick = now;
                    if (chunk_gap_ms > g_tts_max_chunk_gap_ms)
                        g_tts_max_chunk_gap_ms = chunk_gap_ms;
                    if (chunk_gap_ms > 80) g_tts_slow_chunk_gaps++;
                    g_tts_chunk_count++;
                    g_tts_stream_bytes += ol;
                    if (g_tts_chunk_count == 1 || g_tts_chunk_count % 25 == 0)
                        ESP_LOGI(TAG,
                                 "TTS chunks: %d, latest=%d bytes (~%dms), rx_gap=%dms",
                                 g_tts_chunk_count, (int)ol,
                                 g_tts_wav_sr > 0
                                     ? (int)((int64_t)ol * 1000 /
                                             (g_tts_wav_sr * 2))
                                     : 0,
                                 chunk_gap_ms);
                    set_resp(WS_RESP_TTS_AUDIO, NULL, dec, (int)ol);
                } else {
                    g_tts_decode_failures++;
                    ESP_LOGE(TAG, "TTS base64 decode failed: chunk_seq=%d, failures=%d",
                             chunk_seqn, g_tts_decode_failures);
                    free(dec);
                }
            } else {
                g_tts_decode_failures++;
                ESP_LOGE(TAG, "TTS decode allocation failed: need=%d, chunk_seq=%d, failures=%d",
                         dm, chunk_seqn, g_tts_decode_failures);
            }
        }
    } else if (!strcmp(ts, "tts_audio_end")) {
        portENTER_CRITICAL(&g_tts_flow_mux);
        tts_flow_state_t *f = flow_cur();
        if (f) f->stream_ended = true;
        portEXIT_CRITICAL(&g_tts_flow_mux);
        int rx_ms = g_tts_stream_start_tick
                        ? (int)((xTaskGetTickCount() - g_tts_stream_start_tick) *
                                portTICK_PERIOD_MS)
                        : 0;
        int audio_ms = g_tts_wav_sr > 0
                           ? (int)((uint64_t)g_tts_stream_bytes * 1000ULL /
                                   ((uint64_t)g_tts_wav_sr * 2ULL))
                           : 0;
        ESP_LOGI(TAG,
                 "TTS stream end: chunks=%d, audio=%dms, rx=%dms, rate=%.2fx, "
                 "max_gap=%dms, slow_gaps=%d, missing=%d, decode_failed=%d",
                 g_tts_chunk_count, audio_ms, rx_ms,
                 rx_ms > 0 ? (double)audio_ms / (double)rx_ms : 0.0,
                 g_tts_max_chunk_gap_ms, g_tts_slow_chunk_gaps,
                 g_tts_missing_chunks, g_tts_decode_failures);
    } else if (!strcmp(ts, "tts_audio_failed") || !strcmp(ts, "tts_failed")) {
        portENTER_CRITICAL(&g_tts_flow_mux);
        flow_mark_all_ended();
        portEXIT_CRITICAL(&g_tts_flow_mux);
        cJSON *e = cJSON_GetObjectItem(root, "error");
        ESP_LOGW(TAG, "TTS failed: %s", e ? e->valuestring : "?");
    } else if (!strcmp(ts, "tts_audio_cancelled")) {
        portENTER_CRITICAL(&g_tts_flow_mux);
        flow_mark_all_ended();
        portEXIT_CRITICAL(&g_tts_flow_mux);
        ESP_LOGI(TAG, "TTS cancelled");
    } else if (!strcmp(ts, "tts_skip")) {
        ESP_LOGI(TAG, "TTS skip");
    } else if (!strcmp(ts, "stop_playback")) {
        ESP_LOGI(TAG, "STOP"); set_resp(WS_RESP_STOP_PLAYBACK, NULL, NULL, 0);
    } else if (!strcmp(ts, "asr_final")) {
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        if (tx && cJSON_IsString(tx)) ESP_LOGI(TAG, "ASR: %s", tx->valuestring);
    } else if (!strcmp(ts, "llm_delta") || !strcmp(ts, "text_delta")) {
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        cJSON *turn = cJSON_GetObjectItem(root, "turn_id");
        if (tx && cJSON_IsString(tx)) {
            g_last_text_delta_tick = xTaskGetTickCount();
            if (turn && cJSON_IsString(turn) && turn->valuestring)
                strlcpy(g_last_text_delta_turn, turn->valuestring,
                        sizeof(g_last_text_delta_turn));
            set_resp(WS_RESP_LLM_DELTA, tx->valuestring, NULL, 0);
        }
    } else if (!strcmp(ts, "turn_end")) {
        portENTER_CRITICAL(&g_tts_flow_mux);
        flow_mark_all_ended();
        portEXIT_CRITICAL(&g_tts_flow_mux);
        cJSON *s = cJSON_GetObjectItem(root, "status");
        ESP_LOGI(TAG, "Turn: %s", s ? s->valuestring : "?"); set_resp(WS_RESP_TURN_END, NULL, NULL, 0);
    } else if (!strcmp(ts, "error")) {
        cJSON *d = cJSON_GetObjectItem(root, "detail");
        ESP_LOGW(TAG, "Err: %s", d ? d->valuestring : "?"); set_resp(WS_RESP_ERROR, d ? d->valuestring : "?", NULL, 0);
    } else if (!strcmp(ts, "message")) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (msg) {
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (content && cJSON_IsString(content))
                ESP_LOGI(TAG, "MSG[%s]: %s", role ? role->valuestring : "?", content->valuestring);

            /* 实时语音完成后，情绪字段随user message一起返回。独立缓存，
             * 不占用g_resp_type，避免覆盖TTS/turn_end等控制事件。 */
            if (role && cJSON_IsString(role) && !strcmp(role->valuestring, "user")) {
                cJSON *id = cJSON_GetObjectItem(msg, "id");
                cJSON *code = cJSON_GetObjectItem(msg, "emotion_label_code");
                cJSON *name = cJSON_GetObjectItem(msg, "emotion_label_name");
                cJSON *confidence = cJSON_GetObjectItem(msg, "emotion_confidence");
                cJSON *status = cJSON_GetObjectItem(msg, "emotion_recognition_status");
                bool has_emotion = (code && cJSON_IsString(code)) ||
                                   (name && cJSON_IsString(name)) ||
                                   (status && cJSON_IsString(status));
                if (has_emotion &&
                    xSemaphoreTake(g_resp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    memset(&g_emotion, 0, sizeof(g_emotion));
                    g_emotion.message_id = (id && cJSON_IsNumber(id)) ? id->valueint : 0;
                    g_emotion.confidence = (confidence && cJSON_IsNumber(confidence))
                                               ? confidence->valueint : -1;
                    if (code && cJSON_IsString(code) && code->valuestring)
                        strlcpy(g_emotion.label_code, code->valuestring,
                                sizeof(g_emotion.label_code));
                    if (name && cJSON_IsString(name) && name->valuestring)
                        strlcpy(g_emotion.label_name, name->valuestring,
                                sizeof(g_emotion.label_name));
                    if (status && cJSON_IsString(status) && status->valuestring)
                        strlcpy(g_emotion.recognition_status, status->valuestring,
                                sizeof(g_emotion.recognition_status));
                    g_emotion_pending = true;
                    xSemaphoreGive(g_resp_mutex);
                }
            }
        }
    } else if (!strcmp(ts, "timing")) {
        cJSON *phases = cJSON_GetObjectItem(root, "phases");
        if (phases) {
            cJSON *asr  = cJSON_GetObjectItem(phases, "ui_asr");
            cJSON *gen = cJSON_GetObjectItem(phases, "ui_generate");
            ESP_LOGI(TAG, "Timing: ASR=%.1fs LLM=%.1fs",
                     asr ? asr->valuedouble/1000.0 : 0.0,
                     gen ? gen->valuedouble/1000.0 : 0.0);
        }
    }
    cJSON_Delete(root);
}

/* ===================================================================
 *  TTS cross-message accumulation
 * =================================================================== */
#define TTS_BUF_MAX 786432
static char *g_tts_buf = NULL;
static int   g_tts_len = 0;
static bool  g_tts_active = false;
static char *g_json_buf = NULL;
static int   g_json_len = 0;

/* 流式解码：8字符对齐→6字节→永远偶数，16-bit天然对齐 */

static bool is_tts_start(const char *s, int len) {
    if (len < 30 || s[0] != '{') return false;
    /* 这里只匹配旧协议的单个巨大tts_audio JSON。新协议的
     * tts_audio_start/chunk/end必须逐条交给parse_json处理。 */
    return (strstr(s, "\"type\":\"tts_audio\"") ||
            strstr(s, "\"type\": \"tts_audio\""));
}
static bool is_json_start(const char *s, int len) {
    return (len > 15 && s[0] == '{' && s[1] == '"' && s[2] == 't' && s[3] == 'y' && s[4] == 'p' && s[5] == 'e');
}
static inline bool is_base64_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

/* New streaming chunks carry their own PCM format.  Read it from the JSON
 * prefix before decoding data, so playback remains correct even when the
 * optional tts_audio_start message was missed or fragmented. */
static void update_tts_format_from_prefix(void) {
    char *fmt = strstr(g_tts_buf, "\"format\"");
    char *sr = strstr(g_tts_buf, "\"sample_rate\"");
    if (!fmt || !sr || !strstr(fmt, "pcm_s16le")) return;

    char *colon = strchr(sr, ':');
    if (!colon) return;
    long value = strtol(colon + 1, NULL, 10);
    if (value < 8000 || value > 96000) return;

    bool changed = g_tts_wav_sr != (int)value || g_tts_wav_bits != 16;
    g_tts_wav_sr = (int)value;
    g_tts_wav_bits = 16;
    g_tts_hdr_ok = true;
    if (changed)
        ESP_LOGI(TAG, "TTS stream chunk format: %ldHz pcm_s16le", value);
}

static void try_stream_tts(void) {
    if (!g_tts_buf || !g_tts_active) return;

    if (g_tts_data_ofs < 0) {
        char *p = strstr(g_tts_buf, "\"data\"");
        if (!p) return;
        p = strchr(p + 6, '"');
        if (!p) return;
        p++;
        g_tts_data_ofs = (int)(p - g_tts_buf);
        g_tts_decoded_ofs = g_tts_data_ofs;
        update_tts_format_from_prefix();
        /* streaming started */
    }

    while (g_tts_decoded_ofs + 8 <= g_tts_len) {
        int limit = g_tts_len - g_tts_decoded_ofs;
        if (limit > 8192) limit = 8192;  /* ~128ms/chunk，减少DMA断流 */
        limit = (limit / 8) * 8;
        if (limit < 8) break;

        int valid = g_tts_decoded_ofs;
        while (valid < g_tts_decoded_ofs + limit && is_base64_char(g_tts_buf[valid])) valid++;
        int declen = ((valid - g_tts_decoded_ofs) / 8) * 8;
        if (declen < 8) break;

        int raw_max = (declen / 4) * 3 + 4;
        uint8_t *raw = malloc(raw_max);
        if (!raw) break;
        size_t raw_len = 0;
        if (mbedtls_base64_decode(raw, raw_max, &raw_len,
            (const unsigned char *)(g_tts_buf + g_tts_decoded_ofs), declen) != 0) {
            free(raw); break;
        }
        g_tts_decoded_ofs += declen;

        int offset = 0;
        if (raw_len >= 44 && memcmp(raw, "RIFF", 4) == 0) {
            g_tts_wav_sr = raw[24]|(raw[25]<<8)|(raw[26]<<16)|(raw[27]<<24);
            g_tts_wav_bits = raw[34]|(raw[35]<<8);
            g_tts_hdr_ok = true;
            offset = 44;
            ESP_LOGI(TAG, "TTS stream WAV: %dHz %dbit", g_tts_wav_sr, g_tts_wav_bits);
        }

        int pcm_size = (int)raw_len - offset;
        if (pcm_size > 0) {
            uint8_t *chunk = malloc(pcm_size);
            if (chunk) {
                memcpy(chunk, raw + offset, pcm_size);
                tts_audio_item_t item = {
                    .data = chunk,
                    .len = pcm_size,
                    .sample_rate = g_tts_wav_sr,
                    .bits = g_tts_wav_bits,
                };
                if (!tts_queue_send(&item)) free(chunk);
            }
        }
        free(raw);
    }
}

bool realtime_ws_get_tts_stream_format(int *sr, int *bits) {
    if (g_tts_play_sr <= 0 || g_tts_play_bits <= 0) return false;
    *sr = g_tts_play_sr; *bits = g_tts_play_bits;
    return true;
}

static void flush_tts(void) {
    if (!g_tts_active || !g_tts_buf || g_tts_len < 50) { if (g_tts_buf) free(g_tts_buf); g_tts_buf = NULL; g_tts_len = 0; g_tts_active = false; return; }
    try_stream_tts();
    if (!g_tts_hdr_ok)
        parse_json(g_tts_buf, g_tts_len);
    free(g_tts_buf); g_tts_buf = NULL; g_tts_len = 0; g_tts_active = false;
    g_tts_data_ofs = -1; g_tts_decoded_ofs = 0;
}

/* ===================================================================
 *  Processing task
 * =================================================================== */
static void proc_task(void *arg) {
    ws_msg_t msg;
    while (1) {
        if (xQueueReceive(g_msg_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        g_last_rx_tick = xTaskGetTickCount();

        /* TTS base64 体积很大，输出到串口会阻塞接收并造成播放欠载。
         * 同时覆盖紧凑JSON、带空格JSON以及后续WebSocket分片。 */
        if (msg.is_binary) {
            /* 二进制帧静默 */
        } else {
            const char *s = (const char *)msg.data;
            if (g_tts_active || g_json_buf || is_tts_start(s, msg.len) ||
                strstr(s, "\"type\":\"tts_audio") ||
                strstr(s, "\"type\": \"tts_audio")) {
                /* TTS消息及其续接分片静默，解析时只打印格式摘要 */
            } else if (msg.len <= 400) {
                ESP_LOGI(TAG, "%.*s", msg.len, s);
            } else {
                ESP_LOGI(TAG, "%.400s...[+%d]", s, msg.len - 400);
            }
        }

        if (msg.is_binary) {
            if (msg.data) { uint8_t *cp = malloc(msg.len); if (cp) { memcpy(cp, msg.data, msg.len); set_resp(WS_RESP_TTS_AUDIO, NULL, cp, msg.len); } }
        } else {
            const char *s = (const char *)msg.data;
            if (is_tts_start(s, msg.len)) {
                flush_tts(); /* g_tts_hdr_ok 不重置，等新WAV头覆盖 */
                g_tts_buf = malloc(msg.len+1);
                if (g_tts_buf) { memcpy(g_tts_buf, s, msg.len); g_tts_len = msg.len; g_tts_buf[g_tts_len] = 0; g_tts_active = true; }
                try_stream_tts();
            } else if (g_tts_active) {
                if (g_tts_len + msg.len > TTS_BUF_MAX) {
                    ESP_LOGW(TAG, "TTS buffer overflow (%d), discarding", g_tts_len + msg.len);
                    free(g_tts_buf); g_tts_buf = NULL; g_tts_len = 0; g_tts_active = false;
                } else {
                    char *nb = realloc(g_tts_buf, g_tts_len + msg.len + 1);
                    if (nb) { g_tts_buf = nb; memcpy(g_tts_buf + g_tts_len, s, msg.len); g_tts_len += msg.len; g_tts_buf[g_tts_len] = 0; }
                    try_stream_tts();
                    if (g_tts_buf && g_tts_len > 2 && g_tts_buf[g_tts_len-1] == '}') {
                        cJSON *test = cJSON_Parse(g_tts_buf);
                        if (test) { cJSON_Delete(test); flush_tts(); }
                    }
                }
            }
            if (g_tts_active && is_json_start(s, msg.len) && !is_tts_start(s, msg.len)) flush_tts();
            if (!g_tts_active) {
                cJSON *test = cJSON_Parse(s);
                if (test) {
                    if (g_json_buf) {
                        free(g_json_buf); g_json_buf = NULL; g_json_len = 0;
                    }
                    cJSON_Delete(test);
                    parse_json(s, msg.len);
                } else if (s[0] == '{' || g_json_buf) {
                    /* 新JSON片段 或 续接已有片段 */
                    char *nb = realloc(g_json_buf, g_json_len + msg.len + 1);
                    if (nb) {
                        g_json_buf = nb;
                        memcpy(g_json_buf + g_json_len, s, msg.len);
                        g_json_len += msg.len;
                        g_json_buf[g_json_len] = '\0';
                        /* 尝试解析累积的完整JSON */
                        cJSON *tt = cJSON_Parse(g_json_buf);
                        if (tt) {
                            cJSON_Delete(tt);
                            parse_json(g_json_buf, g_json_len);
                            free(g_json_buf); g_json_buf = NULL; g_json_len = 0;
                        }
                    }
                }
            }
        }
        free(msg.data);
    }
    vTaskDelete(NULL);
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
int realtime_ws_init(const char *url) {
    if (!url) return -1;
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
    xTaskCreate(proc_task, "ws_proc", 8192, NULL, 6, NULL);
    /* 播放水位/finished反馈也要及时送出，否则服务端会暂停后续TTS窗口。 */
    xTaskCreate(ws_tx_task, "ws_tx", 8192, NULL, 5, &g_ws_tx_task_handle);
    return ws_url ? 0 : -1;
}

/* 鉴权+连接独立任务: esp_http_client 的 TLS+JSON 吃栈, 不能在 main 任务里跑 */
static void ws_connect_task(void *arg)
{
    if (!WiFiWaitForTimeSync(10000)) {
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

/* 权威连接检查: 用客户端内部状态, 避免断连后缓存标志 stale 导致反复调发送 */
static bool ws_sendable(void)
{
    return ws_connected && ws_client &&
           esp_websocket_client_is_connected(ws_client);
}
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

int realtime_ws_send_text(const char *text) {
    if (!text) return -1;
    int r = -1;
    if (g_ws_tx_mutex && xSemaphoreTake(g_ws_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    if (ws_sendable())
        r = esp_websocket_client_send_text(ws_client, text, strlen(text), pdMS_TO_TICKS(5000));
    if (g_ws_tx_mutex) xSemaphoreGive(g_ws_tx_mutex);
    return r > 0 ? 0 : -1;
}

static int tts_queued_ms(size_t queued_bytes)
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
