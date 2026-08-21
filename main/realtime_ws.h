/*
 * realtime_ws.h — WebSocket实时音频通信管理器 (WebSocket realtime audio communication manager)
 *
 * 协议(Protocol) — 按硬件集成文档：
 *   1. 连接WSS (Connect WSS)
 *   2. 发送hello JSON（元数据，不含音频）
 *   3. 等待服务端返回"ready"
 *   4. 以二进制帧发送PCM S16LE 16kHz单声道 (640采样点=40ms)
 *   5. 接收JSON响应: tts_audio, asr_final, llm_delta, stop_playback, turn_end
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 *  服务端响应类型 (Response types from server)
 * =================================================================== */
typedef enum {
    WS_RESP_NONE = 0,
    WS_RESP_TTS_AUDIO,      // 待播放的base64音频 — base64 audio to play
    WS_RESP_STOP_PLAYBACK,  // 立即停止当前播放 — stop current playback immediately
    WS_RESP_ASR_FINAL,      // 识别到的用户语音→暂停发送 — recognized user speech → pause sending
    WS_RESP_LLM_DELTA,      // LLM增量回复文本 — LLM response text (incremental)
    WS_RESP_TURN_END,       // 对话轮次结束 — conversation turn ended
    WS_RESP_ERROR,          // 服务端错误 — server error
} ws_resp_type_t;

/* 用户语音对应的服务端情绪识别结果。confidence=-1表示服务端未给出置信度。 */
typedef struct {
    int message_id;
    char label_code[24];
    char label_name[64];
    int confidence;
    char recognition_status[32];
} realtime_ws_emotion_t;

/** 获取最新解析的服务端响应 — Get latest parsed server response */
ws_resp_type_t realtime_ws_get_response(char **out_text, uint8_t **out_audio, int *out_audio_len);

/** 清除缓冲的响应 — Clear buffered response */
void realtime_ws_clear_response(void);

/** 检查服务端是否已发送"ready" */
bool realtime_ws_is_ready(void);

/** 距离最后一次收到数据的毫秒数（用于检测服务端忙） */
int realtime_ws_ms_since_last_rx(void);

/** 非阻塞获取最新情绪结果；返回true时out有效并消费该结果。 */
bool realtime_ws_get_emotion(realtime_ws_emotion_t *out);

/* ===================================================================
 *  连接管理 (Connection)
 * =================================================================== */
int  realtime_ws_init(const char *url);
void realtime_ws_connect(void);
void realtime_ws_disconnect(void);
bool realtime_ws_is_connected(void);
bool realtime_ws_wait_connected(int timeout_ms);
void realtime_ws_deinit(void);

/** 获取TTS音频（非阻塞），返回true表示有数据。out_* 返回该音频所属流的ID */
bool realtime_ws_get_tts_audio(uint8_t **out_data, int *out_len,
                               int *out_session_id, int *out_generation_id, int *out_seq);

/** 当前等待播放的TTS音频块数量 */
int realtime_ws_get_tts_queue_depth(void);

/** 队列中剩余TTS音频的可播时长（ms），用于预缓冲判断 */
int realtime_ws_tts_queued_ms(void);

/** 最近一个TTS流是否已收到 tts_audio_end */
bool realtime_ws_tts_current_stream_ended(void);

/** 是否还有尚未结束的音频流在途（服务端仍在合成/下发后续句子） */
bool realtime_ws_tts_has_pending_stream(void);

/** 获取流式解码的WAV格式（sample_rate, bits） */
bool realtime_ws_get_tts_stream_format(int *sample_rate, int *bits);

/** 通知服务端首个TTS采样已开始播放（服务端据此启动播放流控） */
void realtime_ws_tts_playback_started(int session_id, int generation_id, int seq);

/** 上报当前尚未播放的TTS队列时长，释放服务端发送窗口 */
void realtime_ws_tts_playback_buffer(int session_id, int generation_id, int seq);

/** 播放队列从一个seq切换到下一个seq时，立即结束前一个流的反馈 */
bool realtime_ws_tts_finish_stream(int session_id, int generation_id, int seq);

/** 已结束的TTS流且本地队列为空时发送playback_finished */
bool realtime_ws_tts_finish_if_drained(void);

/** 停止/打断播放时发送interrupted playback_finished */
void realtime_ws_tts_playback_interrupted(void);

/* ===================================================================
 *  发送 (Send)
 * =================================================================== */

/** 发送hello，包含正确的mime_type格式 — Send hello with correct mime_type format */
int realtime_ws_send_hello(void);

/** 发送原始PCM S16LE 16kHz单声道二进制帧 — Send raw PCM S16LE binary frame */
int realtime_ws_send_audio(const int16_t *pcm_data, int num_samples);

int realtime_ws_send_text(const char *text);

#ifdef __cplusplus
}
#endif
