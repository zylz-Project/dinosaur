/*
 * audio_tone.cc — 提示音合成核 + 双包络算法的公共实现（见 audio_tone.h）
 *
 * 两套调用方系数（与拆分前逐字一致，勿随手"优化"）：
 *   audio.cc Flash 播放：scale=7200, fast=0.28/0.28, slow=0.06,
 *                        bias=1.0, onset=3.2, attack 低通 0.58/0.42
 *   chat.cc TTS 播放  ：scale=9000, fast=0.48/0.12, slow=0.035,
 *                        bias=1.08, onset=3.2, 无 attack 低通
 */
#include "audio_tone.h"
#include "audio.h"

#include <cmath>
#include <cstdint>

void envelope_reset(envelope_t *e)
{
    e->fast = 0.0f;
    e->slow = 0.0f;
    e->attack = 0.0f;
}

float envelope_update(envelope_t *e, float raw_mean, const envelope_coef_t *c)
{
    if (e == nullptr || c == nullptr) return 0.0f;

    float raw = raw_mean / c->level_scale;
    if (raw > 1.0f) raw = 1.0f;

    /* 快包络：上行/下行不同速率，闪避抖动 */
    const float k = raw > e->fast ? c->fast_up : c->fast_down;
    e->fast += (raw - e->fast) * k;

    /* 慢包络：持续响度 */
    e->slow += (raw - e->slow) * c->slow_k;

    /* 起音 = 快包络甩开慢包络的程度 */
    float onset = (e->fast - e->slow * c->slow_bias) * c->onset_gain;
    if (onset < 0.0f) onset = 0.0f;
    if (onset > 1.0f) onset = 1.0f;

    if (c->has_attack) {
        /* 低通一层，动作点头不至于每音节抽动（仅 Flash 路径使用） */
        e->attack = e->attack * 0.58f + onset * 0.42f;
        return e->attack;
    }
    e->attack = onset;
    return onset;
}

void tone_sine_write(float freq_hz, int dur_ms, float vol, int attack_ms, int release_ms)
{
    if (vol <= 0.0f || dur_ms <= 0) return;

    const int sr = 48000;
    const int chunk = 240;  /* 5ms @48k，与播放路径的分块节奏一致 */
    const int total = (int)((long long)sr * dur_ms / 1000);
    const float attack_n = (float)((long long)sr * attack_ms / 1000);
    const float release_n = (float)((long long)sr * release_ms / 1000);
    int16_t buf[chunk];

    for (int pos = 0; pos < total; pos += chunk) {
        int n = total - pos;
        if (n > chunk) n = chunk;
        for (int i = 0; i < n; ++i) {
            const float idx = (float)(pos + i);
            float gain = 1.0f;
            if (idx < attack_n) {
                gain = idx / attack_n;
            } else if (idx >= (float)total - release_n) {
                gain = ((float)total - idx) / release_n;
                if (gain < 0.0f) gain = 0.0f;
            }
            buf[i] = (int16_t)(32767.0f * vol * gain *
                               sinf(2.0f * 3.14159265358979323846f * freq_hz * idx / (float)sr));
        }
        AudioWritePcm48k(buf, n);
    }
}
