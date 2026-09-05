/*
 * chat.cc — 小熊猫 LLM 实时语音对话模块
 *
 * 移植自 chat_ws_espidf/main.c + realtime_ws.c，针对小熊猫板做了裁剪：
 *   - 去掉 ESP-SR/AFE（服务端自己做 VAD）、去掉 OLED、去掉独立音量键
 *   - 麦克风走板载 ES8311（48kHz 双工，AudioReadMic48k 读取后 3:1 抽取到 16k）
 *   - TTS 播放走 AudioWritePcm48k，同时按音频包络驱动舵机动作
 *   - 服务端“message”里的情绪结果驱动动作偏向
 */

#include "chat.h"
#include "audio.h"
#include "audio_tone.h"
#include "config.h"
#include "realtime_ws.h"
#include "servo.h"
#include "wifi.h"
#if ENABLE_AUTO_RUN
#include "auto_run.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "chat";

/* ===================================================================
 *  配置
 * =================================================================== */
#define CHAT_WS_URL "wss://www.mmemoryy.xyz/api-proxy/api/v1/realtime/browser?session_id=141"

#ifndef CHAT_ENABLE
#define CHAT_ENABLE 1
#endif

#define MIC_SAMPLE_RATE 48000   /* 板载 ES8311 以 48k 采样 */
#define CHAT_SAMPLE_RATE 16000  /* 上行发送给服务器的采样率 */
#define WS_FRAME_SAMPLES 640    /* 40ms @ 16kHz = 1280 字节 */
#define DECIMATE 3              /* 48k -> 16k 抽取系数 */

/* 实测服务端初始下发较慢(首个TTS流会先等~1s以上), 且到包存在100ms级抖动。
 * 恐龙板实测 rate 仅约0.74x时就会周期性欠载产生"TTS gap"。增大预缓冲:
 *   - 吸收慢速期/抖动, 避免队列排空
 *   - 上报给服务端的 queued_ms 更大, 促使服务端加快下发(服务端按缓冲水位放量)
 * 代价是开始播放前多等~1.2s, 换来连续不断音。 */
#define TTS_PREBUFFER_MS 1200
#define TTS_PREBUFFER_TIMEOUT_MS 3000
/* 先发送playback_started释放服务端窗口，再补足稳态抖动缓存。首块40ms
 * 已从软件队列取出但尚未播放，因此队列目标为3000-40=2960ms。 */
#define TTS_STEADY_BUFFER_MS 3000
#define TTS_STEADY_QUEUE_MS (TTS_STEADY_BUFFER_MS - 40)
#define TTS_STEADY_WAIT_TIMEOUT_MS 3000
#define TTS_GAP_LOG_MS 200

/* ===================================================================
 *  状态
 * =================================================================== */
typedef enum {
    CHAT_IDLE,
    CHAT_LISTENING, /* 发送麦克风音频 */
    CHAT_PLAYING,   /* 播放 TTS，暂停发送防回声 */
} chat_state_t;

/* g_state 跨任务读写契约：chat_response 是唯一写者，chat_feed / chat_motion
 * 只读。volatile 保证读者不缓存旧值；读到"上一拍"的状态无害（本轮循环会
 * 重读），因此不加锁——50Hz 实时路径锁的开销和优先级反转风险大于收益。 */
static volatile chat_state_t g_state = CHAT_IDLE;
static volatile bool g_chat_on = false;
static volatile bool g_streaming = false;
static volatile bool g_ready_prompt_pending = false;

static int16_t *g_ws_frame_buf = nullptr; /* 640 samples */
static int g_ws_frame_pos = 0;

/* 最近一次麦克风发送的间隔 */
#define FEED_READ_SAMPLES (WS_FRAME_SAMPLES * DECIMATE) /* 1920 @48k = 40ms */

/* ===================================================================
 *  动作：音频包络 + 情绪
 * =================================================================== */
static volatile float g_chat_level = 0.0f;   /* 平滑响度 0..1 */
static volatile float g_chat_attack = 0.0f;  /* 起始能量 0..1 */
static volatile bool  g_chat_playing = false;

/* 情绪驱动 */
typedef struct {
    float speed;     /* 尾巴摆动速度倍率 */
    float head_bias; /* 头部角度偏移（正=抬/偏） */
    float lr_bias;   /* 尾巴左右偏移 */
    float ud_bias;   /* 尾巴上下偏移 */
    bool  shake;     /* 是否摇头 */
} emo_bias_t;
/* g_emo_target 同样是"单写多读"：apply_emotion（chat_response 任务）写，
 * chat_motion 任务读。chat_get_emo_snapshot() 是给跨任务读者的取值口。 */
static emo_bias_t g_emo_target = {1.0f, 0, 0, 0, false};

chat_emo_bias_t chat_get_emo_snapshot(void)
{
    chat_emo_bias_t out;
    out.speed = g_emo_target.speed;
    out.head_bias = g_emo_target.head_bias;
    out.lr_bias = g_emo_target.lr_bias;
    out.ud_bias = g_emo_target.ud_bias;
    out.shake = g_emo_target.shake;
    return out;
}

static void apply_emotion(const char *label)
{
    emo_bias_t t = {1.0f, 0, 0, 0, false};
    if (!label) { g_emo_target = t; return; }
    if (strstr(label, "happy") || strstr(label, "joy") || strstr(label, "开心") || strstr(label, "高兴")) {
        t.speed = 1.6f; t.head_bias = 7; t.ud_bias = -4;
    } else if (strstr(label, "sad") || strstr(label, "悲伤") || strstr(label, "难过")) {
        t.speed = 0.6f; t.head_bias = -8; t.ud_bias = 10;
    } else if (strstr(label, "surprised") || strstr(label, "惊讶") || strstr(label, "amaze")) {
        t.speed = 1.2f; t.head_bias = 12;
    } else if (strstr(label, "angry") || strstr(label, "愤怒") || strstr(label, "生气")) {
        t.speed = 1.8f; t.head_bias = -4; t.shake = true;
    } else if (strstr(label, "fear") || strstr(label, "害怕") || strstr(label, "恐惧")) {
        t.speed = 0.8f; t.head_bias = -4; t.ud_bias = 8;
    } else if (strstr(label, "calm") || strstr(label, "平静") || strstr(label, "neutral") || strstr(label, "中性")) {
        t.speed = 0.8f;
    }
    g_emo_target = t;
    ESP_LOGI(TAG, "emotion -> speed=%.1f head=%+.0f shake=%d", (double)t.speed, (double)t.head_bias, t.shake ? 1 : 0);
}

/* ===================================================================
 *  TTS 播放 + 包络
 * =================================================================== */
/* 对话 TTS 路径的包络系数（与 audio.cc 的 Flash 路径不同：幅度基准 9000、
 * 快包络上行快下行慢、无 attack 低通——这些是实机调校值，见 audio_tone.cc） */
static const envelope_coef_t kChatCoef = {
    9000.0f, 0.48f, 0.12f, 0.035f, 1.08f, 3.2f, false};
static envelope_t chat_env_ = {0, 0, 0};

static void chat_envelope(const int16_t *pcm, int n)
{
    if (!pcm || n <= 0) return;
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += (uint32_t)std::abs((int)pcm[i]);
    float raw = (float)sum / (float)n;
    float onset = envelope_update(&chat_env_, raw, &kChatCoef);
    g_chat_level = chat_env_.slow;
    g_chat_attack = onset;
}

static void chat_play_chunk(const int16_t *pcm, int n)
{
    chat_envelope(pcm, n);
    AudioWritePcm48k(pcm, n);
}

/* 线性插值重采样到 48k */
static void chat_resample_and_play(const int16_t *pcm, int ns, int sr)
{
    if (sr <= 0 || ns <= 0) return;
    float ratio = (float)MIC_SAMPLE_RATE / (float)sr;
    int out_total = (int)(ns * ratio);
    const int chunk = 240;
    int16_t *out = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!out) return;
    for (int pos = 0; pos < out_total; pos += chunk) {
        int n = out_total - pos;
        if (n > chunk) n = chunk;
        for (int j = 0; j < n; j++) {
            float src = (float)(pos + j) / ratio;
            int idx = (int)src;
            float frac = src - (float)idx;
            int a = pcm[idx];
            int b = (idx + 1 < ns) ? pcm[idx + 1] : a;
            out[j] = (int16_t)((float)a + (float)(b - a) * frac);
        }
        chat_play_chunk(out, n);
    }
    heap_caps_free(out);
}

static void chat_play_tts(const uint8_t *data, int len)
{
    if (!data || len < 10) return;

    int ssr = 0, sbits = 0;
    if (realtime_ws_get_tts_stream_format(&ssr, &sbits) && ssr > 0) {
        int ns = len / (sbits / 8);
        if (ssr == MIC_SAMPLE_RATE) {
            chat_play_chunk((const int16_t *)data, ns);
        } else {
            chat_resample_and_play((const int16_t *)data, ns, ssr);
        }
        return;
    }

    if (len >= 44 && memcmp(data, "RIFF", 4) == 0) {
        int sr = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
        int ch = data[22] | (data[23] << 8);
        int bits = data[34] | (data[35] << 8);
        int dsz = data[40] | (data[41] << 8) | (data[42] << 16) | (data[43] << 24);
        const int16_t *pcm = (const int16_t *)(data + 44);
        int ns = dsz / (ch * (bits / 8));
        if (sr == MIC_SAMPLE_RATE) chat_play_chunk(pcm, ns);
        else chat_resample_and_play(pcm, ns, sr);
        return;
    }

    chat_resample_and_play((const int16_t *)data, len / 2, 16000);
}

/* ===================================================================
 *  Feed 任务：麦克风(48k) → 3:1 抽取 → 16k 帧 → WS
 * =================================================================== */
static void chat_feed_task(void *arg)
{
    int16_t *buf = (int16_t *)heap_caps_malloc(FEED_READ_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!buf) { vTaskDelete(NULL); return; }

    TickType_t last_connect_attempt = 0;
    int diag_tick = 0;
    uint32_t sent_ok = 0, sent_fail = 0;
    while (true) {
        if (!g_chat_on) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        /* 断线后周期性补连（覆盖鉴权任务失败/首次未连的情况） */
        if (!realtime_ws_is_connected() &&
            (xTaskGetTickCount() - last_connect_attempt) * portTICK_PERIOD_MS > 3000) {
            last_connect_attempt = xTaskGetTickCount();
            realtime_ws_connect();
        }

        /* 只有收到服务端 ready 后才提示用户并开放麦克风。这样连接/鉴权
         * 较慢时，提示音之前说的话不会被静默丢弃；提示音也不会被上传。 */
        if (g_ready_prompt_pending && realtime_ws_is_ready()) {
            g_ready_prompt_pending = false;
            ESP_LOGI(TAG, "Server ready — playing chat-ready prompt");
            AudioPlayChatReadyTone();
            if (g_chat_on) {
                g_state = CHAT_LISTENING;
                g_streaming = true;
                g_ws_frame_pos = 0;
                ESP_LOGI(TAG, "Ready prompt finished — microphone streaming");
            }
            continue;
        }

        if (g_state != CHAT_LISTENING || !g_streaming || !realtime_ws_is_ready()) {
            if (++diag_tick % 100 == 0)
                ESP_LOGI(TAG, "feed wait: state=%d streaming=%d ready=%d",
                         (int)g_state, g_streaming ? 1 : 0,
                         realtime_ws_is_ready() ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int got = AudioReadMic48k(buf, FEED_READ_SAMPLES);
        if (got <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int samples = got / (int)sizeof(int16_t);

        /* 3:1 抽取（先 3 点平均做简单低通） */
        for (int i = 0; i + 2 < samples; i += DECIMATE) {
            int32_t acc = (int32_t)buf[i] + (int32_t)buf[i + 1] + (int32_t)buf[i + 2];
            g_ws_frame_buf[g_ws_frame_pos++] = (int16_t)(acc / 3);
            if (g_ws_frame_pos >= WS_FRAME_SAMPLES) {
                int r = realtime_ws_send_audio(g_ws_frame_buf, WS_FRAME_SAMPLES);
                if (r == 0) sent_ok++; else sent_fail++;
                if (++diag_tick % 100 == 0) {
                    int16_t peak = 0;
                    for (int k = 0; k < WS_FRAME_SAMPLES; k++)
                        if (abs(g_ws_frame_buf[k]) > peak) peak = abs(g_ws_frame_buf[k]);
                    ESP_LOGI(TAG, "feed: peak=%d sent_ok=%lu sent_fail=%lu",
                             (int)peak, (unsigned long)sent_ok, (unsigned long)sent_fail);
                }
                g_ws_frame_pos = 0;
            }
        }
        /* codec read 本身由 48kHz I2S 时钟节拍阻塞，不再额外睡眠；额外的
         * 2ms 会让每个 40ms 上行帧固定慢约 5%，长对话会逐渐积累延迟。 */
    }
    heap_caps_free(buf);
    vTaskDelete(NULL);
}

/* ===================================================================
 *  Response 任务：消费 TTS 队列 + 控制消息 + 驱动动作
 * =================================================================== */
static void chat_handle_controls(void);

static void chat_response_task(void *arg)
{
    TickType_t tts_empty_since = 0;
    TickType_t tts_prebuffer_start = 0;
    bool tts_run_active = false;
    bool tts_gap_reported = false;
    int tts_underrun_count = 0;
    int last_session = 0;
    int last_gen = 0;
    int last_seq = 0;

    while (true) {
        /* 用户关闭对话后不再播放已经排队的旧回复。通过队列 API 取出可
         * 同步修正 queued_bytes，避免下次进入对话继承虚假的缓冲水位。 */
        if (!g_chat_on) {
            uint8_t *stale = nullptr;
            int stale_len = 0;
            while (realtime_ws_get_tts_audio(&stale, &stale_len,
                                              nullptr, nullptr, nullptr)) {
                free(stale);
            }
            tts_empty_since = 0;
            tts_prebuffer_start = 0;
            tts_run_active = false;
            tts_gap_reported = false;
            tts_underrun_count = 0;
            last_session = last_gen = last_seq = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* --- 预缓冲 --- */
        int queued_chunks = realtime_ws_get_tts_queue_depth();
        if (!tts_run_active && queued_chunks > 0) {
            if (tts_prebuffer_start == 0) tts_prebuffer_start = xTaskGetTickCount();
            int waited_ms = (int)((xTaskGetTickCount() - tts_prebuffer_start) * portTICK_PERIOD_MS);
            bool stream_ended = realtime_ws_tts_current_stream_ended();
            int buffered_ms = realtime_ws_tts_queued_ms();
            int rx_rate_permille = waited_ms > 100
                                       ? buffered_ms * 1000 / waited_ms
                                       : 0;
            bool ended_ready = stream_ended;
            bool target_ready = buffered_ms >= TTS_PREBUFFER_MS;
            bool timeout_ready = waited_ms >= TTS_PREBUFFER_TIMEOUT_MS;
            if (!ended_ready && !target_ready && !timeout_ready) {
                if (waited_ms % 200 == 0) chat_handle_controls();
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const char *ready_reason = ended_ready ? "stream ended" :
                                       target_ready ? "target reached" :
                                       "hard timeout";
            ESP_LOGI(TAG, "%s ready: waited=%dms, buffered=%dms, rx_rate=%.2fx (%s)",
                     "TTS prebuffer",
                     waited_ms, buffered_ms, (double)rx_rate_permille / 1000.0,
                     ready_reason);
            tts_prebuffer_start = 0;
        } else if (queued_chunks == 0) {
            if (tts_run_active && g_state == CHAT_PLAYING &&
                !realtime_ws_tts_current_stream_ended()) {
                if (tts_empty_since == 0)
                    tts_empty_since = xTaskGetTickCount();
                int empty_ms = (int)((xTaskGetTickCount() - tts_empty_since) *
                                     portTICK_PERIOD_MS);
                if (!tts_gap_reported && empty_ms >= TTS_GAP_LOG_MS) {
                    ++tts_underrun_count;
                    tts_gap_reported = true;
                    ESP_LOGW(TAG,
                             "TTS gap #%d: queue empty for %dms; resume on next chunk",
                             tts_underrun_count, empty_ms);
                }
                /* 与网页播放器一致：保持当前流处于started。下一个chunk到达
                 * 就立即接着写I2S，不再额外等待数秒重缓冲。 */
            } else if (g_state != CHAT_PLAYING) {
                tts_run_active = false;
                last_session = last_gen = last_seq = 0;
                tts_prebuffer_start = 0;
                tts_underrun_count = 0;
                tts_gap_reported = false;
            }
        }

        /* --- 第一优先级：清空 TTS 音频队列 --- */
        uint8_t *audio = nullptr;
        int audio_len = 0;
        bool played_tts = false;
        int cur_session = 0, cur_gen = 0, cur_seq = 0;
        while (g_chat_on && realtime_ws_get_tts_audio(&audio, &audio_len,
                                                       &cur_session, &cur_gen, &cur_seq)) {
            bool first_block = !tts_run_active;
            if (!tts_run_active || cur_seq != last_seq) {
                if (tts_run_active && last_seq != 0 && cur_seq != last_seq)
                    realtime_ws_tts_finish_stream(last_session, last_gen, last_seq);
                if (!tts_run_active) {
                    tts_run_active = true;
                    g_state = CHAT_PLAYING;
                    g_streaming = false;
                    g_chat_playing = true;
                    tts_prebuffer_start = 0;
                    ESP_LOGI(TAG, "TTS playback start, buffered=%dms",
                             realtime_ws_tts_queued_ms());
                }
                realtime_ws_tts_playback_started(cur_session, cur_gen, cur_seq);
                last_session = cur_session;
                last_gen = cur_gen;
                last_seq = cur_seq;
            }

            /* 服务端可能在收到playback_started前限制初始下发量。先按600ms
             * 门槛发送started，再在首块尚未播放时补到约1.6s。这样既释放
             * 服务端流控，又能覆盖ESP端实测接近1s的批次间到包低谷。 */
            if (first_block) {
                TickType_t steady_start = xTaskGetTickCount();
                const char *steady_reason = "timeout";
                while (g_chat_on && g_state == CHAT_PLAYING) {
                    int queued_ms = realtime_ws_tts_queued_ms();
                    int waited_ms = (int)((xTaskGetTickCount() - steady_start) *
                                          portTICK_PERIOD_MS);
                    if (realtime_ws_tts_current_stream_ended()) {
                        steady_reason = "stream ended";
                        break;
                    }
                    if (queued_ms >= TTS_STEADY_QUEUE_MS) {
                        steady_reason = "target reached";
                        break;
                    }
                    if (waited_ms >= TTS_STEADY_WAIT_TIMEOUT_MS) break;
                    chat_handle_controls();
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (!g_chat_on || g_state != CHAT_PLAYING) {
                    free(audio);
                    audio = nullptr;
                    break;
                }
                int steady_waited_ms =
                    (int)((xTaskGetTickCount() - steady_start) * portTICK_PERIOD_MS);
                ESP_LOGI(TAG,
                         "TTS steady buffer ready: waited=%dms, buffered=%dms (%s)",
                         steady_waited_ms, realtime_ws_tts_queued_ms() + 40,
                         steady_reason);
            }
            played_tts = true;
            chat_play_tts(audio, audio_len);
            free(audio);
            realtime_ws_tts_playback_buffer(cur_session, cur_gen, cur_seq);
            tts_empty_since = 0;
            tts_gap_reported = false;
        }

        /* 刚播放完一批后立刻再看队列，避免固定 10ms 睡眠让 I2S 断粮。 */
        if (played_tts) continue;

        /* --- TTS 队列已空 --- */
        if (g_state == CHAT_PLAYING) {
            realtime_ws_tts_finish_if_drained();
            bool has_pending = realtime_ws_tts_has_pending_stream();
            if (realtime_ws_get_tts_queue_depth() == 0 && !has_pending) {
                ESP_LOGI(TAG, "TTS drained — resume listening");
                g_state = CHAT_LISTENING;
                g_streaming = true;
                g_ws_frame_pos = 0;
                g_chat_playing = false;
                tts_empty_since = 0;
                tts_prebuffer_start = 0;
                tts_run_active = false;
                tts_gap_reported = false;
                tts_underrun_count = 0;
                last_session = last_gen = last_seq = 0;
                continue;
            }
            if (tts_empty_since == 0) tts_empty_since = xTaskGetTickCount();
            int empty_ms = (int)((xTaskGetTickCount() - tts_empty_since) * portTICK_PERIOD_MS);
            if (empty_ms >= 5000) {
                ESP_LOGW(TAG, "TTS idle timeout");
                realtime_ws_tts_playback_interrupted();
                g_state = CHAT_LISTENING;
                g_streaming = true;
                g_ws_frame_pos = 0;
                g_chat_playing = false;
                tts_empty_since = 0;
                tts_prebuffer_start = 0;
                tts_run_active = false;
                tts_gap_reported = false;
                last_session = last_gen = last_seq = 0;
            }
        } else {
            tts_empty_since = 0;
        }

        /* --- 第二优先级：控制消息 --- */
        chat_handle_controls();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static void chat_handle_controls(void)
{
    realtime_ws_emotion_t emotion;
    if (realtime_ws_get_emotion(&emotion)) {
        apply_emotion(emotion.label_name[0] ? emotion.label_name : emotion.label_code);
    }

    char *text = nullptr;
    uint8_t *audio = nullptr;
    int audio_len = 0;
    ws_resp_type_t t = realtime_ws_get_response(&text, &audio, &audio_len);

    switch (t) {
    case WS_RESP_TTS_AUDIO:
        g_state = CHAT_PLAYING;
        g_streaming = false;
        g_chat_playing = true;
        chat_play_tts(audio, audio_len);
        if (audio) free(audio);
        break;

    case WS_RESP_STOP_PLAYBACK:
        ESP_LOGI(TAG, "Stop playback — resume listening");
        {
            uint8_t *drain = nullptr;
            int drain_len = 0;
            while (realtime_ws_get_tts_audio(&drain, &drain_len, nullptr, nullptr, nullptr))
                free(drain);
        }
        realtime_ws_tts_playback_interrupted();
        g_state = CHAT_LISTENING;
        g_streaming = true;
        g_ws_frame_pos = 0;
        g_chat_playing = false;
        break;

    case WS_RESP_TURN_END:
        ESP_LOGI(TAG, "Turn ended");
        if (text) { ESP_LOGI(TAG, "AI full: %s", text); free(text); }
        if (g_state == CHAT_PLAYING || realtime_ws_get_tts_queue_depth() > 0 ||
            realtime_ws_tts_has_pending_stream()) {
            ESP_LOGI(TAG, "Turn ended — waiting for TTS drain");
        } else {
            g_state = CHAT_LISTENING;
            g_streaming = true;
            g_ws_frame_pos = 0;
            g_chat_playing = false;
        }
        break;

    case WS_RESP_ASR_FINAL:
        if (text) { ESP_LOGI(TAG, "ASR: %s", text); free(text); }
        break;

    case WS_RESP_LLM_DELTA:
        if (text) { ESP_LOGI(TAG, "AI: %s", text); free(text); }
        /* 半双工：服务端开始回复就闭麦 */
        if (g_streaming) {
            ESP_LOGI(TAG, "Reply started — muting mic (half-duplex)");
            g_streaming = false;
        }
        break;

    case WS_RESP_ERROR:
        ESP_LOGW(TAG, "Server error: %s", text ? text : "?");
        if (text) free(text);
        break;

    default:
        break;
    }
}

/* ===================================================================
 *  动作任务：按包络 + 情绪驱动舵机
 * =================================================================== */
static void chat_motion_task(void *arg)
{
    /* 恐龙五轴: 脖子上下=点头/情绪抬颈, 脖子左右=随节奏倾斜,
     * 头=左右转头, 尾巴LR=随音量摆动, 尾巴UD=随音量抬起 */
    float neck = (float)SERVO_NECK_TILT_DEFAULT;   // 0=最上 180=最下
    float lean = (float)SERVO_NECK_LEAN_DEFAULT;   // 0=右 90=中 180=左
    float head = 90, lr = 90, ud = 150;
    float phase = 0;
    int onset_side = 1;
    uint32_t last_onset_ms = 0;
    float emo_head = 0, emo_lr = 0, emo_ud = 0;
    float emo_speed = 1.0f;
    bool emo_shake = false;

    vTaskDelay(pdMS_TO_TICKS(2000));
    while (true) {
        uint32_t now = esp_log_timestamp();

        if (!g_chat_on) {
            /* 未对话：不写舵机，让 auto_run 自运行动画自由控制 */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        float level = g_chat_level;
        float attack = g_chat_attack;
        bool playing = g_chat_playing;

        /* 情绪参数缓慢逼近目标（快照读，见 g_emo_target 契约注释） */
        const chat_emo_bias_t emo = chat_get_emo_snapshot();
        emo_head += (emo.head_bias - emo_head) * 0.02f;
        emo_lr += (emo.lr_bias - emo_lr) * 0.02f;
        emo_ud += (emo.ud_bias - emo_ud) * 0.02f;
        emo_speed += (emo.speed - emo_speed) * 0.02f;
        emo_shake = emo.shake;

        /* 基础姿态: 情绪→抬颈/低头+轻微转头, 尾巴回中 */
        float t_neck = (float)SERVO_NECK_TILT_DEFAULT - emo_head;
        float t_lean = 90.0f;
        float t_head = 90.0f + emo_head * 0.5f;
        float t_lr = 90.0f + emo_lr;
        float t_ud = 150.0f + emo_ud;

        if (playing) {
            /* 按音频包络驱动: 音量→尾巴大摆幅+抬尾+脖子左右摇, 起始能量→点头 */
            phase += 0.06f + 0.15f * level * emo_speed;
            float wag = std::sin(phase) * (12.0f + 55.0f * level * emo_speed);
            float head_off = 0;
            if (attack > 0.12f && now - last_onset_ms > 260) {
                onset_side = -onset_side;
                last_onset_ms = now;
            }
            if (attack > 0.08f) head_off = onset_side * attack * 18.0f;   // 随语音起始点头
            t_lr += wag;                                     // 尾巴左右大摆幅
            t_ud += std::min(18.0f, level * 28.0f);          // 音量高→尾巴抬更高(180=最上)
            t_neck += head_off;                              // 点头(向下=角度增大)
            t_neck += std::sin(phase * 2.0f) * 5.0f * level; // 随节奏轻微点头
            t_lean += std::sin(phase * 1.5f) * 14.0f * level; // 脖子随节奏左右倾斜
            t_head += std::sin(phase * 2.0f) * 14.0f * level; // 头随节奏左右摆动
            if (emo_shake) t_head += std::sin(phase * 3.0f) * 20.0f * level; // 摇头
        } else if (g_state == CHAT_LISTENING) {
            /* 聆听时轻微摆动，表示“在听” */
            phase += 0.03f;
            t_lr += std::sin(phase) * 8.0f;
            t_ud += std::sin(phase * 0.7f) * 5.0f;
            t_neck += std::sin(phase * 0.5f) * 4.0f;
            t_lean += std::sin(phase * 0.4f) * 5.0f;
            t_head += std::sin(phase * 0.5f) * 5.0f;
        }

        /* 指数逼近 + 限位 */
        float k = 1.0f - std::exp(-20.0f / 120.0f);
        neck += (t_neck - neck) * k;
        lean += (t_lean - lean) * k;
        head += (t_head - head) * k;
        lr += (t_lr - lr) * k;
        ud += (t_ud - ud) * k;
        neck = std::fmax(20.0f, std::fmin(165.0f, neck));
        lean = std::fmax(25.0f, std::fmin(155.0f, lean));
        head = std::fmax(6.0f, std::fmin(174.0f, head));
        lr = std::fmax(6.0f, std::fmin(174.0f, lr));
        ud = std::fmax((float)SERVO_TAIL_UD_MIN, std::fmin(178.0f, ud));

        SetServoAngle(SERVO_NECK_TILT, (int)std::lround(neck));
        SetServoAngle(SERVO_NECK_LEAN, (int)std::lround(lean));
        SetServoAngle(SERVO_HEAD_TURN, (int)std::lround(head));
        SetServoAngle(SERVO_TAIL_LR, (int)std::lround(lr));
        SetServoAngle(SERVO_TAIL_UD, (int)std::lround(ud));

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

/* ===================================================================
 *  公共 API
 * =================================================================== */
static volatile bool s_inited = false;
static bool s_auto_run_prev = true;

void ChatInit(void)
{
#if CHAT_ENABLE
    if (s_inited) return;
    if (!g_ws_frame_buf) {
        g_ws_frame_buf = (int16_t *)heap_caps_malloc(WS_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
        if (!g_ws_frame_buf) { ESP_LOGE(TAG, "WS frame buf alloc fail"); return; }
    }
    /* TLS 需要系统时间——把 WiFi 的时间同步等待注入给协议栈，
     * realtime_ws 自身不依赖 wifi 模块。 */
    const realtime_ws_hooks_t hooks = {.time_sync_wait = WiFiWaitForTimeSync};
    if (realtime_ws_init(CHAT_WS_URL, &hooks) != 0) {
        ESP_LOGE(TAG, "realtime_ws_init fail");
        return;
    }
    s_inited = true;
    /* 与参考工程保持核分工：采集/网络上行放 core 0，TTS 播放放 core 1。
     * 避免双核迁移及 JSON/base64 解码抢占音频输出。 */
    xTaskCreatePinnedToCore(chat_feed_task, "chat_feed", 8192, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(chat_response_task, "chat_resp", 8192, nullptr, 7, nullptr, 1);
    xTaskCreate(chat_motion_task, "chat_motion", 3072, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "Chat module initialized (double-click power to toggle)");
#else
    ESP_LOGW(TAG, "Chat disabled (CHAT_ENABLE=0)");
#endif
}

bool ChatStart(void)
{
#if !CHAT_ENABLE
    return false;
#else
    if (!s_inited) { ESP_LOGW(TAG, "Chat not initialized"); return false; }
    if (g_chat_on) return true;
    ESP_LOGI(TAG, ">>> Chat start <<<");

    /* 暂停小熊猫自动动作 + 清空排队音效, 避免与对话抢 I2S */
#if ENABLE_AUTO_RUN
    if (IsAutoRunRunning()) { s_auto_run_prev = true; SetAutoRunRunning(false); }
    else s_auto_run_prev = false;
#endif
    AudioStopCurrent();  // 立即打断正在播放的叫声/环境声
    FlushAudioQueue();
    AudioPlayChime(true);  // 双击已识别；此时连接可能仍在建立

    /* 参考工程实时音频期间始终关闭 modem sleep。省电模式会按 DTIM
     * 批量收包，表现正是 TTS 突发到达、播放卡顿和控制消息延迟。 */
    WiFiPowerSave(false);

    /* 先清缓冲再开闸（g_ws_frame_pos = 0 必须在 g_chat_on = true 之前）：
     * chat_feed 看到 g_chat_on 立刻开始发帧，若开关在前、清零在后，
     * 可能发出一帧残留的旧音频。 */
    g_ws_frame_pos = 0;
    g_chat_on = true;
    g_state = CHAT_IDLE;
    g_streaming = false;
    g_ready_prompt_pending = true;
    g_chat_playing = false;
    g_chat_level = 0;
    g_chat_attack = 0;
    envelope_reset(&chat_env_);

    ESP_LOGI(TAG, "heap free=%lu bytes at chat start (internal=%lu, psram=%lu)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    realtime_ws_connect();
    return true;
#endif
}

void ChatStop(void)
{
#if !CHAT_ENABLE
    return;
#else
    if (!g_chat_on) return;
    ESP_LOGI(TAG, ">>> Chat stop <<<");
    g_chat_on = false;
    g_state = CHAT_IDLE;
    g_streaming = false;
    g_ready_prompt_pending = false;
    g_chat_playing = false;

    realtime_ws_disconnect();
    WiFiPowerSave(true);

    /* 恢复自动动作（仅当进入对话前它是开启的） */
#if ENABLE_AUTO_RUN
    if (s_auto_run_prev && !IsAutoRunRunning()) SetAutoRunRunning(true);
#endif
    AudioPlayChime(false);  // 关闭对话提示音（合成下扬叮咚）
#endif
}

void ChatToggle(void)
{
    if (g_chat_on) ChatStop();
    else ChatStart();
}

bool ChatIsActive(void) { return g_chat_on; }
bool ChatIsReady(void) {
    return g_chat_on && realtime_ws_is_ready() && !g_ready_prompt_pending;
}
