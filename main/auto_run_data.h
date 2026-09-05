/*
 * auto_run_data.h — 动作库数据表（"动作库本体"的类型与声明）
 *
 * 这里只放"改动作要动的东西"的类型与声明：
 *   DinoPose  五轴目标姿态（脖子俯仰/脖颈前后/小头左右/尾巴上下/尾巴左右）
 *   Keyframe  一个时间点：{持续ms, 目标姿态, 缓动曲线}；kKeep = 保持上一帧
 *   MotionClip 一段动作编舞：名字 + 关键帧序列 + 是否随叫声循环
 *   ClipSet   一个动作的编舞随机池
 * 具体的表在 auto_run_data.cc —— 加/改动作只看那一个文件。
 * 引擎（插值、波形、状态机）在 auto_run.cc，不在这里。
 */
#pragma once

#include "auto_run.h"   // DinoAction 枚举（kActionClips 按它索引）

#include <cstdint>

/* kKeep：该轴保持上一帧的姿态不新插值——这是"动物感"节奏的关键：
 * 头先动 → 脖子跟上 → 尾巴最后交代情绪 */
constexpr int kKeep = -1;

/* 五轴姿态。注意编舞内部 tail_ud 用 0=上、125=下（引擎输出时再转换） */
struct DinoPose {
    int neck_tilt;   // IO15 小头俯仰
    int neck_lean;   // IO16 长颈前后
    int head_turn;   // IO17 小头左右
    int tail_ud;     // IO8  尾巴上下
    int tail_lr;     // IO18 尾巴左右
};

enum Ease : uint8_t {
    EASE_SMOOTH,     // 平滑 S 曲线（默认）
    EASE_FAST_OUT,   // 快出慢收（啄食、惊跳这类"脆"的动作）
    EASE_LINEAR,     // 匀速（保持/凝视类）
};

struct Keyframe {
    uint16_t duration_ms;  // 到达本姿态所用时间（引擎还会加随机 tempo）
    DinoPose target;
    Ease ease;
};

/* 一段编舞。continue_with_audio=true 表示放完一遍不停，换下一段接着做
 * （配长叫声用，如 proud 系列）；false 表示放完回到待机。 */
struct MotionClip {
    const char *name;              // 编舞名（日志里显示）
    const Keyframe *frames;        // 关键帧序列
    uint8_t frame_count;
    bool continue_with_audio;
};

/* 一个动作的编舞池：每次触发从池里不重复随机抽一段 */
struct ClipSet {
    const MotionClip *clips;
    uint8_t count;
};

/* ---- 关键帧表（按动作分组，一表一段编舞） --------------------------- */
extern const Keyframe kDiscoverFrames[];        // 好奇-正面发现
extern const Keyframe kDiscoverPeekFrames[];    // 好奇-先缩后探
extern const Keyframe kDiscoverSkyFrames[];     // 好奇-仰望追踪
extern const Keyframe kAffectionFrames[];       // 亲近-长颈问候鞠躬
extern const Keyframe kAffectionCuddleFrames[]; // 亲近-大弧贴近深低头
extern const Keyframe kAffectionNuzzleFrames[]; // 亲近-脸颊轻蹭
extern const Keyframe kAffectionShyFrames[];    // 亲近-害羞贴近
extern const Keyframe kHappyFrames[];           // 高兴-挺胸展示
extern const Keyframe kHappyDanceFrames[];      // 高兴-大斜线摆动
extern const Keyframe kHappyOrbitFrames[];      // 高兴-空间画圆
extern const Keyframe kHappyChaseFrames[];      // 高兴-追尾游戏
extern const Keyframe kProudCallFrames[];       // 炫耀-正面仰天长鸣
extern const Keyframe kProudSweepFrames[];      // 炫耀-高扫巡视
extern const Keyframe kProudOrbitFrames[];      // 炫耀-长叫慢速巡视
extern const Keyframe kProudEchoFrames[];       // 炫耀-双段长啸
extern const Keyframe kEatFrames[];             // 进食-低啄三口
extern const Keyframe kEatSniffFrames[];        // 进食-先嗅后咬
extern const Keyframe kEatLookUpFrames[];       // 进食-咬完抬头看人
extern const Keyframe kEatGrazeFrames[];        // 进食-贴地横吃
extern const Keyframe kListenFrames[];          // 倾听-定住定位
extern const Keyframe kListenTrackFrames[];     // 倾听-循声跟踪
extern const Keyframe kListenDoubleCheckFrames[]; // 倾听-二次确认
extern const Keyframe kListenOverheadFrames[];  // 倾听-高空声源
extern const Keyframe kStartledFrames[];        // 受惊-后缩评估
extern const Keyframe kStartledJumpFrames[];    // 受惊-对角弹跳
extern const Keyframe kStartledDuckFrames[];    // 受惊-低伏躲避
extern const Keyframe kSleepyFrames[];          // 犯困-慢呼吸趴下
extern const Keyframe kSleepyNodFrames[];       // 犯困-两次点头打盹
extern const Keyframe kSleepyWrapFrames[];      // 犯困-绕身入睡

/* ---- 编舞池（一个动作 = 一个 ClipSet） ----------------------------- */
extern const MotionClip kDiscoverClips[];   // 3 段：正面/先缩后探/仰望
extern const MotionClip kAffectionClips[];  // 4 段：鞠躬/贴近/轻蹭/害羞
extern const MotionClip kHappyClips[];      // 4 段：展示/摆动/画圆/追尾
extern const MotionClip kProudClips[];      // 4 段（配长叫声，循环播）
extern const MotionClip kEatClips[];        // 4 段（配咀嚼声，循环播）
extern const MotionClip kListenClips[];     // 4 段（配脚步声，循环播）
extern const MotionClip kStartledClips[];   // 3 段：后缩/弹跳/低伏
extern const MotionClip kSleepyClips[];     // 3 段：趴下/点头/绕身入睡

/* 全部动作的总表，下标 = DinoAction 枚举值 */
extern const ClipSet kActionClips[DINO_ACTION_COUNT];
