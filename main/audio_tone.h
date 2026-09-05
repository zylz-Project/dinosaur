/*
 * audio_tone.h — 共享的"提示音合成 + 音量包络"算法模块
 *
 * audio.cc（Flash 播放路径）和 chat.cc（对话 TTS 路径）各自都要：
 *   1) 从 PCM 块算平均幅度 → 双包络（快跟慢）→ onset 起音检测，驱动动作；
 *   2) 现场合成正弦提示音（开关机叮咚、对话提示音）。
 * 两处算法相同但系数不同（动作手感是实机调校的，不能合并成一套），
 * 这里提供参数化的公共实现，两边各传自己的系数，行为与原来逐样本一致。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 包络系数组：两边各自持有 static const，数值与拆分前完全一致 */
typedef struct {
    float level_scale;   /* 原始幅度归一化除数（7200=Flash 路径 / 9000=对话路径） */
    float fast_up;       /* 快包络：音量增大时的追击系数 */
    float fast_down;     /* 快包络：音量减小时的下落系数 */
    float slow_k;        /* 慢包络（持续响度）追击系数 */
    float slow_bias;     /* onset 公式里慢包络的偏置系数（audio=1.0, chat=1.08） */
    float onset_gain;    /* onset = (fast - slow*bias) * gain */
    bool  has_attack;    /* Flash 路径多一层 attack 低通（0.58/0.42），对话路径无 */
} envelope_coef_t;

/* 包络状态（两路各一份，互不影响） */
typedef struct {
    float fast;
    float slow;
    float attack;
} envelope_t;

/** 清零包络状态（每个新文件/新会话开始时调用）。 */
void envelope_reset(envelope_t *e);

/**
 * 按一个 PCM 块的平均幅度推进包络。
 * @return onset 起音强度 0..1（新音节/新咬合的瞬间最大）。
 */
float envelope_update(envelope_t *e, float raw_mean, const envelope_coef_t *c);

/**
 * 单段正弦提示音（直写 codec，阻塞播完）。供各提示音函数复用。
 * @param freq_hz 频率; dur_ms 时长; vol 0..1 音量;
 *        attack_ms 起音淡入; release_ms 收尾淡出（防咔哒）。
 */
void tone_sine_write(float freq_hz, int dur_ms, float vol, int attack_ms, int release_ms);

#ifdef __cplusplus
}
#endif
