/*
 * realtime_ws_internal.h — realtime_ws 拆分后的内部共享声明（仅限三个实现文件）
 *
 * realtime_ws.c（连接生命周期）/ realtime_ws_protocol.c（收包解析）/
 * realtime_ws_tts.c（TTS 流控与发送）同属一个 component，本头文件只放
 * 跨文件共享的类型、常量与全局定义。组件外的调用方永远只 include
 * realtime_ws.h，不要 include 本文件。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "realtime_ws.h"

/* ---- 共享常量（原 realtime_ws.c 内的 #define，拆分后多处引用） ---- */
#define WS_MSG_QUEUE_LEN 512
#define TTS_QUEUE_LEN 1024
#define TTS_BUF_MAX 786432
#define TTS_ID_LEN 48
#define TTS_FLOW_SLOTS 16
#define WS_TX_QUEUE_LEN 64
#define TTS_BUFFER_DRAIN_REPORT_INTERVAL_MS 200

/* ---- 共享类型 ---- */
typedef struct { uint8_t *data; int len; bool is_binary; } ws_msg_t;

/* 每个队列条目携带所属流的 ID，播放侧据此回传正确的流反馈 */
typedef struct {
    uint8_t *data;
    int len;
    int sample_rate;
    int bits;
    int session_id;
    int generation_id;
    int seq;
} tts_audio_item_t;

typedef struct { char *json; } ws_tx_msg_t;

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

/* ---- realtime_ws.c（连接层）定义 ---- */
extern esp_websocket_client_handle_t ws_client;
extern volatile bool ws_connected;
extern volatile bool g_ready;
extern volatile TickType_t g_last_rx_tick;
extern QueueHandle_t g_msg_queue;

/* ---- realtime_ws_protocol.c（收包解析层）定义 ---- */
extern SemaphoreHandle_t g_resp_mutex;
extern char *g_llm_buf;          /* LLM 增量文本累积；断连清理时由连接层释放 */
extern char *g_tts_buf;          /* 旧协议大 JSON 跨帧累积缓冲（同上） */
extern int   g_tts_len;
extern bool  g_tts_active;
extern char *g_json_buf;         /* 分片 JSON 重组缓冲（同上） */
extern int   g_json_len;
extern int  g_tts_wav_sr, g_tts_wav_bits;  /* 生产端 PCM 格式（由解析层更新） */
void ws_proc_task(void *arg);

/* ---- realtime_ws_tts.c（TTS 流控与发送层）定义 ---- */
extern QueueHandle_t g_tts_queue;
extern bool g_tts_queue_with_caps;
extern tts_flow_state_t g_tts_flows[TTS_FLOW_SLOTS];
extern int g_tts_cur_flow;
extern size_t g_tts_queued_bytes;
extern portMUX_TYPE g_tts_flow_mux;
extern volatile uint32_t g_ctrl_tx_ok;
extern volatile uint32_t g_ctrl_tx_failed;
extern volatile uint32_t g_buffer_tx_ok;
extern volatile uint32_t g_buffer_tx_failed;
extern QueueHandle_t g_tx_queue;
extern SemaphoreHandle_t g_ws_tx_mutex;
extern TaskHandle_t g_ws_tx_task_handle;
void ws_tx_task(void *arg);

/* ---- TTS 流槽位与队列助手（除 tts_queue_* 外，调用方需已持有
 *      g_tts_flow_mux；契约与拆分前一致） ---- */
tts_flow_state_t *flow_find(int session_id, int generation_id, int seq);
tts_flow_state_t *flow_alloc(int session_id, int generation_id, int seq);
tts_flow_state_t *flow_cur(void);
void flow_mark_all_ended(void);
bool tts_queue_send(tts_audio_item_t *item);
bool tts_queue_receive(tts_audio_item_t *item);
int tts_queued_ms(size_t queued_bytes);
