# 恐龙动作库与动作执行流程

本文对应当前固件代码，说明两件事：

1. 动作库的数据是怎么组织的；
2. 一个动作从“被触发”到“五路舵机实际运动”经过哪些步骤。

如果只是调整或增加一段编舞，主要看 `main/auto_run_data.cc`；如果要了解队列、随机变化、插值、音频联动和舵机输出，则看 `main/auto_run.cc`。

## 1. 相关文件与职责

| 文件 | 职责 |
|---|---|
| `main/auto_run.h` | 对外动作 API、`DinoAction` 动作类型枚举 |
| `main/auto_run_data.h` | 动作库的数据结构和外部声明 |
| `main/auto_run_data.cc` | 动作库本体：29 段关键帧编舞、8 个动作池 |
| `main/auto_run.cc` | 动作队列、片段抽取、插值、随机变化、音频联动、待机状态机和 50 Hz 输出 |
| `main/servo.h/.cc` | 将最终角度转换为 50 Hz LEDC PWM，真正写入舵机 |
| `main/audio.cc` | 播放声音，并向动作引擎提供 PCM 音量、起音和播放进度 |
| `main/http_server.cc` | 网页的 `/api/action` 动作入口 |
| `main/chat.cc` | 对话期间暂停自主动作，由 `chat_motion` 临时接管舵机 |

模块关系如下：

```text
业务代码 / 网页 / 开机问候
          │
          ▼
  TriggerDinoAction*()          auto_run.h
          │ 非阻塞入队
          ▼
  FreeRTOS ActionRequest 队列   auto_run.cc
          │ dino_auto 每 20 ms 检查
          ▼
  选择动作池中的 MotionClip     auto_run_data.cc
          │
          ▼
  关键帧插值 + 有机变化 + PCM 联动
          │
          ▼
       DinoPose
          │ 角度限制、尾巴上下轴换算
          ▼
  SetServoAngle() × 5           servo.cc
          │
          ▼
       LEDC PWM → 舵机
```

## 2. 五路舵机与动作坐标

动作库中一个姿态用 `DinoPose` 表示，字段顺序固定为：

```cpp
struct DinoPose {
    int neck_tilt;  // IO17，长颈上下：0°最高，180°最低
    int neck_lean;  // IO16，长颈左右：0°右，90°中，180°左
    int head_turn;  // IO15，小头左右：0°右，90°中，180°左
    int tail_ud;    // 动作库内部尾巴上下坐标，0最高，125最低
    int tail_lr;    // IO18，尾巴左右：0°左，90°中，180°右
};
```

其中 `tail_ud` 需要特别注意。它不是 IO8 的真实角度，而是为了让编舞时“数值越小、尾巴越高”更直观而使用的内部坐标。输出时执行：

```text
IO8 实际角度 = 180 - tail_ud
```

因此动作库中的 `tail_ud=0` 对应 IO8 的 180°最高位，`tail_ud=90` 对应 IO8 的 90°，`tail_ud=125` 对应 IO8 的 55°安全最低位。其余四轴在动作库中的值就是舵机角度。

默认实机姿态为 `{70, 90, 90, 90, 90}`，即 IO17 的长颈略微抬起，其余轴在中位。

## 3. 动作库的四层结构

动作数据从小到大分为四层：

```text
DinoPose（五轴姿态）
    └── Keyframe（在指定时间内到达一个姿态）
          └── MotionClip（一串关键帧组成的完整编舞）
                └── ClipSet（同一种情绪/意图的编舞随机池）
```

### 3.1 DinoPose：一个姿态

例如：

```cpp
{62, 118, 124, 32, 64}
```

表示长颈上下到 62°、长颈左右到 118°、小头左右到 124°、内部尾巴上下到 32、尾巴左右到 64°。

某一轴不想在当前关键帧中改变时，使用 `kKeep`：

```cpp
{kKeep, kKeep, 128, kKeep, kKeep}
```

这表示只有小头先转到 128°，其他轴保持上一姿态。利用 `kKeep` 可以做出“头先发现 → 脖子再跟随 → 尾巴最后响应”的错峰动作，避免五个舵机同时启动。

### 3.2 Keyframe：一个关键帧

```cpp
struct Keyframe {
    uint16_t duration_ms; // 从上一姿态运动到本姿态的基础时间
    DinoPose target;      // 目标姿态
    Ease ease;            // 缓动方式
};
```

实际写法：

```cpp
{320, {62, 118, 124, 32, 64}, EASE_SMOOTH}
```

含义是“以该次表演的随机节奏，在约 320 ms 的基础时长内运动到目标姿态”。引擎会根据本次动作的速度和每帧随机 tempo 修正实际时长，所以同一关键帧每次不会完全一样快。

三种缓动方式：

| 缓动 | 用途 |
|---|---|
| `EASE_SMOOTH` | 平滑连续动作；使用 Catmull-Rom 曲线连接前后姿态 |
| `EASE_FAST_OUT` | 快速起动、缓慢收住；适合啄食、惊跳和突然发现 |
| `EASE_LINEAR` | 匀速到达；适合凝视、保持和长鸣停顿 |

### 3.3 MotionClip：一段完整编舞

```cpp
struct MotionClip {
    const char *name;
    const Keyframe *frames;
    uint8_t frame_count;
    bool continue_with_audio;
};
```

文件底部用 `CLIP` 宏把关键帧表注册成片段：

```cpp
CLIP("discover-direct", kDiscoverFrames, false)
```

- `name` 用于串口日志定位当前播放的编舞；
- `frames` 和 `frame_count` 指向整段关键帧；
- `continue_with_audio=true` 表示片段结束但音频还在播放时，再抽取同类的另一段编舞继续演。

当前 `PROUD_CALL`、`EAT` 和 `LISTEN` 池设置了续演，适合长叫声、长咀嚼声和长脚步声；其他动作播完一段后进入收势。

### 3.4 ClipSet：一个动作的编舞池

例如 `DINO_ACTION_DISCOVER` 对应：

```cpp
const MotionClip kDiscoverClips[] = {
    CLIP("discover-direct", kDiscoverFrames, false),
    CLIP("discover-peek", kDiscoverPeekFrames, false),
    CLIP("discover-sky-track", kDiscoverSkyFrames, false),
};
```

全部动作池再按 `DinoAction` 枚举的顺序放进总表 `kActionClips[]`。引擎用枚举值直接作为下标，因此二者顺序必须完全一致。

## 4. 当前动作库目录

`DinoAction` 当前有 8 类动作、共 29 段编舞：

| 数值 | 动作枚举 | 片段数 | 当前片段 |
|---:|---|---:|---|
| 0 | `DINO_ACTION_DISCOVER` | 3 | 正面发现、先缩后探、仰望追踪 |
| 1 | `DINO_ACTION_AFFECTION` | 4 | 鞠躬问候、大弧贴近、脸颊轻蹭、害羞贴近 |
| 2 | `DINO_ACTION_HAPPY` | 4 | 挺胸展示、大斜线摆动、空间画圆、追尾游戏 |
| 3 | `DINO_ACTION_PROUD_CALL` | 4 | 仰天长鸣、高扫巡视、慢速巡视、双段长啸 |
| 4 | `DINO_ACTION_EAT` | 4 | 低啄、先嗅后咬、咬完抬头、贴地横吃 |
| 5 | `DINO_ACTION_LISTEN` | 4 | 定位、循声跟踪、二次确认、高空声源 |
| 6 | `DINO_ACTION_STARTLED` | 3 | 后缩评估、对角弹跳、低伏躲避 |
| 7 | `DINO_ACTION_SLEEPY` | 3 | 慢呼吸趴下、两次点头、绕身入睡 |

同一动作池使用“洗牌袋”抽取：先将池内片段做 Fisher-Yates 随机排列，全部播放一遍后才重新洗牌，同时避免新一轮的第一段与刚结束的片段相同。当前引擎的 `kMaxClipsPerAction` 是 4，所以每个动作池最多会抽取 4 段；若要给同一动作增加第 5 段，必须同步增大该常量。

## 5. 一次动作的完整执行流程

以下以网页触发“高兴”为例。

### 5.1 接收触发

网页发送：

```http
POST /api/action
Content-Type: application/json

{"action":2}
```

`HandleAction()` 检查数值合法且当前不在对话中，然后调用：

```cpp
SetAutoRunRunning(true);
TriggerDinoActionWithAutoSound(DINO_ACTION_HAPPY);
```

动作也可以直接由其他业务代码触发：

```cpp
// 只做动作，不播放声音
TriggerDinoAction(DINO_ACTION_HAPPY);

// 做动作，并从 animal 分类中选择语义匹配的声音
TriggerDinoActionWithAutoSound(DINO_ACTION_HAPPY);

// 已经知道声音索引：根据文件名/时长反选动作，并一起触发
TriggerDinoSoundAction(sound_index);

// 指定动作和指定声音同时触发
TriggerDinoAction(DINO_ACTION_HAPPY, sound_index);
```

这些接口都是非阻塞的。返回 `true` 只表示请求成功进入队列，不表示动作已经播放完成。

### 5.2 请求进入队列

请求被包装成：

```cpp
struct ActionRequest {
    DinoAction action;
    int sound_index; // -1 表示静音动作
};
```

FreeRTOS 队列容量为 4。队列满时会丢弃最旧的一条，再放入最新请求，使最新的触摸或网页交互优先响应。

### 5.3 `dino_auto` 任务取出请求

`InitAutoRun()` 创建 `dino_auto` 任务。任务以 20 ms 为一帧运行，也就是 50 FPS。每轮先检查动作队列：

1. 如果正在运行环境音场景，先退出环境场景；
2. 请求带有效声音且当前没有其他声音时，调用 `PlayDinoSound()`；
3. 调用 `start_motion()` 初始化本次动作；
4. 关闭待机姿态的回接淡入，正式进入互动动作。

### 5.4 初始化本次表演

`start_motion()` 不只是选一张固定动作表，还会为这次表演生成一组稳定的随机参数：

1. 从对应 `ClipSet` 的洗牌袋中抽取一个 `MotionClip`；
2. 根据最近的左右停留偏差决定是否镜像，防止长期偏向同一侧；
3. 为每个关键帧生成独立 tempo；
4. 根据动作类型设置力度、整体速度和收势时间；
5. 随机选择头、颈、尾巴的附加表演风格；
6. 生成一次性的头部回看、脖子犹豫和尾巴补摆；
7. 用近期动作签名避免“同片段、同方向、同风格”短时间内完全重复。

随机值只在动作开始时生成，播放过程中保持不变，因此轨迹连续，不会每 20 ms 随机抖动。

### 5.5 260 ms 互动预备

正式进入关键帧前，引擎先用 `kEngageMs=260` ms 从当前姿态平滑移到该动作的“注意姿态”。例如亲近和高兴会先看向正前方用户，倾听会先收紧身体，受惊会先抬颈并提高尾姿。

这样即使上一个动作停在任意角度，新动作也不会直接从关键帧第一行硬跳过去。

### 5.6 逐帧采样与插值

每个 20 ms 周期，`sample_motion()` 根据已播放时间判断当前处于哪两个关键姿态之间：

- `EASE_SMOOTH` 使用前一姿态、当前姿态、目标姿态和后一姿态做 Catmull-Rom 插值；
- `EASE_FAST_OUT` 和 `EASE_LINEAR` 使用对应缓动后的线性角度插值；
- `kKeep` 在解析目标时继承上一姿态；
- 镜像主要作用于左右轴；
- 力度以 90°为中心放大或缩小动作幅度；
- 最后将五轴限制在机械安全范围内。

### 5.7 叠加“生命感”和音频响应

关键帧只是动作骨架，`apply_living_motion()` 还会叠加：

- 不同周期和相位的头、颈、尾巴有机波形；
- 本次随机选出的宽弧、空间椭圆、前探、跟随视线等头颈风格；
- 尾巴宽扫、画圆、侧甩、配重、高位保持或低位摆动；
- 一次性的头、颈、尾小动作；
- “脖子向前下探，尾巴相应抬起”的动态配重；
- 进食低位时的短促咀嚼点头。

如果声音正在播放，还会读取 `GetAudioMotionData()` 提供的 PCM 快包络、起音强度和播放进度，让头颈对真实声音作出响应。例如长鸣时随音量抬颈，脚步起音时轻微警觉，咀嚼声只在低头进食姿态下驱动小幅点头。尾巴不直接逐采样跟随音量，避免五轴一起机械跳动。

### 5.8 收势与长音频续演

关键帧播放完后：

- 普通片段用约 300～950 ms 平滑收势，头和尾巴左右轴回到 90°；
- 如果 `continue_with_audio=true` 且音频仍未结束，引擎跳过待机，抽取同一动作池的另一片段继续表演；
- 整个动作结束后，再用约 800 ms 从结束姿态回接到平静待机姿态。环境音场景结束时使用的是约 1.4 s 回接。

### 5.9 输出到舵机

最终姿态先执行角度限制和尾巴长期左右偏差校正，再由 `output_pose()` 调用五次 `SetServoAngle()`。`servo.cc` 将 0～180°换算为约 500～2500 μs 的 PWM 脉宽；IO8 还会再次限制在 55～180°安全范围内。

## 6. 如何修改一个现有动作

假设要让“正面发现”的第二次观察更慢：

1. 在 `main/auto_run_data.cc` 找到 `kDiscoverFrames[]`；
2. 找到对应目标姿态；
3. 增大该行的 `duration_ms`，或调整五轴角度；
4. 编译、烧录后观察串口中的 `Action: discover-direct ...` 日志；
5. 先小幅调整，确认没有撞到结构件，再逐步扩大角度。

示例：

```cpp
// 修改前
{440, {70, 150, 56, 28, 146}, EASE_SMOOTH},

// 修改后：观察动作更慢，尾巴左右幅度略收小
{620, {70, 150, 56, 28, 132}, EASE_SMOOTH},
```

调试时建议遵循以下顺序：

1. 先在 Web 舵机滑块中单轴确认安全角度；
2. 再写成 2～3 个低幅关键帧；
3. 确认方向正确后增加幅度；
4. 最后再补 `kKeep`、停顿和其他部位的延迟响应。

## 7. 如何给现有动作增加一个新片段

下面以给 `DISCOVER` 增加新片段为例。

### 第一步：增加关键帧表

在 `main/auto_run_data.cc` 中增加：

```cpp
const Keyframe kDiscoverSideFrames[] = {
    {140, {kKeep, kKeep, 140, kKeep, kKeep}, EASE_FAST_OUT},
    {260, {74, 118, 132, 42, 70}, EASE_SMOOTH},
    {480, {92, 62, 54, 54, 128}, EASE_SMOOTH},
    {620, {94, 96, 90, 58, 90}, EASE_SMOOTH},
};
```

### 第二步：增加声明

在 `main/auto_run_data.h` 中增加：

```cpp
extern const Keyframe kDiscoverSideFrames[];
```

### 第三步：注册到动作池

在 `kDiscoverClips[]` 中增加：

```cpp
CLIP("discover-side", kDiscoverSideFrames, false),
```

`DISCOVER` 原本只有 3 段，增加后正好是当前上限 4 段。若某个池已经有 4 段，要增加第 5 段，需要将 `auto_run.cc` 中的 `kMaxClipsPerAction` 调大，否则后面的片段不会被抽到。

## 8. 如何增加一种全新的动作类型

增加全新类型不只改数据表，需要同步检查以下位置：

1. 在 `main/auto_run.h` 的 `DinoAction` 中、`DINO_ACTION_COUNT` 之前增加枚举；
2. 在 `main/auto_run_data.cc` 增加关键帧表、`MotionClip` 数组和 `ClipSet`；
3. 在 `main/auto_run_data.h` 增加相应声明；
4. 严格按枚举顺序把新池放进 `kActionClips[]`；
5. 在 `auto_run.cc` 检查 `clip_attention_pose()`、力度、速度、收势、头颈尾风格等 `switch (action)` 是否需要专属参数；
6. 如果声音能够触发新动作，更新 `action_for_sound()` 和 `TriggerDinoActionWithAutoSound()` 中的文件名映射；
7. 如果网页需要按钮，更新 `main/web_assets/panel.html` 中的动作名称和枚举值；
8. 编译并检查所有 `DINO_ACTION_COUNT` 相关数组是否与新数量一致。

特别注意：网页接口发送的是枚举数值。如果在枚举中间插入新动作，后续动作的数字会整体变化；为了兼容已有网页或外部调用，通常更适合把新枚举追加在 `DINO_ACTION_COUNT` 前。

## 9. 音效与动作如何匹配

`action_for_sound()` 优先按 Flash 文件名关键词匹配：

| 文件名关键词 | 动作 |
|---|---|
| `咀嚼` / `eat` / `chew` | `EAT` |
| `脚步` / `step` / `foot` | `LISTEN` |
| `入睡` / `sleep` | `SLEEPY` |
| `警觉` / `startle` | `STARTLED` |
| `雀跃` / `happy` / `joy` | `HAPPY` |
| `亲近` / `comfort` / `nuzzle` | `AFFECTION` |

文件名没有命中时按时长兜底：

- 不超过 1.5 s：`DISCOVER`；
- 不超过 2.7 s：`HAPPY`；
- 更长或时长未知：`PROUD_CALL`。

`TriggerDinoActionWithAutoSound()` 只从 `animal` 分类中选择能映射回目标动作的音频。找不到合适声音时仍会提交动作，只是静音播放，不会强行配错声音。

## 10. 待机动作、环境场景与互动动作的关系

动作库中的 29 段 `MotionClip` 是明确的互动编舞。除此之外，`auto_run.cc` 还包含三套程序生成动作：

- `BehaviorState`：平静、左右好奇、活泼、自信和困倦等待机姿态；
- `LifePulse`：观察、尾巴爆发、嗅闻、精神一振等一次性生命动作；
- `NatureSceneState`：播放 `ambient` 环境音时的巡视、听雨、喝水、仰望和甩水场景。

它们不在 `auto_run_data.cc` 的关键帧动作库里。优先级可以理解为：

```text
互动 MotionClip / 环境音场景
             高于
       普通待机 + LifePulse
```

只要互动动作处于 `motion.active` 状态，当前输出就以它为主；互动结束后才重新平滑回到待机。

## 11. 并发、暂停与常见注意事项

- `TriggerDinoAction*()` 只是入队；若 `SetAutoRunRunning(false)`，`dino_auto` 暂停消费和输出，直到恢复。
- 对话开始时 `chat.cc` 会暂停自主动作并停止当前叫声，由 `chat_motion` 以 50 Hz 接管舵机；对话结束后按进入前状态恢复。
- 网页动作和舵机接口在对话期间返回 `{"ok":false,"reason":"chat_active"}`，避免两个任务同时写舵机。
- 新请求不会立即硬切舵机角度。任务以当前 `current_pose` 作为新动作起点，再经过 260 ms 预备姿态。
- `TriggerDinoAction(action, sound_index)` 只有在取出请求时发现当前未播放其他声音，才会启动指定声音；动作本身仍会执行。
- 动作表里的角度最终会被钳位，但“被钳位”只防止软件越界，不等于机械结构一定安全；安装偏差、舵盘零位和连杆干涉仍需实机验证。

## 12. 快速定位问题

| 现象 | 优先检查 |
|---|---|
| 调用返回 `false` | 是否已执行 `InitAutoRun()`、枚举是否合法、队列是否创建成功 |
| 调用成功但暂时不动 | `IsAutoRunRunning()` 是否为 `false`，是否正处于对话模式 |
| 新片段一直不出现 | 是否注册到正确的 `kXxxClips[]`；池是否超过 `kMaxClipsPerAction=4` |
| 动作方向相反 | 确认该次是否随机镜像；再检查实机舵机安装方向 |
| 尾巴上下方向写反 | 动作库使用内部 `tail_ud`，实际 IO8 角度是 `180-tail_ud` |
| 动作碰到结构件 | 先用 Web 单轴测安全范围，再收小关键帧角度；不要只依赖软件 0～180°钳位 |
| 长音频中动作停住 | 检查片段的 `continue_with_audio` 是否为 `true` |
| 声音没有匹配预期动作 | 检查音频 `category` 是否为 `animal`，以及文件名关键词和时长 |
| 每次动作看起来不完全一致 | 这是镜像、随机 tempo、风格叠加和一次性 accent 的设计效果 |
