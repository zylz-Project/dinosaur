/*
 * realtime_ws_protocol.c — WebSocket 实时音频通道（收包与解析层）
 *
 * 从 realtime_ws.c 拆出的"消息处理"半边：ws_proc_task 消费 g_msg_queue，
 * 负责 TTS 大 JSON 的跨帧拼接与流式 base64 解码（try_stream_tts/flush_tts），
 * parse_json 按 type 分发（tts_audio 双协议：旧 WAV base64 / 新 start-chunk-end、
 * llm_delta、turn_end、情绪字段…），set_resp 把解码结果送入响应/TTS 队列。
 * 协议解析与流式解码逻辑逐字保留（拆文件≠改逻辑）。
 * 连接生命周期在 realtime_ws.c，TTS 流控反馈在 realtime_ws_tts.c。
 */

#include "realtime_ws.h"
#include "realtime_ws_internal.h"
#include "display.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "REALTIME_WS";

/* Response output */
static ws_resp_type_t g_resp_type = WS_RESP_NONE;
static char    *g_resp_text  = NULL;
static uint8_t *g_resp_audio = NULL;
static int      g_resp_audio_len = 0;
char    *g_llm_buf = NULL;
SemaphoreHandle_t g_resp_mutex = NULL;
static realtime_ws_emotion_t g_emotion = { .confidence = -1 };
static bool g_emotion_pending = false;
/* TTS 流式解码状态 (新协议 tts_audio_start/chunk 与旧协议 WAV 共用) */
static int  g_tts_data_ofs = -1;
static int  g_tts_decoded_ofs = 0;
static bool g_tts_hdr_ok = false;
int  g_tts_wav_sr = 0, g_tts_wav_bits = 0;  /* 生产端 PCM 格式，tts 层 tts_queued_ms 读取 */
static int  g_tts_chunk_count = 0;
static int  g_tts_last_chunk_seq = -1;
static int  g_tts_missing_chunks = 0;
static int  g_tts_decode_failures = 0;
static size_t g_tts_stream_bytes = 0;
static TickType_t g_tts_stream_start_tick = 0;
static TickType_t g_tts_last_chunk_tick = 0;
static int g_tts_max_chunk_gap_ms = 0;
static int g_tts_slow_chunk_gaps = 0;
static TickType_t g_last_text_delta_tick = 0;
static char g_last_text_delta_turn[TTS_ID_LEN] = "";
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
char *g_tts_buf = NULL;
int   g_tts_len = 0;
bool  g_tts_active = false;
char *g_json_buf = NULL;
int   g_json_len = 0;

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
void ws_proc_task(void *arg) {
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
