/*
 * auto_run.h — 动作系统总入口（自主行为引擎 + 动作调用 API）
 *
 * 职责：管理五轴舵机的"动作"层。上层调用本文件的 Trigger* 系列即可让恐龙
 * 做一个完整动作（含配套叫声），不需要关心舵机角度。
 * 动作编排的关键帧数据表在 auto_run_data.cc（动作库本体）；
 * 波形合成/状态机/音效选择逻辑在 auto_run.cc。
 * 被谁调用：main.cc（开机问候/初始化）、http_server.cc（网页动作按钮）、
 * chat.cc（对话开始/结束时暂停与恢复）。
 */
#pragma once

#include "config.h"

#if ENABLE_AUTO_RUN

// High-level character actions. Callers do not need to know servo indices or
// angles; the auto-run task sequences all five axes and optionally starts a
// matching sound on the same frame.
//
// ── 动作调用速查 ─────────────────────────────────────────────
//   TriggerDinoAction(DINO_ACTION_EAT);
//       → 让恐龙做"进食"动作（不叫）。
//   TriggerDinoActionWithAutoSound(DINO_ACTION_HAPPY);
//       → 做动作，并自动从 Flash 音频库里挑一段语义匹配的叫声一起播。
//   TriggerDinoSoundAction(idx);
//       → 播放音频库第 idx 个文件，并按文件名/时长自动选配动作。
//   TriggerDinoGreeting();
//       → 开机问候：随机挑一段短叫声 + 配套动作（main.cc 上电时调用）。
//   SetAutoRunRunning(false/true);
//       → 暂停/恢复待机自主行为（进入对话时 chat.cc 会先暂停）。
// ────────────────────────────────────────────────────────────
// 8 个动作枚举：每个动作由 auto_run_data.cc 里的一组动作片段(ClipSet)编排。
enum DinoAction {
    DINO_ACTION_DISCOVER = 0,   // 好奇发现：先转头、脖子跟上、歪头打量
    DINO_ACTION_AFFECTION,      // 亲近：凑过来贴贴蹭蹭
    DINO_ACTION_HAPPY,          // 高兴：兴奋鸣叫 + 尾巴快摇
    DINO_ACTION_PROUD_CALL,     // 炫耀：幼年恐龙挺胸长鸣
    DINO_ACTION_EAT,            // 进食：有节奏的啄食/咀嚼
    DINO_ACTION_LISTEN,         // 倾听：定住身体、循声定位
    DINO_ACTION_STARTLED,       // 受惊：后缩、张望、缓过来
    DINO_ACTION_SLEEPY,         // 犯困：趴下放松、呼吸起伏
    DINO_ACTION_COUNT,
};

void InitAutoRun();
bool IsAutoRunRunning();
void SetAutoRunRunning(bool v);
bool IsAutoRunHardSwing();
void SetAutoRunHardSwing(bool v);

// Queue one complete action. sound_index=-1 plays motion only. When a valid
// Flash audio index is supplied, sound and motion are started together.
bool TriggerDinoAction(DinoAction action, int sound_index = -1);

// Choose the action from the Flash filename/duration and queue both together.
// Useful for startup, Web controls, and future touch/voice interaction code.
bool TriggerDinoSoundAction(int sound_index);

// Choose a semantically matching Flash sound for this action. If no suitable
// file exists, the motion still runs silently instead of playing a wrong sound.
bool TriggerDinoActionWithAutoSound(DinoAction action);

// Pick a short vocal sound (never chewing/footsteps) for the boot greeting.
bool TriggerDinoGreeting();

#endif  // ENABLE_AUTO_RUN
