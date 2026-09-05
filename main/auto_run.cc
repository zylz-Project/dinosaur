/*
 * auto_run.cc — 动作引擎：有机波形合成 + 待机状态机 + 音效语义匹配
 *
 * 动作关键帧数据表在 auto_run_data.cc（动作库本体，改动作只改那个文件）；
 * 本文件负责：把 ClipSet 里的关键帧插值成 50Hz 舵机输出（organic_sin
 * 相位扭曲+谐波，避免机械感）、待机状态机（IDLE/RELAX 轮换+随机音效）、
 * 以及 Trigger* 动作调用 API 和"音效文件名→动作"的匹配规则。
 */
#include "auto_run.h"
#include "audio.h"
#include "config.h"
#include "flash_audio.h"
#include "auto_run_data.h"
#include "servo.h"

#if ENABLE_AUTO_RUN

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

static const char *TAG = "dino_auto";

namespace {

constexpr int kTickMs = 20;
constexpr float kPi = 3.14159265358979323846f;

enum TailStyle : uint8_t {
    TAIL_AUTHORED,       // 保留编舞中的情绪轨迹
    TAIL_CROSS_SWEEP,    // 一次或数次跨中位宽扫
    TAIL_CIRCLE,         // 上下+左右空间画圆
    TAIL_SIDE_FLICK,     // 偶发的短促尾尖确认（低权重，不作为猫式常态）
    TAIL_COUNTERWEIGHT,  // 与头颈反向，表现动态配重
    TAIL_HIGH_HOLD,      // 明显上翘并保持，期间仍可轻摆
    TAIL_LOW_SWEEP,      // 中低位左右摆：放下以后仍可跨中位并改变高度
};

enum NeckStyle : uint8_t {
    NECK_AUTHORED,       // 保留角色编舞
    NECK_WIDE_ARC,       // 一次宽阔的侧向巡视弧线
    NECK_ORBIT,          // 两路拉线形成空间椭圆
    NECK_S_CURVE,        // 柔性长脖子的慢速S形
    NECK_REACH,          // 面向用户或食物的前探
};

enum HeadStyle : uint8_t {
    HEAD_AUTHORED,       // 保留关键帧视线
    HEAD_TRACK_NECK,     // 反向补偿脖子，保持观察目标
    HEAD_USER_FOCUS,     // 主要看正前方用户
    HEAD_SIDE_GLANCE,    // 单侧凝视后慢慢回正
};

struct MotionState {
    const MotionClip *clip = nullptr;
    DinoAction action = DINO_ACTION_DISCOVER;
    DinoPose start_pose{SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                        SERVO_HEAD_TURN_DEFAULT, 90, SERVO_TAIL_LR_DEFAULT};
    uint32_t start_ms = 0;
    bool mirror = false;
    float intensity = 1.0f;
    uint8_t speed_percent = 100;
    uint16_t settle_ms = 950;
    float living_amount = 0.0f;
    float living_phase = 0.0f;
    TailStyle tail_style = TAIL_AUTHORED;
    float tail_cycles = 1.2f;
    float tail_amplitude = 48.0f;
    NeckStyle neck_style = NECK_AUTHORED;
    float neck_cycles = 0.9f;
    float neck_amplitude = 42.0f;
    HeadStyle head_style = HEAD_AUTHORED;
    float head_amplitude = 22.0f;
    // Stable random values for this one performance. They change between
    // runs, but remain continuous while sampling the same motion.
    uint32_t variation_seed = 1;
    float head_accent_at = 0.30f;
    float head_accent_width = 0.10f;
    int head_accent = 0;
    float neck_accent_at = 0.48f;
    float neck_accent_width = 0.16f;
    int neck_accent_lr = 0;
    int neck_accent_ud = 0;
    float tail_accent_at = 0.68f;
    float tail_accent_width = 0.18f;
    int tail_accent_lr = 0;
    int tail_accent_ud = 0;
    bool active = false;
};

struct ActionRequest {
    DinoAction action;
    int sound_index;
};

enum BehaviorMode : uint8_t {
    BEHAVIOR_CALM,
    BEHAVIOR_CURIOUS_LEFT,
    BEHAVIOR_CURIOUS_RIGHT,
    BEHAVIOR_PLAYFUL,
    BEHAVIOR_PROUD,
    BEHAVIOR_SLEEP,
    BEHAVIOR_COUNT,
};

struct BehaviorParam {
    DinoPose center;
    DinoPose amplitude;
    float neck_period;
    float head_period;
    float tail_period;
};

struct BehaviorState {
    BehaviorMode current = BEHAVIOR_CALM;
    BehaviorMode previous = BEHAVIOR_CALM;
    uint32_t changed_ms = 0;
};

enum NatureSceneKind : uint8_t {
    NATURE_HORIZON_SCAN,  // 昂首从一侧看向另一侧
    NATURE_RAIN_LISTEN,   // 头先听雨，脖子和尾巴延迟跟随
    NATURE_LOW_DRINK,     // 低头喝水，途中抬眼确认用户
    NATURE_LOOK_UP,       // 从低位抬头观察天空
    NATURE_RAIN_SHAKE,    // 偶发甩水：头快、颈慢、尾巴最后动
    NATURE_SCENE_COUNT,
};

struct NatureSceneState {
    NatureSceneKind current = NATURE_HORIZON_SCAN;
    NatureSceneKind previous = NATURE_RAIN_SHAKE;
    DinoPose from{90, 90, 90, 90, 90};
    uint32_t start_ms = 0;
    uint16_t duration_ms = 7000;
    int direction = 1;
    float head_phase = 0.0f;
    float tail_phase = 0.0f;
};

struct PoseFade {
    bool active = false;
    uint32_t start_ms = 0;
    uint16_t duration_ms = 800;
    DinoPose from{90, 90, 90, 90, 90};
};

enum LifePulseKind : uint8_t {
    LIFE_GLANCE,      // 突然发现一个小动静，头先动、脖子后跟
    LIFE_TAIL_BURST,  // 一次宽尾配重扫，不做犬类连续摇尾
    LIFE_SNIFF,       // 两次轻嗅/点头
    LIFE_PERK,        // 精神一振，身体短暂提起
};

struct LifePulse {
    LifePulseKind kind = LIFE_GLANCE;
    uint32_t start_ms = 0;
    uint16_t duration_ms = 1000;
    int direction = 1;
    int strength = 20;
    bool active = false;
};

static volatile bool g_running = AUTO_RUN_DEFAULT_ON;
static volatile bool g_hard_swing = AUTO_RUN_DEFAULT_HARD;
static QueueHandle_t g_action_queue = nullptr;
// 正值表示 IO18 最近在右侧(>90°)停留更多，负值表示左侧更多。
// 记录角度×时间，而不是只记录“动作次数”。
static float g_tail_lr_bias = 0.0f;

inline int irnd(int n) {
    return n > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(n)) : 0;
}

inline int irand(int lo, int hi) {
    return lo + irnd(hi - lo + 1);
}

inline float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

inline float smoothstep(float value) {
    float x = clamp01(value);
    return x * x * (3.0f - 2.0f * x);
}

inline float ease_value(Ease ease, float value) {
    float x = clamp01(value);
    if (ease == EASE_FAST_OUT) {
        float inv = 1.0f - x;
        return 1.0f - inv * inv * inv;
    }
    if (ease == EASE_LINEAR) return x;
    return smoothstep(x);
}

inline int lerp_angle(int from, int to, float amount) {
    return from + static_cast<int>(std::lround((to - from) * amount));
}

static DinoPose lerp_pose(const DinoPose &from, const DinoPose &to, float amount) {
    return {
        lerp_angle(from.neck_tilt, to.neck_tilt, amount),
        lerp_angle(from.neck_lean, to.neck_lean, amount),
        lerp_angle(from.head_turn, to.head_turn, amount),
        lerp_angle(from.tail_ud, to.tail_ud, amount),
        lerp_angle(from.tail_lr, to.tail_lr, amount),
    };
}

inline int catmull_angle(int p0, int p1, int p2, int p3, float amount) {
    float t2 = amount * amount;
    float t3 = t2 * amount;
    float value = 0.5f * ((2.0f * p1) + (-p0 + p2) * amount +
                          (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                          (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    return static_cast<int>(std::lround(value));
}

static DinoPose catmull_pose(const DinoPose &p0, const DinoPose &p1,
                             const DinoPose &p2, const DinoPose &p3,
                             float amount) {
    return {
        catmull_angle(p0.neck_tilt, p1.neck_tilt, p2.neck_tilt, p3.neck_tilt, amount),
        catmull_angle(p0.neck_lean, p1.neck_lean, p2.neck_lean, p3.neck_lean, amount),
        catmull_angle(p0.head_turn, p1.head_turn, p2.head_turn, p3.head_turn, amount),
        catmull_angle(p0.tail_ud, p1.tail_ud, p2.tail_ud, p3.tail_ud, amount),
        catmull_angle(p0.tail_lr, p1.tail_lr, p2.tail_lr, p3.tail_lr, amount),
    };
}

inline int mirrored_angle(int value) {
    return 180 - value;
}

inline int expressive_angle(int value, float intensity) {
    return 90 + static_cast<int>(std::lround((value - 90) * intensity));
}

static DinoPose resolve_target(const DinoPose &encoded, const DinoPose &previous,
                               bool mirror, float intensity) {
    DinoPose result = previous;
    if (encoded.neck_tilt != kKeep)
        result.neck_tilt = expressive_angle(encoded.neck_tilt, intensity);
    if (encoded.neck_lean != kKeep) {
        int value = expressive_angle(encoded.neck_lean, intensity);
        result.neck_lean = mirror ? mirrored_angle(value) : value;
    }
    if (encoded.head_turn != kKeep) {
        // 头部允许使用完整机械行程，平时仍由编舞角度控制在自然范围。
        int value = expressive_angle(encoded.head_turn, std::min(intensity, 1.12f));
        result.head_turn = mirror ? mirrored_angle(value) : value;
    }
    if (encoded.tail_ud != kKeep)
        result.tail_ud = expressive_angle(encoded.tail_ud, intensity);
    if (encoded.tail_lr != kKeep) {
        int value = expressive_angle(encoded.tail_lr, intensity);
        result.tail_lr = mirror ? mirrored_angle(value) : value;
    }
    return result;
}

static void clamp_pose(DinoPose &pose) {
    // 实机确认头部、脖子和尾巴左右都可使用完整行程。
    pose.neck_tilt = std::clamp(pose.neck_tilt, 0, 180);
    pose.neck_lean = std::clamp(pose.neck_lean, 0, 180);
    pose.head_turn = std::clamp(pose.head_turn, 0, 180);
    // 编舞内部仍使用 0=上、125=下；输出时转换为 IO8 的 180=上、55=下。
    pose.tail_ud = std::clamp(pose.tail_ud, 0,
                              SERVO_MAX_ANGLE - SERVO_TAIL_UD_MIN);
    pose.tail_lr = std::clamp(pose.tail_lr, 0, 180);
}

static void output_pose(DinoPose pose) {
    clamp_pose(pose);
    // 约6.7秒时间常数的左右占用记忆；>90 是 IO18 右侧。
    g_tail_lr_bias = g_tail_lr_bias * 0.997f +
                     static_cast<float>(pose.tail_lr - SERVO_TAIL_LR_DEFAULT) * 0.003f;
    // 保留单次大幅侧摆，同时轻量抵消长时间累积的左/右侧停留偏差。
    pose.tail_lr -= static_cast<int>(g_tail_lr_bias * 0.22f);
    pose.tail_lr = std::clamp(pose.tail_lr, 0, 180);
    SetServoAngle(SERVO_NECK_TILT, pose.neck_tilt);
    SetServoAngle(SERVO_NECK_LEAN, pose.neck_lean);
    SetServoAngle(SERVO_HEAD_TURN, pose.head_turn);
    SetServoAngle(SERVO_TAIL_UD, SERVO_MAX_ANGLE - pose.tail_ud);
    SetServoAngle(SERVO_TAIL_LR, pose.tail_lr);
}


// ↑ 动作关键帧数据表在 auto_run_data.cc（动作库本体，改动作只改那个文件），
// 类型与总表 kActionClips 由 auto_run_data.h 提供；本文件只负责引擎。

static uint32_t mix_variation(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

static uint32_t frame_duration_ms(const MotionState &motion, const Keyframe &frame) {
    uint32_t frame_index = static_cast<uint32_t>(&frame - motion.clip->frames);
    uint32_t hash = mix_variation(motion.variation_seed ^ (frame_index * 0x9e3779b9U));
    uint32_t local_tempo = 118U + hash % 37U;  // each frame independently 118%..154%
    uint32_t duration = static_cast<uint32_t>(frame.duration_ms) * 100U /
                        motion.speed_percent;
    duration = duration * local_tempo / 100U;
    return std::max<uint32_t>(55, duration);
}

static uint32_t clip_content_duration_ms(const MotionState &motion) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < motion.clip->frame_count; ++i)
        total += frame_duration_ms(motion, motion.clip->frames[i]);
    return total;
}

// 先用短暂“看向用户”建立互动，再进入角色动作；最后留出收势时间。
constexpr uint32_t kEngageMs = 260;

static DinoPose clip_attention_pose(const MotionState &motion) {
    DinoPose pose = motion.start_pose;
    pose.neck_lean = 90;
    pose.head_turn = SERVO_HEAD_TURN_DEFAULT;
    pose.tail_lr = SERVO_TAIL_LR_DEFAULT;
    switch (motion.action) {
    case DINO_ACTION_DISCOVER:
    case DINO_ACTION_AFFECTION:
    case DINO_ACTION_HAPPY:
        pose.neck_tilt = 108;  // 向正前方用户靠近一点
        pose.tail_ud = 58;
        break;
    case DINO_ACTION_EAT:
        pose.neck_tilt = 112;  // 先看用户手中的食物，再低头
        pose.tail_ud = 72;
        break;
    case DINO_ACTION_PROUD_CALL:
        pose.neck_tilt = 68;
        pose.tail_ud = 44;
        break;
    case DINO_ACTION_LISTEN:
        pose.neck_tilt = 78;
        pose.tail_ud = 46;
        break;
    case DINO_ACTION_STARTLED:
        pose.neck_tilt = 56;
        pose.tail_ud = 28;   // 长颈恐龙受惊时用高位尾姿稳定身体，不做猫式夹尾
        break;
    case DINO_ACTION_SLEEPY:
    default:
        pose.neck_tilt = 82;
        pose.tail_ud = 110;
        break;
    }
    return pose;
}

static DinoPose clip_final_pose(const MotionState &motion) {
    DinoPose pose = clip_attention_pose(motion);
    for (uint8_t i = 0; i < motion.clip->frame_count; ++i)
        pose = resolve_target(motion.clip->frames[i].target, pose, motion.mirror,
                              motion.intensity);
    return pose;
}

static DinoPose clip_settled_pose(const MotionState &motion) {
    DinoPose pose = clip_final_pose(motion);
    pose.neck_lean = 90;
    pose.head_turn = SERVO_HEAD_TURN_DEFAULT;  // 90° = 朝正前方
    pose.tail_lr = SERVO_TAIL_LR_DEFAULT;      // 90° = 尾巴左右中位
    return pose;
}

// 编舞池抽取：每个动作一个"洗牌袋"——池里每段都抽过一次后重新洗牌，
// 保证不连播同一段也不会漏段。
constexpr uint8_t kMaxClipsPerAction = 4;

struct ClipShuffleBag {
    uint8_t order[kMaxClipsPerAction]{};
    uint8_t count = 0;
    uint8_t cursor = 0;
};

static const MotionClip *choose_clip(DinoAction action, const MotionClip *avoid) {
    const ClipSet &set = kActionClips[action];
    static ClipShuffleBag bags[DINO_ACTION_COUNT];
    ClipShuffleBag &bag = bags[action];

    if (bag.count != set.count || bag.cursor >= bag.count) {
        bag.count = std::min<uint8_t>(set.count, kMaxClipsPerAction);
        bag.cursor = 0;
        for (uint8_t i = 0; i < bag.count; ++i) bag.order[i] = i;
        // Fisher-Yates shuffle: every clip appears once before the next cycle.
        for (int i = bag.count - 1; i > 0; --i) {
            int j = irnd(i + 1);
            std::swap(bag.order[i], bag.order[j]);
        }
        // Across a bag boundary, the new first clip must not equal the one
        // that just finished. Swap, do not discard, so coverage stays intact.
        if (avoid && bag.count > 1 && &set.clips[bag.order[0]] == avoid) {
            int swap_index = 1 + irnd(bag.count - 1);
            std::swap(bag.order[0], bag.order[swap_index]);
        }
    }

    int index = bag.order[bag.cursor++];
    return &set.clips[index];
}

static float action_intensity(DinoAction action) {
    switch (action) {
    case DINO_ACTION_HAPPY:
    case DINO_ACTION_PROUD_CALL:
    case DINO_ACTION_STARTLED:
        return 1.32f + irnd(15) / 100.0f;  // 1.32~1.47: visual-impact actions
    case DINO_ACTION_DISCOVER:
    case DINO_ACTION_LISTEN:
        return 1.26f + irnd(15) / 100.0f;
    case DINO_ACTION_AFFECTION:
    case DINO_ACTION_EAT:
        return 1.20f + irnd(13) / 100.0f;
    case DINO_ACTION_SLEEPY:
    default:
        return 1.06f + irnd(8) / 100.0f;
    }
}

static float action_living_amount(DinoAction action) {
    switch (action) {
    case DINO_ACTION_HAPPY: return 1.9f;
    case DINO_ACTION_DISCOVER: return 1.5f;
    case DINO_ACTION_AFFECTION: return 1.2f;
    case DINO_ACTION_PROUD_CALL: return 1.4f;
    case DINO_ACTION_EAT: return 1.0f;
    case DINO_ACTION_LISTEN: return 0.7f;
    case DINO_ACTION_STARTLED: return 0.5f;
    case DINO_ACTION_SLEEPY: return 0.45f;
    default: return 0.9f;
    }
}

static TailStyle random_tail_style(DinoAction action) {
    int pick = irnd(100);
    switch (action) {
    case DINO_ACTION_HAPPY:
        if (pick < 24) return TAIL_CROSS_SWEEP;
        if (pick < 42) return TAIL_CIRCLE;
        if (pick < 64) return TAIL_LOW_SWEEP;
        if (pick < 76) return TAIL_SIDE_FLICK;
        if (pick < 82) return TAIL_HIGH_HOLD;
        if (pick < 96) return TAIL_COUNTERWEIGHT;
        return TAIL_AUTHORED;
    case DINO_ACTION_PROUD_CALL:
        if (pick < 18) return TAIL_CIRCLE;
        if (pick < 36) return TAIL_CROSS_SWEEP;
        if (pick < 54) return TAIL_LOW_SWEEP;
        if (pick < 78) return TAIL_COUNTERWEIGHT;
        if (pick < 86) return TAIL_HIGH_HOLD;
        return TAIL_AUTHORED;
    case DINO_ACTION_AFFECTION:
        if (pick < 16) return TAIL_COUNTERWEIGHT;
        if (pick < 21) return TAIL_HIGH_HOLD;
        if (pick < 35) return TAIL_AUTHORED;
        if (pick < 53) return TAIL_CROSS_SWEEP;
        if (pick < 65) return TAIL_SIDE_FLICK;
        if (pick < 75) return TAIL_CIRCLE;
        return TAIL_LOW_SWEEP;
    case DINO_ACTION_EAT:
        if (pick < 20) return TAIL_AUTHORED;
        if (pick < 48) return TAIL_COUNTERWEIGHT;
        if (pick < 80) return TAIL_LOW_SWEEP;
        if (pick < 85) return TAIL_HIGH_HOLD;
        return TAIL_SIDE_FLICK;
    case DINO_ACTION_LISTEN:
        if (pick < 10) return TAIL_HIGH_HOLD;
        if (pick < 35) return TAIL_COUNTERWEIGHT;
        if (pick < 65) return TAIL_LOW_SWEEP;
        if (pick < 85) return TAIL_AUTHORED;
        return TAIL_SIDE_FLICK;
    case DINO_ACTION_SLEEPY:
        if (pick < 30) return TAIL_AUTHORED;
        if (pick < 55) return TAIL_COUNTERWEIGHT;
        return TAIL_LOW_SWEEP;
    case DINO_ACTION_STARTLED:
        if (pick < 35) return TAIL_COUNTERWEIGHT;
        if (pick < 50) return TAIL_HIGH_HOLD;
        if (pick < 65) return TAIL_LOW_SWEEP;
        if (pick < 85) return TAIL_AUTHORED;
        return TAIL_CROSS_SWEEP;
    case DINO_ACTION_DISCOVER:
    default:
        if (pick < 20) return TAIL_COUNTERWEIGHT;
        if (pick < 28) return TAIL_HIGH_HOLD;
        if (pick < 42) return TAIL_AUTHORED;
        if (pick < 56) return TAIL_CIRCLE;
        if (pick < 74) return TAIL_CROSS_SWEEP;
        return TAIL_LOW_SWEEP;
    }
}

static TailStyle choose_tail_style(DinoAction action) {
    static TailStyle previous = TAIL_AUTHORED;
    TailStyle selected = random_tail_style(action);
    // 可以偶尔重复，但不会连续三段都像复制粘贴。
    static uint8_t repeat_count = 0;
    if (selected == previous) {
        ++repeat_count;
        if (repeat_count >= 2) {
            for (int i = 0; i < 4 && selected == previous; ++i)
                selected = random_tail_style(action);
            repeat_count = 0;
        }
    } else {
        repeat_count = 0;
    }
    previous = selected;
    return selected;
}

static NeckStyle random_neck_style(DinoAction action) {
    int pick = irnd(100);
    switch (action) {
    case DINO_ACTION_HAPPY:
        if (pick < 18) return NECK_ORBIT;
        if (pick < 40) return NECK_S_CURVE;
        if (pick < 80) return NECK_WIDE_ARC;
        return NECK_AUTHORED;
    case DINO_ACTION_PROUD_CALL:
        // 叫声主体优先保留IO17=0°仰天姿态；空间变化放在叫声保持之后。
        if (pick < 60) return NECK_AUTHORED;
        if (pick < 85) return NECK_WIDE_ARC;
        if (pick < 95) return NECK_ORBIT;
        return NECK_S_CURVE;
    case DINO_ACTION_AFFECTION:
        if (pick < 48) return NECK_REACH;
        if (pick < 75) return NECK_WIDE_ARC;
        if (pick < 90) return NECK_AUTHORED;
        return NECK_S_CURVE;
    case DINO_ACTION_EAT:
        if (pick < 45) return NECK_REACH;
        if (pick < 78) return NECK_AUTHORED;
        if (pick < 95) return NECK_WIDE_ARC;
        return NECK_S_CURVE;
    case DINO_ACTION_LISTEN:
        if (pick < 38) return NECK_WIDE_ARC;
        if (pick < 68) return NECK_AUTHORED;
        if (pick < 92) return NECK_REACH;
        return NECK_S_CURVE;
    case DINO_ACTION_SLEEPY:
        if (pick < 68) return NECK_AUTHORED;
        return NECK_WIDE_ARC;
    case DINO_ACTION_STARTLED:
        if (pick < 55) return NECK_AUTHORED;
        if (pick < 78) return NECK_REACH;
        return NECK_WIDE_ARC;
    case DINO_ACTION_DISCOVER:
    default:
        if (pick < 30) return NECK_REACH;
        if (pick < 70) return NECK_WIDE_ARC;
        if (pick < 90) return NECK_AUTHORED;
        return NECK_S_CURVE;
    }
}

static NeckStyle choose_neck_style(DinoAction action) {
    static NeckStyle previous = NECK_AUTHORED;
    NeckStyle selected = random_neck_style(action);
    if (selected == previous) {
        for (int i = 0; i < 3 && selected == previous; ++i)
            selected = random_neck_style(action);
    }
    previous = selected;
    return selected;
}

static HeadStyle random_head_style(DinoAction action) {
    int pick = irnd(100);
    if (action == DINO_ACTION_EAT) {
        if (pick < 35) return HEAD_USER_FOCUS;
        if (pick < 75) return HEAD_TRACK_NECK;
        if (pick < 90) return HEAD_AUTHORED;
        return HEAD_SIDE_GLANCE;
    }
    if (action == DINO_ACTION_AFFECTION || action == DINO_ACTION_DISCOVER) {
        if (pick < 42) return HEAD_USER_FOCUS;
        if (pick < 78) return HEAD_TRACK_NECK;
        if (pick < 92) return HEAD_AUTHORED;
        return HEAD_SIDE_GLANCE;
    }
    if (action == DINO_ACTION_LISTEN || action == DINO_ACTION_STARTLED) {
        if (pick < 38) return HEAD_SIDE_GLANCE;
        if (pick < 68) return HEAD_TRACK_NECK;
        if (pick < 86) return HEAD_AUTHORED;
        return HEAD_USER_FOCUS;
    }
    if (pick < 42) return HEAD_TRACK_NECK;
    if (pick < 72) return HEAD_USER_FOCUS;
    if (pick < 94) return HEAD_AUTHORED;
    return HEAD_SIDE_GLANCE;
}

static HeadStyle choose_head_style(DinoAction action) {
    static HeadStyle previous = HEAD_AUTHORED;
    HeadStyle selected = random_head_style(action);
    if (selected == previous) {
        for (int i = 0; i < 3 && selected == previous; ++i)
            selected = random_head_style(action);
    }
    previous = selected;
    return selected;
}

static void start_motion(MotionState &motion, DinoAction action, uint32_t now_ms,
                         const DinoPose &current_pose, const MotionClip *avoid = nullptr) {
    if (action < 0 || action >= DINO_ACTION_COUNT) return;
    motion.clip = choose_clip(action, avoid);
    motion.action = action;
    motion.start_pose = current_pose;
    motion.start_ms = now_ms;
    // 首摆方向保持随机，但概率由最近真实左右停留面积轻微校正。
    // g_tail_lr_bias>0 代表 IO18 在右侧停留更多，此时提高 mirror(向左)概率。
    static bool previous_mirror = false;
    static uint8_t same_direction_count = 0;
    int mirror_probability = 50 + static_cast<int>(g_tail_lr_bias * 2.4f);
    mirror_probability = std::clamp(mirror_probability, 8, 92);
    bool selected_mirror = irnd(100) < mirror_probability;
    if (selected_mirror == previous_mirror) {
        ++same_direction_count;
        if (same_direction_count >= 2) {
            selected_mirror = !previous_mirror;
            same_direction_count = 0;
        }
    } else {
        same_direction_count = 0;
    }
    previous_mirror = selected_mirror;
    motion.mirror = selected_mirror;
    motion.variation_seed = esp_random();

    // One-off organic accents are generated independently for head, neck and
    // tail. Some are intentionally absent, so the same clip does not reveal
    // the same small reaction every time.
    motion.head_accent_at = irand(15, 64) / 100.0f;
    motion.head_accent_width = irand(8, 14) / 100.0f;
    motion.head_accent = irnd(100) < 80 ? (irnd(2) ? 1 : -1) * irand(14, 32) : 0;

    motion.neck_accent_at = irand(24, 70) / 100.0f;
    motion.neck_accent_width = irand(12, 20) / 100.0f;
    motion.neck_accent_lr = irnd(100) < 84 ? (irnd(2) ? 1 : -1) * irand(16, 38) : 0;
    motion.neck_accent_ud = irand(-9, 10);

    motion.tail_accent_at = irand(34, 77) / 100.0f;
    motion.tail_accent_width = irand(14, 20) / 100.0f;
    motion.tail_accent_lr = irnd(100) < 92 ? (irnd(2) ? 1 : -1) * irand(30, 62) : 0;
    motion.tail_accent_ud = irnd(100) < 72 ? irand(14, 38) : -irand(10, 30);

    if (action == DINO_ACTION_PROUD_CALL) {
        motion.neck_accent_ud = -irand(4, 12);  // a surprise accent reinforces the call
        motion.tail_accent_ud = irnd(100) < 62 ? -irand(8, 24) : irand(8, 22);
    } else if (action == DINO_ACTION_EAT) {
        motion.neck_accent_ud = irand(3, 11);   // remain close to the food
        motion.tail_accent_ud = irand(16, 32);  // counter the frequent eating lift with release
    } else if (action == DINO_ACTION_SLEEPY) {
        motion.head_accent /= 2;
        motion.neck_accent_lr /= 2;
        motion.neck_accent_ud = irand(2, 8);
        motion.tail_accent_lr /= 2;
        motion.tail_accent_ud = irand(4, 18);   // more likely to relax downward
    }
    motion.intensity = action_intensity(action);
    // 头颈以宽而慢的弧线为主；只有发现、听声、受惊保留少量快速反应。
    switch (action) {
    case DINO_ACTION_STARTLED:
        motion.speed_percent = static_cast<uint8_t>(irand(120, 140));
        break;
    case DINO_ACTION_DISCOVER:
    case DINO_ACTION_LISTEN:
        motion.speed_percent = static_cast<uint8_t>(irand(110, 130));
        break;
    case DINO_ACTION_HAPPY:
        motion.speed_percent = static_cast<uint8_t>(irand(105, 125));
        break;
    case DINO_ACTION_EAT:
    case DINO_ACTION_PROUD_CALL:
        motion.speed_percent = static_cast<uint8_t>(irand(100, 120));
        break;
    case DINO_ACTION_AFFECTION:
        motion.speed_percent = static_cast<uint8_t>(irand(95, 115));
        break;
    case DINO_ACTION_SLEEPY:
    default:
        motion.speed_percent = static_cast<uint8_t>(irand(90, 110));
        break;
    }
    if (action == DINO_ACTION_SLEEPY) {
        motion.settle_ms = static_cast<uint16_t>(irand(600, 950));
    } else if (action == DINO_ACTION_AFFECTION) {
        motion.settle_ms = static_cast<uint16_t>(irand(450, 750));
    } else {
        motion.settle_ms = static_cast<uint16_t>(irand(300, 600));
    }
    motion.living_amount = action_living_amount(action);
    motion.living_phase = irnd(628) / 100.0f;
    motion.tail_style = choose_tail_style(action);
    motion.tail_cycles = irand(105, 165) / 100.0f;
    motion.tail_amplitude = static_cast<float>(irand(70, 104));
    if (action == DINO_ACTION_HAPPY) {
        motion.tail_cycles = irand(130, 195) / 100.0f;
        motion.tail_amplitude = static_cast<float>(irand(88, 112));
    } else if (action == DINO_ACTION_PROUD_CALL) {
        motion.tail_cycles = irand(110, 165) / 100.0f;
        motion.tail_amplitude = static_cast<float>(irand(82, 110));
    } else if (action == DINO_ACTION_SLEEPY) {
        motion.tail_cycles = irand(55, 90) / 100.0f;
        motion.tail_amplitude = static_cast<float>(irand(34, 54));
    } else if (action == DINO_ACTION_EAT) {
        motion.tail_cycles = irand(85, 130) / 100.0f;
        motion.tail_amplitude = static_cast<float>(irand(50, 76));
    }
    // 宽扫和画圆在一段动作内至少完成足够相位，确保真正跨过90°中位。
    // 仍由整段速度控制，因此表现为大而流畅，不是快速机械抖动。
    if (motion.tail_style == TAIL_CROSS_SWEEP || motion.tail_style == TAIL_CIRCLE)
        motion.tail_cycles = std::max(motion.tail_cycles, 1.08f);
    else if (motion.tail_style == TAIL_LOW_SWEEP)
        motion.tail_cycles = std::max(motion.tail_cycles, 0.92f);
    motion.neck_style = choose_neck_style(action);
    motion.head_style = choose_head_style(action);
    motion.neck_cycles = irand(60, 105) / 100.0f;
    motion.neck_amplitude = static_cast<float>(irand(44, 76));
    motion.head_amplitude = static_cast<float>(irand(24, 46));
    if (action == DINO_ACTION_HAPPY || action == DINO_ACTION_PROUD_CALL) {
        motion.neck_cycles = irand(65, 115) / 100.0f;
        motion.neck_amplitude = static_cast<float>(irand(68, 98));
    } else if (action == DINO_ACTION_EAT || action == DINO_ACTION_SLEEPY) {
        motion.neck_cycles = irand(42, 80) / 100.0f;
        motion.neck_amplitude = static_cast<float>(irand(28, 50));
    }
    if (action == DINO_ACTION_DISCOVER || action == DINO_ACTION_LISTEN ||
        action == DINO_ACTION_STARTLED) {
        motion.head_amplitude = static_cast<float>(irand(30, 54));
    } else if (action == DINO_ACTION_EAT || action == DINO_ACTION_SLEEPY) {
        motion.head_amplitude = static_cast<float>(irand(16, 34));
    }

    // Avoid recently seen high-level performances, not just the exact clip.
    // The signature excludes tiny organic accents: those already change on
    // every run and should not hide a repeated clip/style/direction skeleton.
    static uint32_t recent_signatures[12]{};
    static uint8_t recent_count = 0;
    static uint8_t recent_cursor = 0;
    const ClipSet &clip_set = kActionClips[action];
    uint32_t clip_index = static_cast<uint32_t>(motion.clip - clip_set.clips);
    auto make_signature = [&]() {
        return 0x80000000U |
               (static_cast<uint32_t>(action) << 12) |
               (clip_index << 10) |
               (static_cast<uint32_t>(motion.mirror) << 9) |
               (static_cast<uint32_t>(motion.neck_style) << 6) |
               (static_cast<uint32_t>(motion.head_style) << 4) |
               static_cast<uint32_t>(motion.tail_style);
    };
    uint32_t signature = make_signature();
    for (int attempt = 0; attempt < 6; ++attempt) {
        bool seen = false;
        for (uint8_t i = 0; i < recent_count; ++i) {
            if (recent_signatures[i] == signature) {
                seen = true;
                break;
            }
        }
        if (!seen) break;
        motion.tail_style = random_tail_style(action);
        motion.neck_style = random_neck_style(action);
        motion.head_style = random_head_style(action);
        if (irnd(100) < 35) motion.mirror = !motion.mirror;
        signature = make_signature();
    }
    recent_signatures[recent_cursor] = signature;
    recent_cursor = (recent_cursor + 1) %
                    static_cast<uint8_t>(sizeof(recent_signatures) /
                                         sizeof(recent_signatures[0]));
    recent_count = std::min<uint8_t>(
        static_cast<uint8_t>(sizeof(recent_signatures) / sizeof(recent_signatures[0])),
        static_cast<uint8_t>(recent_count + 1));

    // A collision reroll may have selected a spatial tail style after its
    // original cycle constraint was applied.
    if (motion.tail_style == TAIL_CROSS_SWEEP || motion.tail_style == TAIL_CIRCLE)
        motion.tail_cycles = std::max(motion.tail_cycles, 1.08f);
    else if (motion.tail_style == TAIL_LOW_SWEEP)
        motion.tail_cycles = std::max(motion.tail_cycles, 0.92f);

    motion.active = true;
    ESP_LOGI(TAG, "Action: %s%s intensity=%.2f speed=%u%% styles[n=%u h=%u t=%u] sig=%08lx tail_bias=%.1f",
             motion.clip->name,
             motion.mirror ? " (mirror)" : "", motion.intensity,
             static_cast<unsigned>(motion.speed_percent),
             static_cast<unsigned>(motion.neck_style),
             static_cast<unsigned>(motion.head_style),
             static_cast<unsigned>(motion.tail_style),
             static_cast<unsigned long>(signature), g_tail_lr_bias);
}

static float organic_accent(float progress, float center, float half_width) {
    if (half_width <= 0.0f) return 0.0f;
    float distance = std::fabs(progress - center);
    if (distance >= half_width) return 0.0f;
    // A raised-cosine single gesture has zero speed at both ends, unlike a
    // square random offset that would look like servo noise.
    float local = 1.0f - distance / half_width;
    return 0.5f - 0.5f * std::cos(local * kPi);
}

static void apply_living_motion(const MotionState &motion, uint32_t now_ms,
                                uint32_t elapsed, uint32_t total, DinoPose &pose) {
    if (motion.living_amount <= 0.0f || total == 0) return;
    float ramp_in = smoothstep(static_cast<float>(elapsed) / 260.0f);
    float ramp_out = smoothstep(static_cast<float>(total - std::min(total, elapsed)) / 340.0f);
    float envelope = std::min(ramp_in, ramp_out) * motion.living_amount;
    float t = now_ms / 1000.0f + motion.living_phase;
    float progress = clamp01(static_cast<float>(elapsed) / static_cast<float>(total));
    float story_envelope = std::sin(progress * kPi);
    story_envelope = std::sqrt(std::max(0.0f, story_envelope));
    float story_phase = progress * motion.neck_cycles * 2.0f * kPi;
    float story_direction = motion.mirror ? -1.0f : 1.0f;

    float neck_orbit = 1.0f;
    float tail_orbit = 1.0f;
    if (motion.action == DINO_ACTION_HAPPY) {
        neck_orbit = 2.6f;
        tail_orbit = 3.0f;
    } else if (motion.action == DINO_ACTION_PROUD_CALL) {
        neck_orbit = 2.4f;
        tail_orbit = 2.7f;
    } else if (motion.action == DINO_ACTION_DISCOVER ||
               motion.action == DINO_ACTION_AFFECTION) {
        neck_orbit = 1.8f;
        tail_orbit = 2.0f;
    } else if (motion.action == DINO_ACTION_SLEEPY) {
        neck_orbit = 0.6f;
        tail_orbit = 0.5f;
    }

    // Different irrational-looking periods prevent the five axes from forming
    // a mechanical synchronized wave. Slow quadrature terms make spatial
    // neck/tail ellipses; quicker small terms keep the surface alive.
    float neck_circle = t * 2.15f;
    float tail_circle = t * 2.67f - 0.9f;
    pose.neck_tilt += static_cast<int>((std::sin(neck_circle) * 8.0f * neck_orbit +
                                       std::sin(t * 3.2f) * 2.0f) * envelope);
    pose.neck_lean += static_cast<int>(std::sin(t * 2.7f) * 2.6f * envelope +
                                      std::cos(neck_circle) * 10.0f * neck_orbit * envelope);
    pose.head_turn += static_cast<int>((std::sin(t * 3.6f + 0.8f) * 2.2f -
                                       std::cos(neck_circle) * 2.8f * neck_orbit) * envelope);

    // 同一关键帧片段可叠加不同长脖子表演方式。变化都是慢速大路径，
    // 不用快速左右反向来制造“活泼”。
    switch (motion.neck_style) {
    case NECK_WIDE_ARC:
        pose.neck_lean += static_cast<int>(story_direction * motion.neck_amplitude *
                                           0.78f * story_envelope);
        pose.neck_tilt += static_cast<int>(motion.neck_amplitude * 0.30f *
                                           std::sin(progress * 2.0f * kPi - 0.7f) *
                                           story_envelope);
        break;
    case NECK_ORBIT: {
        DinoPose orbit = pose;
        orbit.neck_tilt = 90 + static_cast<int>(motion.neck_amplitude * 0.82f *
                                                std::sin(story_phase));
        orbit.neck_lean = 90 + static_cast<int>(story_direction * motion.neck_amplitude *
                                                std::cos(story_phase));
        float blend = 0.58f * story_envelope;
        pose.neck_tilt = lerp_angle(pose.neck_tilt, orbit.neck_tilt, blend);
        pose.neck_lean = lerp_angle(pose.neck_lean, orbit.neck_lean, blend);
        break;
    }
    case NECK_S_CURVE:
        pose.neck_lean += static_cast<int>(story_direction * motion.neck_amplitude *
                                           0.68f * std::sin(story_phase) * story_envelope);
        pose.neck_tilt += static_cast<int>(motion.neck_amplitude * 0.34f *
                                           std::sin(story_phase * 2.0f + 0.9f) *
                                           story_envelope);
        break;
    case NECK_REACH:
        pose.neck_tilt += static_cast<int>(motion.neck_amplitude * 0.72f * story_envelope);
        pose.neck_lean += static_cast<int>(story_direction * motion.neck_amplitude * 0.20f *
                                           std::sin(progress * kPi) * story_envelope);
        break;
    case NECK_AUTHORED:
    default:
        break;
    }

    switch (motion.head_style) {
    case HEAD_TRACK_NECK: {
        int desired = 90 - static_cast<int>((pose.neck_lean - 90) * 0.48f);
        pose.head_turn = lerp_angle(pose.head_turn, desired, 0.62f * story_envelope);
        break;
    }
    case HEAD_USER_FOCUS:
        pose.head_turn = lerp_angle(pose.head_turn, 90, 0.72f * story_envelope);
        break;
    case HEAD_SIDE_GLANCE:
        pose.head_turn += static_cast<int>(story_direction * motion.head_amplitude *
                                           story_envelope);
        break;
    case HEAD_AUTHORED:
    default:
        break;
    }

    // Use the decoded PCM envelope, onset and playback position to let the
    // neck perform the sound itself. This layer is intentionally neck/head
    // only: the tail follows emotion and balance on a delayed spatial path,
    // so all five axes never pulse mechanically to the same waveform.
    const AudioMotionData sound = GetAudioMotionData();
    if (sound.playing) {
        float level = clamp01(sound.level);
        float onset = clamp01(sound.attack);
        float sound_progress = sound.duration_ms > 0
            ? clamp01(static_cast<float>(sound.elapsed_ms) /
                      static_cast<float>(sound.duration_ms))
            : 0.0f;
        float sound_arc = std::sin(sound_progress * kPi);
        float sound_seconds = sound.elapsed_ms / 1000.0f;
        float slow_phrase = std::sin(sound_seconds * 2.1f + motion.living_phase);

        switch (motion.action) {
        case DINO_ACTION_PROUD_CALL:
            // Louder calls and fresh syllables lift IO17 farther skyward;
            // total-duration arc keeps a long call broad through its middle.
            pose.neck_tilt -= static_cast<int>(22.0f * level + 12.0f * onset +
                                               8.0f * sound_arc);
            pose.neck_lean += static_cast<int>(slow_phrase * 9.0f * level);
            pose.head_turn -= static_cast<int>(slow_phrase * 5.0f * level);
            break;
        case DINO_ACTION_HAPPY:
            pose.neck_tilt -= static_cast<int>(13.0f * onset + 7.0f * level);
            pose.neck_lean += static_cast<int>(slow_phrase * 7.0f * level);
            break;
        case DINO_ACTION_DISCOVER:
            pose.neck_tilt -= static_cast<int>(15.0f * onset + 5.0f * level);
            pose.head_turn += static_cast<int>(slow_phrase * 7.0f * level);
            break;
        case DINO_ACTION_AFFECTION:
            // A soft sustained coo deepens the approach; its onset lifts the
            // head briefly as if checking the owner's response.
            pose.neck_tilt += static_cast<int>(8.0f * level - 10.0f * onset);
            pose.neck_lean += static_cast<int>(slow_phrase * 5.0f * level);
            break;
        case DINO_ACTION_EAT:
            if (pose.neck_tilt > 128) {
                // Crunch energy drives a small low nod while the slower phrase
                // moves IO16 and IO15 in opposite directions across food spots.
                float graze = std::sin(sound_seconds * 3.2f + motion.living_phase);
                pose.neck_tilt += static_cast<int>(7.0f * onset + 3.0f * level);
                pose.neck_lean += static_cast<int>(graze * (5.0f + 8.0f * level));
                pose.head_turn -= static_cast<int>(graze * (7.0f + 10.0f * level));
            }
            break;
        case DINO_ACTION_LISTEN: {
            // Each footstep onset produces a restrained perk; the slower
            // envelope decides which side the long neck investigates.
            float locate = std::sin(sound_seconds * 1.45f + motion.living_phase);
            pose.neck_tilt -= static_cast<int>(14.0f * onset + 4.0f * level);
            pose.neck_lean += static_cast<int>(locate * 12.0f * level);
            pose.head_turn -= static_cast<int>(locate * 9.0f * level);
            break;
        }
        case DINO_ACTION_STARTLED:
            pose.neck_tilt -= static_cast<int>(12.0f * onset);
            break;
        case DINO_ACTION_SLEEPY:
            pose.neck_tilt += static_cast<int>(3.0f * level * sound_arc);
            break;
        default:
            break;
        }
    }

    // Non-repeating single accents inside the clip. Their independent times,
    // widths and optional absence break the recognisable keyframe template
    // while preserving the selected emotion and continuous trajectories.
    float head_accent = organic_accent(progress, motion.head_accent_at,
                                        motion.head_accent_width);
    float neck_accent = organic_accent(progress, motion.neck_accent_at,
                                        motion.neck_accent_width);
    pose.head_turn += static_cast<int>(motion.head_accent * head_accent);
    pose.neck_lean += static_cast<int>(motion.neck_accent_lr * neck_accent);
    pose.neck_tilt += static_cast<int>(motion.neck_accent_ud * neck_accent);

    pose.tail_ud += static_cast<int>((std::sin(tail_circle) * 11.0f * tail_orbit +
                                     std::sin(t * 3.1f + 1.4f) * 5.0f) * envelope);
    pose.tail_lr += static_cast<int>((std::cos(tail_circle) * 16.0f * tail_orbit +
                                     std::sin(t * 5.9f - 0.9f) * 9.0f +
                                     std::sin(t * 11.2f) * 2.4f) * envelope);

    // 尾巴不是固定模板：同一情绪会随机使用宽扫、画圆、单侧轻甩、
    // 动态配重或原编舞。允许某次只偏一侧，也允许跨越两侧。
    float sweep_envelope = story_envelope;
    float sweep_phase = progress * motion.tail_cycles * 2.0f * kPi;
    float sweep_wave = std::sin(sweep_phase) +
                       0.18f * std::sin(sweep_phase * 2.0f + 0.75f);
    float sweep_direction = motion.mirror ? -1.0f : 1.0f;
    float authored_offset = static_cast<float>(pose.tail_lr - 90);
    float neck_counterbalance = static_cast<float>(pose.neck_lean - 90) * -0.62f;

    switch (motion.tail_style) {
    case TAIL_CROSS_SWEEP:
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.22f +
                                             sweep_direction * motion.tail_amplitude *
                                                 sweep_wave * sweep_envelope);
        // IO18跨中位宽扫时，IO8走相差约四分之一相位的高低弧线。
        // 中心不在极限，高点只短暂经过，不会一直顶在180°。
        pose.tail_ud = lerp_angle(
            pose.tail_ud,
            72 + static_cast<int>(motion.tail_amplitude * 0.42f *
                                  std::sin(sweep_phase - 0.9f)),
            0.68f * sweep_envelope);
        break;
    case TAIL_CIRCLE: {
        float circle = sweep_phase + 0.45f;
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.18f +
                                             sweep_direction * motion.tail_amplitude *
                                                 std::cos(circle) * sweep_envelope);
        int circle_center = lerp_angle(pose.tail_ud, 76, 0.72f * sweep_envelope);
        pose.tail_ud = circle_center +
                       static_cast<int>(motion.tail_amplitude * 0.62f *
                                        std::sin(circle) * sweep_envelope);
        break;
    }
    case TAIL_SIDE_FLICK: {
        float side_hold = sweep_direction * (18.0f + motion.tail_amplitude * 0.22f);
        float flick = sweep_direction * motion.tail_amplitude * 0.32f *
                      std::sin(sweep_phase * 1.7f + 0.4f);
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.20f +
                                             (side_hold + flick) * sweep_envelope);
        // 侧甩不是在固定高度做“雨刷”：先略放低，甩到外侧时再抬一点。
        int flick_height = 82 + static_cast<int>(18.0f *
            std::sin(sweep_phase * 0.72f + 1.0f));
        pose.tail_ud = lerp_angle(pose.tail_ud, flick_height,
                                  0.48f * sweep_envelope);
        break;
    }
    case TAIL_COUNTERWEIGHT: {
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.14f +
                                             neck_counterbalance * sweep_envelope +
                                             sweep_direction * motion.tail_amplitude * 0.18f *
                                                 sweep_wave * sweep_envelope);
        // 配重高度同时参考脖子上下位置，并叠加独立慢相位。
        int balance_height = 78 - static_cast<int>((pose.neck_tilt - 90) * 0.30f) +
                             static_cast<int>(14.0f *
                                 std::sin(sweep_phase * 0.58f + 2.0f));
        pose.tail_ud = lerp_angle(pose.tail_ud, balance_height,
                                  0.50f * sweep_envelope);
        break;
    }
    case TAIL_LOW_SWEEP: {
        // 明确的“尾巴放下来再左右晃”：TUD越大代表实机IO8越低。
        // 高度中心在内部92°～114°之间缓慢移动（实机约88°～66°），
        // 左右轴仍完成宽阔跨中位，且两轴不共用同一相位。
        float low_phase = sweep_phase * 0.82f + 0.35f;
        float low_lr = std::sin(low_phase) +
                       0.16f * std::sin(low_phase * 2.2f + 0.8f);
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.14f +
                                             sweep_direction * motion.tail_amplitude * 0.62f *
                                                 low_lr * sweep_envelope);
        int low_center = 103 + static_cast<int>(11.0f *
            std::sin(sweep_phase * 0.43f + 0.9f));
        int low_height = low_center + static_cast<int>(12.0f *
            std::sin(low_phase - 1.05f));
        pose.tail_ud = lerp_angle(pose.tail_ud, low_height,
                                  0.86f * sweep_envelope);
        break;
    }
    case TAIL_HIGH_HOLD: {
        int high_angle = 24;
        if (motion.action == DINO_ACTION_HAPPY ||
            motion.action == DINO_ACTION_PROUD_CALL)
            high_angle = 14;
        else if (motion.action == DINO_ACTION_LISTEN)
            high_angle = 20;
        // 高位也有一次缓慢“蓄力—上翘—回落”，不把IO8长期锁在180°。
        int breathing_height = high_angle + static_cast<int>(18.0f *
            (0.5f + 0.5f * std::sin(sweep_phase * 0.54f + 0.8f)));
        pose.tail_ud = lerp_angle(pose.tail_ud, breathing_height,
                                  0.78f * sweep_envelope);
        // 高位尾姿围绕真正的90°中位缓慢跨越两侧，不固定挂在某一边。
        float gentle_sway = std::sin(sweep_phase * 0.72f + 0.5f);
        pose.tail_lr = 90 + static_cast<int>(authored_offset * 0.16f +
                                             sweep_direction * motion.tail_amplitude * 0.30f *
                                                 gentle_sway * sweep_envelope);
        break;
    }
    case TAIL_AUTHORED:
    default:
        // 保留关键帧的不对称情绪轨迹，只叠加很轻的头颈配重。
        pose.tail_lr += static_cast<int>(neck_counterbalance * 0.12f * sweep_envelope);
        break;
    }

    float tail_accent = organic_accent(progress, motion.tail_accent_at,
                                        motion.tail_accent_width);
    pose.tail_lr += static_cast<int>(motion.tail_accent_lr * tail_accent);
    pose.tail_ud += static_cast<int>(motion.tail_accent_ud * tail_accent);

    // 恐龙式动态配重：脖子越向前下探，尾巴越上提；反向动作则略放低。
    pose.tail_ud -= static_cast<int>((pose.neck_tilt - 90) * 0.10f);

    if (motion.action == DINO_ACTION_EAT && pose.neck_tilt > 135) {
        // 没有下颌舵机，用低位短促点头模拟咀嚼；只在靠近食物时出现。
        float chew = std::sin(t * 8.0f);
        pose.neck_tilt += static_cast<int>(4.5f * chew * envelope);
        pose.neck_lean += static_cast<int>(2.5f * std::sin(t * 4.0f + 0.7f) * envelope);
        pose.head_turn -= static_cast<int>(1.5f * chew * envelope);
    }
}

static bool sample_motion(MotionState &motion, uint32_t now_ms, DinoPose &output) {
    if (!motion.active || !motion.clip) return false;

    uint32_t content_total = clip_content_duration_ms(motion);
    uint32_t total = kEngageMs + content_total + motion.settle_ms;
    uint32_t elapsed = now_ms - motion.start_ms;
    if (elapsed >= total) {
        output = clip_settled_pose(motion);
        motion.active = false;
        return true;
    }

    DinoPose attention_pose = clip_attention_pose(motion);
    if (elapsed < kEngageMs) {
        output = lerp_pose(motion.start_pose, attention_pose,
                           smoothstep(static_cast<float>(elapsed) / kEngageMs));
        clamp_pose(output);
        return true;
    }

    uint32_t action_elapsed = elapsed - kEngageMs;

    if (action_elapsed >= content_total) {
        DinoPose final_pose = clip_final_pose(motion);
        DinoPose settled_pose = clip_settled_pose(motion);
        float settle_x = static_cast<float>(action_elapsed - content_total) /
                         static_cast<float>(motion.settle_ms);
        float amount = smoothstep(settle_x);
        output = lerp_pose(final_pose, settled_pose, amount);
        // 头部先多看用户一会儿，再比身体稍晚回正；到收势末端严格为90°。
        float head_x = clamp01((settle_x - 0.12f) / 0.88f);
        output.head_turn = lerp_angle(final_pose.head_turn, settled_pose.head_turn,
                                      smoothstep(head_x));
        clamp_pose(output);
        return true;
    }

    DinoPose from = attention_pose;
    DinoPose before_from = from;
    uint32_t frame_start = 0;
    for (uint8_t i = 0; i < motion.clip->frame_count; ++i) {
        const Keyframe &frame = motion.clip->frames[i];
        DinoPose target = resolve_target(frame.target, from, motion.mirror,
                                         motion.intensity);
        uint32_t duration = frame_duration_ms(motion, frame);
        uint32_t frame_end = frame_start + duration;
        if (action_elapsed < frame_end) {
            float position = static_cast<float>(action_elapsed - frame_start) /
                             static_cast<float>(duration);
            if (frame.ease == EASE_SMOOTH) {
                DinoPose after_target = target;
                if (i + 1 < motion.clip->frame_count)
                    after_target = resolve_target(motion.clip->frames[i + 1].target,
                                                  target, motion.mirror,
                                                  motion.intensity);
                output = catmull_pose(before_from, from, target, after_target, position);
            } else {
                output = lerp_pose(from, target, ease_value(frame.ease, position));
            }
            apply_living_motion(motion, now_ms, action_elapsed, content_total, output);
            clamp_pose(output);
            return true;
        }
        before_from = from;
        from = target;
        frame_start = frame_end;
    }

    output = from;
    return true;
}

// -------------------------------------------------------------------------
// Coherent idle personalities. A preset owns all five axes so random choices
// cannot make the head and neck look in contradictory directions.
// -------------------------------------------------------------------------

static constexpr BehaviorParam kBehavior[BEHAVIOR_COUNT] = {
    // Default calm pose: IO17=70°, neck raised 20° from geometric centre.
    {{70, 90, 90, 104, 90}, {14, 12, 8, 22, 40}, 3.4f, 4.0f, 2.6f},
    {{98, 110, 102, 92, 94}, {18, 20, 16, 26, 56}, 3.0f, 3.4f, 2.4f},
    {{98, 70, 78, 92, 86}, {18, 20, 16, 26, 56}, 3.0f, 3.4f, 2.4f},
    {{102, 90, 90, 76, 94}, {36, 48, 26, 42, 100}, 2.2f, 2.6f, 1.4f},
    {{108, 90, 90, 60, 92}, {24, 32, 20, 38, 74}, 3.6f, 3.9f, 3.2f},
    {{70, 108, 94, 118, 90}, {8, 10, 6, 14, 28}, 5.5f, 7.0f, 6.0f},
};

static float organic_sin(float phase) {
    float x = phase * 2.0f * kPi;
    return std::sin(x) + 0.12f * std::sin(2.0f * x + 0.5f) +
           0.05f * std::sin(3.0f * x + 1.2f);
}

static float motion_wave(float seconds, float period, float phase) {
    float speed = g_hard_swing ? HARD_SWING_SPEED_X : 1.0f;
    float p = std::fmod(seconds * speed / period + phase, 1.0f);
    if (p < 0.0f) p += 1.0f;
    float value = organic_sin(p);
    if (g_hard_swing) {
        float magnitude = std::fabs(value);
        magnitude = magnitude * magnitude * (3.0f - 2.0f * std::min(magnitude, 1.0f));
        value = std::copysign(magnitude, value);
    }
    return value;
}

static float staged_ramp(float x, float start, float end) {
    if (end <= start) return x >= end ? 1.0f : 0.0f;
    return smoothstep((x - start) / (end - start));
}

static float staged_pulse(float x, float start, float end) {
    float local = clamp01((x - start) / std::max(0.001f, end - start));
    if (x <= start || x >= end) return 0.0f;
    return std::sin(local * kPi);
}

static const char *nature_scene_name(NatureSceneKind scene) {
    switch (scene) {
    case NATURE_HORIZON_SCAN: return "horizon-scan";
    case NATURE_RAIN_LISTEN: return "rain-listen";
    case NATURE_LOW_DRINK: return "low-drink";
    case NATURE_LOOK_UP: return "look-up";
    case NATURE_RAIN_SHAKE: return "rain-shake";
    default: return "unknown";
    }
}

static void start_nature_scene(NatureSceneState &scene, uint32_t now_ms,
                               const DinoPose &current_pose) {
    // Shuffle all five one-shot scenes. A customer sees the complete natural
    // repertoire before any scene can return, while order changes each cycle.
    static NatureSceneKind bag[NATURE_SCENE_COUNT]{};
    static uint8_t bag_cursor = NATURE_SCENE_COUNT;
    if (bag_cursor >= NATURE_SCENE_COUNT) {
        for (uint8_t i = 0; i < NATURE_SCENE_COUNT; ++i)
            bag[i] = static_cast<NatureSceneKind>(i);
        for (int i = NATURE_SCENE_COUNT - 1; i > 0; --i) {
            int j = irnd(i + 1);
            std::swap(bag[i], bag[j]);
        }
        if (NATURE_SCENE_COUNT > 1 && bag[0] == scene.current) {
            int swap_index = 1 + irnd(NATURE_SCENE_COUNT - 1);
            std::swap(bag[0], bag[swap_index]);
        }
        bag_cursor = 0;
    }
    NatureSceneKind selected = bag[bag_cursor++];

    scene.previous = scene.current;
    scene.current = selected;
    scene.from = current_pose;
    scene.start_ms = now_ms;
    scene.head_phase = irnd(628) / 100.0f;
    scene.tail_phase = irnd(628) / 100.0f;

    // 方向保持随机，但不允许三段连续偏向同一侧。
    static int previous_direction = 1;
    static uint8_t same_direction_count = 0;
    int direction = irnd(2) ? 1 : -1;
    if (direction == previous_direction) {
        ++same_direction_count;
        if (same_direction_count >= 2) {
            direction = -previous_direction;
            same_direction_count = 0;
        }
    } else {
        same_direction_count = 0;
    }
    previous_direction = direction;
    scene.direction = direction;

    switch (scene.current) {
    case NATURE_HORIZON_SCAN: scene.duration_ms = static_cast<uint16_t>(irand(6800, 9500)); break;
    case NATURE_RAIN_LISTEN: scene.duration_ms = static_cast<uint16_t>(irand(4200, 6500)); break;
    case NATURE_LOW_DRINK: scene.duration_ms = static_cast<uint16_t>(irand(6500, 9000)); break;
    case NATURE_LOOK_UP: scene.duration_ms = static_cast<uint16_t>(irand(5200, 7600)); break;
    case NATURE_RAIN_SHAKE: scene.duration_ms = static_cast<uint16_t>(irand(3500, 4800)); break;
    default: scene.duration_ms = 6500; break;
    }
    ESP_LOGI(TAG, "Nature: %s dir=%d duration=%ums", nature_scene_name(scene.current),
             scene.direction, static_cast<unsigned>(scene.duration_ms));
}

static DinoPose sample_nature_scene(const NatureSceneState &scene, uint32_t now_ms) {
    uint32_t elapsed = now_ms - scene.start_ms;
    float x = clamp01(static_cast<float>(elapsed) /
                      static_cast<float>(std::max<uint16_t>(1, scene.duration_ms)));
    float seconds = elapsed / 1000.0f;
    float direction = static_cast<float>(scene.direction);

    // 头和尾巴只共享“场景意图”，不共享波形、周期或相位。
    float head_micro = 3.2f * std::sin(seconds * 2.0f * kPi / 5.7f + scene.head_phase) +
                       1.4f * std::sin(seconds * 2.0f * kPi / 2.3f + 0.4f);
    float tail_micro = 6.0f * std::sin(seconds * 2.0f * kPi / 7.9f + scene.tail_phase) +
                       2.0f * std::sin(seconds * 2.0f * kPi / 3.7f + 1.1f);
    DinoPose target{90, 90, 90, 52, 90};

    switch (scene.current) {
    case NATURE_HORIZON_SCAN: {
        float neck_move = staged_ramp(x, 0.12f, 0.90f);
        float head_move = staged_ramp(x, 0.05f, 0.58f);       // 头先到远端
        float tail_move = staged_ramp(x, 0.30f, 0.88f);       // 尾巴明显晚于头颈
        target.neck_tilt = 58 + static_cast<int>(42.0f * std::sin(x * kPi));
        target.neck_lean = 90 + static_cast<int>(direction * (-62.0f + 130.0f * neck_move));
        target.head_turn = 90 + static_cast<int>(direction * (-36.0f + 70.0f * head_move) +
                                                 head_micro);
        target.tail_ud = 36 + static_cast<int>(20.0f *
            std::sin(tail_move * 2.0f * kPi - 0.8f) *
            staged_pulse(x, 0.22f, 0.96f));
        target.tail_lr = 90 + static_cast<int>(direction * (50.0f - 100.0f * tail_move) +
                                               tail_micro);
        break;
    }
    case NATURE_RAIN_LISTEN: {
        float head_notice = staged_ramp(x, 0.04f, 0.18f);
        float head_release = staged_ramp(x, 0.70f, 0.94f);
        float neck_follow = staged_ramp(x, 0.20f, 0.58f);
        float tail_brace = staged_ramp(x, 0.43f, 0.78f);
        float tail_story = staged_ramp(x, 0.38f, 0.94f);
        float tail_window = staged_pulse(x, 0.36f, 0.99f);
        target.neck_tilt = 72 - static_cast<int>(16.0f * neck_follow);
        target.neck_lean = 90 + static_cast<int>(direction * 42.0f * neck_follow);
        target.head_turn = 90 + static_cast<int>(direction * 46.0f * head_notice *
                                                 (1.0f - head_release) + head_micro);
        target.tail_ud = 48 - static_cast<int>(20.0f * tail_brace) +
                         static_cast<int>(16.0f * std::sin(tail_story * 2.0f * kPi + 0.6f) *
                                          tail_window);
        target.tail_lr = 90 + static_cast<int>(direction * 62.0f *
                         std::sin(tail_story * 1.7f * kPi - 0.65f) * tail_window) +
                         static_cast<int>(tail_micro * 0.45f);
        break;
    }
    case NATURE_LOW_DRINK: {
        float descend = staged_ramp(x, 0.04f, 0.36f);
        float drink_window = staged_pulse(x, 0.32f, 0.94f);
        float drink = std::sin((x - 0.34f) * 5.0f * kPi) * drink_window;
        float tail_lift = staged_ramp(x, 0.28f, 0.64f);
        float tail_story = staged_ramp(x, 0.34f, 0.92f);
        target.neck_tilt = 108 + static_cast<int>(68.0f * descend + 6.0f * drink);
        target.neck_lean = 90 + static_cast<int>(direction * 34.0f * std::sin(x * kPi));
        // 喝水期间两次抬眼，节奏与颈部点水不同。
        target.head_turn = 90 + static_cast<int>(direction *
            (22.0f * staged_pulse(x, 0.13f, 0.42f) -
             16.0f * staged_pulse(x, 0.61f, 0.91f)) + head_micro);
        target.tail_ud = 58 - static_cast<int>(30.0f * tail_lift) +
                         static_cast<int>(11.0f * std::sin(tail_story * 2.0f * kPi + 0.9f) *
                                          drink_window);
        target.tail_lr = 90 + static_cast<int>(direction * 48.0f *
                         std::sin(tail_story * 1.6f * kPi) * drink_window) +
                         static_cast<int>(tail_micro * 0.55f);
        break;
    }
    case NATURE_LOOK_UP: {
        float rise = staged_ramp(x, 0.07f, 0.48f);
        float relax = staged_ramp(x, 0.78f, 0.98f);
        float head_search = staged_ramp(x, 0.50f, 0.72f);     // 抬稳后头才寻找
        float tail_sweep = staged_pulse(x, 0.54f, 0.98f);     // 尾巴最后画一次空间圆
        float tail_progress = staged_ramp(x, 0.54f, 0.98f);
        float tail_circle = tail_progress * 2.0f * kPi;
        target.neck_tilt = 164 - static_cast<int>(122.0f * rise) +
                           static_cast<int>(36.0f * relax);
        target.neck_lean = 90 + static_cast<int>(direction * (34.0f - 58.0f * rise));
        target.head_turn = 90 + static_cast<int>(direction * 38.0f * head_search + head_micro);
        target.tail_ud = 46 - static_cast<int>(18.0f * rise) +
                         static_cast<int>(24.0f * std::sin(tail_circle) * tail_sweep) +
                         static_cast<int>(10.0f * relax);
        target.tail_lr = 90 + static_cast<int>(direction * 64.0f *
                         std::cos(tail_circle) * tail_sweep) +
                         static_cast<int>(tail_micro * 0.50f);
        break;
    }
    case NATURE_RAIN_SHAKE: {
        float head_local = clamp01((x - 0.07f) / 0.50f);
        float head_envelope = staged_pulse(x, 0.07f, 0.57f) * (1.0f - 0.45f * head_local);
        float neck_local = clamp01((x - 0.18f) / 0.58f);
        float neck_envelope = staged_pulse(x, 0.18f, 0.76f);
        float tail_once = staged_pulse(x, 0.54f, 0.96f);
        target.neck_tilt = 94 - static_cast<int>(22.0f * staged_pulse(x, 0.05f, 0.86f));
        target.neck_lean = 90 + static_cast<int>(direction * 22.0f *
                            std::sin(neck_local * 3.0f * kPi) * neck_envelope);
        target.head_turn = 90 + static_cast<int>(direction * 52.0f *
                            std::sin(head_local * 6.0f * kPi) * head_envelope);
        target.tail_ud = 30 - static_cast<int>(10.0f * tail_once);
        target.tail_lr = 90 - static_cast<int>(direction * 66.0f * tail_once) +
                         static_cast<int>(tail_micro * 0.35f);
        break;
    }
    default:
        break;
    }

    // 场景切换采用约1.1秒连续过渡，但新场景内部各部位仍按自己的延迟启动。
    float enter = smoothstep(static_cast<float>(elapsed) / 1100.0f);
    return lerp_pose(scene.from, target, enter);
}

static DinoPose sample_behavior_mode(BehaviorMode mode, uint32_t now_ms) {
    const BehaviorParam &p = kBehavior[mode];
    float seconds = now_ms / 1000.0f;
    float nt = motion_wave(seconds, p.neck_period, 0.02f);
    float nl = motion_wave(seconds, p.neck_period, 0.27f);
    float ht = motion_wave(seconds, p.head_period, 0.13f);
    float tu = motion_wave(seconds, p.tail_period, 0.31f);
    float tl = motion_wave(seconds, p.tail_period, 0.56f);
    return {
        p.center.neck_tilt + static_cast<int>(p.amplitude.neck_tilt * nt),
        p.center.neck_lean + static_cast<int>(p.amplitude.neck_lean * nl),
        p.center.head_turn + static_cast<int>(p.amplitude.head_turn * ht),
        p.center.tail_ud + static_cast<int>(p.amplitude.tail_ud * tu),
        p.center.tail_lr + static_cast<int>(p.amplitude.tail_lr * tl),
    };
}

static DinoPose sample_behavior(const BehaviorState &state, uint32_t now_ms) {
    DinoPose current = sample_behavior_mode(state.current, now_ms);
    uint32_t elapsed = now_ms - state.changed_ms;
    constexpr uint32_t kBlendMs = 1400;
    if (state.previous == state.current || elapsed >= kBlendMs) return current;
    DinoPose previous = sample_behavior_mode(state.previous, now_ms);
    return lerp_pose(previous, current, smoothstep(static_cast<float>(elapsed) / kBlendMs));
}

static void change_behavior(BehaviorState &state, BehaviorMode next, uint32_t now_ms) {
    if (state.current == next) return;
    state.previous = state.current;
    state.current = next;
    state.changed_ms = now_ms;
}

static void start_life_pulse(LifePulse &pulse, uint32_t now_ms) {
    static LifePulseKind bag[4]{};
    static uint8_t bag_cursor = 4;
    static LifePulseKind previous_kind = LIFE_PERK;
    if (bag_cursor >= 4) {
        for (uint8_t i = 0; i < 4; ++i)
            bag[i] = static_cast<LifePulseKind>(i);
        for (int i = 3; i > 0; --i) {
            int j = irnd(i + 1);
            std::swap(bag[i], bag[j]);
        }
        if (bag[0] == previous_kind)
            std::swap(bag[0], bag[1 + irnd(3)]);
        bag_cursor = 0;
    }
    pulse.kind = bag[bag_cursor++];
    previous_kind = pulse.kind;

    // Direction is genuinely random; only a third consecutive same-side
    // pulse is corrected. This avoids both long bias and obvious L-R-L-R order.
    static int previous_direction = 1;
    static uint8_t same_direction_count = 0;
    pulse.direction = irnd(2) ? 1 : -1;
    if (pulse.direction == previous_direction) {
        ++same_direction_count;
        if (same_direction_count >= 2) {
            pulse.direction = -previous_direction;
            same_direction_count = 0;
        }
    } else {
        same_direction_count = 0;
    }
    previous_direction = pulse.direction;
    pulse.start_ms = now_ms;
    pulse.active = true;

    switch (pulse.kind) {
    case LIFE_GLANCE:
        pulse.duration_ms = static_cast<uint16_t>(irand(900, 1550));
        pulse.strength = irand(20, 38);
        break;
    case LIFE_TAIL_BURST:
        pulse.duration_ms = static_cast<uint16_t>(irand(1250, 1900));
        pulse.strength = irand(52, 82);
        break;
    case LIFE_SNIFF:
        pulse.duration_ms = static_cast<uint16_t>(irand(850, 1350));
        pulse.strength = irand(14, 27);
        break;
    case LIFE_PERK:
    default:
        pulse.duration_ms = static_cast<uint16_t>(irand(900, 1450));
        pulse.strength = irand(18, 32);
        break;
    }
}

static float life_envelope(float x) {
    if (x < 0.18f) return smoothstep(x / 0.18f);
    if (x < 0.62f) return 1.0f;
    return smoothstep((1.0f - x) / 0.38f);
}

static void apply_life_pulse(LifePulse &pulse, uint32_t now_ms, DinoPose &pose) {
    if (!pulse.active) return;
    uint32_t elapsed = now_ms - pulse.start_ms;
    if (elapsed >= pulse.duration_ms) {
        pulse.active = false;
        return;
    }

    float x = static_cast<float>(elapsed) / pulse.duration_ms;
    float envelope = life_envelope(x);
    float direction = static_cast<float>(pulse.direction);
    switch (pulse.kind) {
    case LIFE_GLANCE:
        // 眼神先到，颈部只跟一部分，尾巴反向微调保持身体平衡。
        pose.head_turn += static_cast<int>(direction * pulse.strength * envelope);
        pose.neck_lean += static_cast<int>(direction * pulse.strength * 0.42f *
                                           smoothstep(std::max(0.0f, x - 0.08f) / 0.25f) *
                                           life_envelope(x));
        pose.tail_lr -= static_cast<int>(direction * pulse.strength * 0.30f * envelope);
        break;
    case LIFE_TAIL_BURST: {
        // 一次中低位摇尾：与“精神一振”的上提尾姿形成随机高低对比。
        float wag = std::sin(x * 4.5f * kPi) * std::sin(x * kPi);
        pose.tail_lr = lerp_angle(pose.tail_lr, 90, envelope * 0.72f) +
                       static_cast<int>(direction * pulse.strength * wag);
        pose.tail_ud += static_cast<int>(18.0f * std::sin(x * kPi));
        pose.neck_lean -= static_cast<int>(direction * 7.0f * wag);
        break;
    }
    case LIFE_SNIFF: {
        float sniff = std::sin(x * 4.0f * kPi) * std::sin(x * kPi);
        pose.neck_tilt += static_cast<int>(pulse.strength * sniff);
        pose.head_turn += static_cast<int>(direction * 7.0f * envelope);
        pose.tail_lr -= static_cast<int>(direction * 12.0f * sniff);
        break;
    }
    case LIFE_PERK:
        pose.neck_tilt -= static_cast<int>(pulse.strength * envelope);
        pose.neck_lean += static_cast<int>(direction * pulse.strength * 0.55f * envelope);
        pose.tail_ud -= static_cast<int>(pulse.strength * 0.8f * envelope);
        pose.tail_lr += static_cast<int>(direction * pulse.strength * 1.1f * envelope);
        pose.head_turn -= static_cast<int>(direction * pulse.strength * 0.35f * envelope);
        break;
    }
}

static BehaviorMode random_idle_behavior() {
    static constexpr BehaviorMode modes[] = {
        BEHAVIOR_CALM, BEHAVIOR_CALM, BEHAVIOR_CURIOUS_LEFT,
        BEHAVIOR_CURIOUS_RIGHT, BEHAVIOR_PLAYFUL, BEHAVIOR_PROUD,
        BEHAVIOR_SLEEP,
    };
    return modes[irnd(sizeof(modes) / sizeof(modes[0]))];
}

static DinoAction action_for_sound(int sound_index) {
    /* 取文件条目快照（TOC 可能被并发上传/删除改写，get_name 返回的内部
     * 指针不宜持有；整体拷贝一次再慢慢匹配最稳）。 */
    flash_audio_info_t info{};
    if (flash_audio_get_file_info(sound_index, &info) != ESP_OK)
        return DINO_ACTION_PROUD_CALL;
    const char *name = info.name;
    int duration = (int)info.duration_ms;

    if (std::strstr(name, "咀嚼") || std::strstr(name, "eat") || std::strstr(name, "chew"))
        return DINO_ACTION_EAT;
    if (std::strstr(name, "脚步") || std::strstr(name, "step") || std::strstr(name, "foot"))
        return DINO_ACTION_LISTEN;
    if (std::strstr(name, "入睡") || std::strstr(name, "sleep"))
        return DINO_ACTION_SLEEPY;
    if (std::strstr(name, "警觉") || std::strstr(name, "startle"))
        return DINO_ACTION_STARTLED;
    if (std::strstr(name, "雀跃") || std::strstr(name, "happy") || std::strstr(name, "joy"))
        return DINO_ACTION_HAPPY;
    if (std::strstr(name, "亲近") || std::strstr(name, "comfort") || std::strstr(name, "nuzzle"))
        return DINO_ACTION_AFFECTION;

    // Existing dinosaur call files only carry collection numbers. Their
    // duration still gives them distinct roles: chirp, happy call, or display.
    if (duration > 0 && duration <= 1500) return DINO_ACTION_DISCOVER;
    if (duration > 0 && duration <= 2700) return DINO_ACTION_HAPPY;
    return DINO_ACTION_PROUD_CALL;
}

static bool trigger_random_animal_sound() {
    int index = flash_audio_get_random_in_category("animal");
    return index >= 0 && TriggerDinoAction(action_for_sound(index), index);
}

}  // namespace

bool IsAutoRunRunning() { return g_running; }
void SetAutoRunRunning(bool value) { g_running = value; }
bool IsAutoRunHardSwing() { return g_hard_swing; }
void SetAutoRunHardSwing(bool value) { g_hard_swing = value; }

bool TriggerDinoAction(DinoAction action, int sound_index) {
    if (!g_action_queue || action < 0 || action >= DINO_ACTION_COUNT) return false;
    ActionRequest request{action, sound_index};
    if (xQueueSend(g_action_queue, &request, 0) == pdTRUE) return true;

    // Prefer the newest interaction when several sensors fire together.
    ActionRequest discarded{};
    xQueueReceive(g_action_queue, &discarded, 0);
    return xQueueSend(g_action_queue, &request, 0) == pdTRUE;
}

bool TriggerDinoSoundAction(int sound_index) {
    if (sound_index < 0 || sound_index >= flash_audio_get_file_count()) return false;
    return TriggerDinoAction(action_for_sound(sound_index), sound_index);
}

bool TriggerDinoActionWithAutoSound(DinoAction action) {
    if (action < 0 || action >= DINO_ACTION_COUNT) return false;
    int selected = -1;
    int candidates = 0;
    int total = flash_audio_get_file_count();
    for (int i = 0; i < total; ++i) {
        flash_audio_info_t info{};
        if (flash_audio_get_file_info(i, &info) != ESP_OK) continue;
        if (std::strcmp(info.category, "animal") != 0) continue;
        // action_for_sound 内部会再取一次 info；这里只需按名称/时长初判，
        // 直接内联判断避免双次拷贝。
        // （保持与 action_for_sound 相同的匹配规则）
        const char *name = info.name;
        const int duration = (int)info.duration_ms;
        DinoAction act;
        if (std::strstr(name, "咀嚼") || std::strstr(name, "eat") || std::strstr(name, "chew"))
            act = DINO_ACTION_EAT;
        else if (std::strstr(name, "脚步") || std::strstr(name, "step") || std::strstr(name, "foot"))
            act = DINO_ACTION_LISTEN;
        else if (std::strstr(name, "入睡") || std::strstr(name, "sleep"))
            act = DINO_ACTION_SLEEPY;
        else if (std::strstr(name, "警觉") || std::strstr(name, "startle"))
            act = DINO_ACTION_STARTLED;
        else if (std::strstr(name, "雀跃") || std::strstr(name, "happy") || std::strstr(name, "joy"))
            act = DINO_ACTION_HAPPY;
        else if (std::strstr(name, "亲近") || std::strstr(name, "comfort") || std::strstr(name, "nuzzle"))
            act = DINO_ACTION_AFFECTION;
        else if (duration > 0 && duration <= 1500)
            act = DINO_ACTION_DISCOVER;
        else if (duration > 0 && duration <= 2700)
            act = DINO_ACTION_HAPPY;
        else
            act = DINO_ACTION_PROUD_CALL;
        if (act != action) continue;
        ++candidates;
        if (irnd(candidates) == 0) selected = i;
    }
    return TriggerDinoAction(action, selected);
}

bool TriggerDinoGreeting() {
    int selected = -1;
    int candidates = 0;
    int total = flash_audio_get_file_count();
    for (int i = 0; i < total; ++i) {
        flash_audio_info_t info{};
        if (flash_audio_get_file_info(i, &info) != ESP_OK) continue;
        const char *name = info.name;
        int duration = (int)info.duration_ms;
        bool is_call = std::strstr(name, "叫声") || std::strstr(name, "call") ||
                       std::strstr(name, "roar");
        if (is_call && duration > 0 && duration <= 5000) {
            ++candidates;
            if (irnd(candidates) == 0) selected = i;  // reservoir sampling
        }
    }
    if (selected < 0) selected = flash_audio_get_random_in_category("animal");
    return selected >= 0 && TriggerDinoSoundAction(selected);
}

static void auto_run_task(void *) {
    int total = flash_audio_get_file_count();
    ESP_LOGI(TAG, "Audio: %d total (%d animal, %d ambient)", total,
             flash_audio_get_count_by_category("animal"),
             flash_audio_get_count_by_category("ambient"));

    uint32_t tick = 0;
    DinoPose current_pose{SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                          SERVO_HEAD_TURN_DEFAULT, 90, SERVO_TAIL_LR_DEFAULT};
    MotionState motion;
    BehaviorState behavior;
    NatureSceneState nature_scene;
    PoseFade rejoin;
    LifePulse life_pulse;

    bool nature_on = false;
    bool was_audio_playing = false;
    uint32_t next_sound = 5000 / kTickMs;
    uint32_t next_idle = static_cast<uint32_t>(irand(4, 9) * 1000 / kTickMs);
    uint32_t next_spontaneous = static_cast<uint32_t>(irand(8, 16) * 1000 / kTickMs);
    uint32_t next_relax = 300000 / kTickMs;
    uint32_t next_nature_variation = 0;
    uint32_t next_life_pulse_ms = 500;

    while (true) {
        uint32_t now_ms = tick * kTickMs;

        if (!g_running) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        ActionRequest request{};
        if (xQueueReceive(g_action_queue, &request, 0) == pdTRUE) {
            if (nature_on) {
                nature_on = false;
                next_relax = tick + static_cast<uint32_t>(irand(300, 600) * 1000 / kTickMs);
            }
            if (request.sound_index >= 0 && !IsAudioPlaying())
                PlayDinoSound(request.sound_index);
            const MotionClip *avoid = request.action == motion.action ? motion.clip : nullptr;
            start_motion(motion, request.action, now_ms, current_pose, avoid);
            rejoin.active = false;
        }

        bool audio_playing = IsAudioPlaying();

        // A long ambient track is an occasional calm scene, not a random
        // collection of unrelated head/neck/tail modes.
        if (!nature_on && !audio_playing && !motion.active && tick >= next_relax) {
            int ambient = flash_audio_get_random_in_category("ambient");
            if (ambient >= 0 && PlayDinoSound(ambient)) {
                nature_on = true;
                start_nature_scene(nature_scene, now_ms, current_pose);
                next_nature_variation = tick + nature_scene.duration_ms / kTickMs;
                ESP_LOGI(TAG, "Nature scene ON");
            } else {
                next_relax = tick + 300000 / kTickMs;
            }
        }

        if (nature_on && audio_playing && tick >= next_nature_variation) {
            // 每段只演一次且不连续重复；片段自身决定3.5～9.5秒时长。
            start_nature_scene(nature_scene, now_ms, current_pose);
            next_nature_variation = tick + nature_scene.duration_ms / kTickMs;
        }

        if (!audio_playing && was_audio_playing && nature_on) {
            nature_on = false;
            rejoin.active = true;
            rejoin.start_ms = now_ms;
            rejoin.duration_ms = 1400;
            rejoin.from = current_pose;
            change_behavior(behavior, BEHAVIOR_CALM, now_ms);
            next_relax = tick + static_cast<uint32_t>(irand(300, 600) * 1000 / kTickMs);
            ESP_LOGI(TAG, "Nature scene OFF");
        }
        was_audio_playing = audio_playing;

        // Sound-to-action selection uses filename semantics and duration, not
        // unstable manifest indices.
        if (!nature_on && !audio_playing && !motion.active && tick >= next_sound) {
            trigger_random_animal_sound();
            next_sound = tick + 1000 / kTickMs;
        }
        if (audio_playing || motion.active) {
            next_sound = tick + static_cast<uint32_t>(irand(AUDIO_SILENT_INTERVAL_MIN_S,
                                                            AUDIO_SILENT_INTERVAL_MAX_S) *
                                                        1000 / kTickMs);
            next_idle = tick + static_cast<uint32_t>(irand(4, 9) * 1000 / kTickMs);
            next_spontaneous = tick + static_cast<uint32_t>(irand(8, 16) * 1000 / kTickMs);
        }

        if (!nature_on && !audio_playing && !motion.active && tick >= next_idle) {
            change_behavior(behavior, random_idle_behavior(), now_ms);
            next_idle = tick + static_cast<uint32_t>(irand(4, 8) * 1000 / kTickMs);
        }

        // 非周期“生命脉冲”：每次只做一次观察、摇尾、嗅闻或精神一振，
        // 随后完整回落。它打破待机正弦的可预测性，也不会无休止重复。
        if (nature_on || motion.active) {
            life_pulse.active = false;
            next_life_pulse_ms = now_ms + static_cast<uint32_t>(irand(700, 1500));
        } else if (!life_pulse.active && now_ms >= next_life_pulse_ms) {
            start_life_pulse(life_pulse, now_ms);
            next_life_pulse_ms = now_ms + life_pulse.duration_ms +
                                 static_cast<uint32_t>(irand(500, 1600));
        }

        // A silent glance or nuzzle keeps the pet alive between vocalizations.
        if (!nature_on && !audio_playing && !motion.active && tick >= next_spontaneous) {
            if (irnd(100) < 65) {
                DinoAction spontaneous = irnd(4) == 0 ? DINO_ACTION_AFFECTION
                                                       : DINO_ACTION_DISCOVER;
                const MotionClip *avoid = spontaneous == motion.action ? motion.clip : nullptr;
                start_motion(motion, spontaneous, now_ms, current_pose, avoid);
                rejoin.active = false;
            }
            next_spontaneous = tick + static_cast<uint32_t>(irand(8, 16) * 1000 / kTickMs);
        }

        bool motion_was_active = motion.active;
        const MotionClip *completed_clip = motion.clip;
        DinoAction completed_action = motion.action;
        DinoPose target{};
        bool has_motion_pose = sample_motion(motion, now_ms, target);
        if (motion_was_active && !motion.active) {
            if (audio_playing && completed_clip && completed_clip->continue_with_audio) {
                // Long eating/footstep sounds receive a changing performance:
                // select another clip, direction, strength and tempo instead of
                // looping the same short gesture.
                start_motion(motion, completed_action, now_ms, target, completed_clip);
                rejoin.active = false;
            } else {
                rejoin.active = true;
                rejoin.start_ms = now_ms;
                rejoin.from = target;
                change_behavior(behavior, BEHAVIOR_CALM, now_ms);
            }
        }

        if (!has_motion_pose) {
            target = nature_on ? sample_nature_scene(nature_scene, now_ms)
                               : sample_behavior(behavior, now_ms);
            if (!nature_on && rejoin.active) {
                uint32_t elapsed = now_ms - rejoin.start_ms;
                if (elapsed >= rejoin.duration_ms) {
                    rejoin.active = false;
                } else {
                    target = lerp_pose(rejoin.from, target,
                                       smoothstep(static_cast<float>(elapsed) /
                                                  rejoin.duration_ms));
                }
            }
            if (!nature_on) apply_life_pulse(life_pulse, now_ms, target);
        }

        clamp_pose(target);
        output_pose(target);
        current_pose = target;

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

void InitAutoRun() {
    if (!g_action_queue) g_action_queue = xQueueCreate(4, sizeof(ActionRequest));
    if (!g_action_queue) {
        ESP_LOGE(TAG, "Failed to create action queue");
        return;
    }
    xTaskCreate(auto_run_task, "dino_auto", 6144, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "Auto-run task created with keyframe motion library");
}

#endif  // ENABLE_AUTO_RUN
