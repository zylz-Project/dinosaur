/*
 * chat.h — 小熊猫 LLM 实时语音对话模块
 *
 * 由电源键“双击”触发开始/结束（见 power.cc 的 ChatToggle 调用）。
 * 数据流：板载 ES8311 麦克风(48k) → 抽取到16k → WebSocket → 服务器 ASR/LLM/TTS
 *        → 48k PCM 播放 → 舵机动作（按音频包络 + 情绪）。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化对话模块（创建任务，不连接服务器）。在 app_main 中调用一次。 */
void ChatInit(void);

/** 开始对话会话：连接 WS 并进入聆听状态。返回 true 表示已启动。 */
bool ChatStart(void);

/** 结束对话会话：断开 WS、停止发送、复位动作。 */
void ChatStop(void);

/** 双击电源键回调：切换对话开/关。 */
void ChatToggle(void);

/** 当前是否处于对话会话中。 */
bool ChatIsActive(void);

/** WS 是否已连接且服务端 ready（供网页状态展示）。 */
bool ChatIsReady(void);

/** 情绪驱动参数快照（chat_motion 任务按此驱动舵机；单写多读，免锁）。 */
typedef struct {
    float speed;     /* 尾巴摆动速度倍率 */
    float head_bias; /* 头部角度偏移（正=抬/偏） */
    float lr_bias;   /* 尾巴左右偏移 */
    float ud_bias;   /* 尾巴上下偏移 */
    bool  shake;     /* 是否摇头 */
} chat_emo_bias_t;

chat_emo_bias_t chat_get_emo_snapshot(void);

#ifdef __cplusplus
}
#endif
