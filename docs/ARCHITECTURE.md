# 恐龙宠物玩具 · 全景架构文档

> **写给第一次接手这台玩具的人。** 先讲这台玩具是什么、怎么玩，再讲每个模块怎么实现；
> 读完你应该能回答三个问题：**怎么调一个动作？怎么加一段音效？对话数据是怎么流的？**
> 深入细节的动作目录见 [docs/dinosaur_action_catalog_v2_8.md](dinosaur_action_catalog_v2_8.md)，
> 动作引擎的波形设计见 [docs/elegant_motion_v2.md](elegant_motion_v2.md)。

---

## 1. 一分钟总览

这是一台会动、会叫、还会"聊天"的桌面恐龙玩具：

- **身体**：5 个舵机（脖子俯仰/脖颈前后/小头左右转/尾巴上下/尾巴左右），
  由"动作引擎"驱动，做出有生命感的动作而不是机械晃动。
- **嗓子**：一块外置 SPI NAND Flash 存着几十段 .opus 音效（叫声/咀嚼/环境声…），
  播放时经 OGG 解封装 + Opus 解码送到 ES8311 音频芯片。
- **耳朵和嘴（AI 对话）**：双击电源键进入实时语音对话——麦克风录音上行到
  LLM 服务器（ASR→LLM→TTS），TTS 音频流式下行播放，同时服务器还回情绪标签，
  恐龙会跟着情绪摆头摇尾。
- **遥控**：局域网里用手机/电脑浏览器打开设备 IP，就是一块控制面板
  （拖滑条控制舵机、触发动作、上传音效、配 WiFi）。

模块关系一张图（箭头 = 调用方向）：

```
                        ┌─────────────┐
                        │   main.cc    │  app_main()：按顺序把大家拉起来
                        └──────┬──────┘
        ┌──────────┬───────────┼────────────┬──────────────┐
        ▼          ▼           ▼            ▼              ▼
   power.cc    servo.cc     audio.cc    wifi.cc        auto_run.cc（动作引擎）
   电源/按键    5舵机PWM     播放+录音    WiFi/配网       ├─ auto_run_data.cc（动作库）
        │          ▲        │  │  ▲       │              └─ 状态机/波形/选音
        │          │        │  │  │       │
   双击电源键 ──────┼────────┼──┼──┼───────┼──→ chat.cc（AI 对话总管）
   （回调接线）     │        │  │  │       │        ├─ realtime_ws.c（WS 连接层）
        │          │        │  │  │       │        ├─ realtime_ws_protocol.c（收包解析）
        │          │        │  │  │       │        ├─ realtime_ws_tts.c（TTS 流控/发送）
        │          │        │  │  │       │        └─ ws_auth.c（登录换 cookie）
        │          │        ▼  ▼  │       ▼
        │          │   flash_audio.cc  wifi_config.cc（配网热点/DNS劫持）
        │          │   (Flash 音频文件系统)
        │          │        │
        │          │        ▼
        │          │   w25n01gv.c / w25q256.cc（外置 SPI Flash 驱动）
        │          │
        ▼          │
   http_server.cc ─┴── flash_upload_server.cc（网页：面板/Flash管理/音频验证）
                        （页面 HTML 在 main/web_assets/，EMBED 链入）
```

**一句话记忆**：`main.cc` 是点名册，`auto_run` 管身体，`audio+flash_audio` 管嗓子，
`chat+realtime_ws` 管耳朵和嘴，`http_server` 管遥控器，`power` 管生死。

---

## 2. 上电之后发生了什么

对照 `main/main.cc` 的 `app_main()`，按顺序：

| 步骤 | 代码 | 干什么 |
|------|------|--------|
| ① | `InitPower()` | IO7 拉高锁住供电（松手也不会断电），启动 `dino_power` 任务盯按键和电池 |
| ② | `PowerSetButtonCallback(ChatToggle)` | **接线**：以后电源键"双击"就调 `ChatToggle()` 开关对话 |
| ③ | `InitAudio()` / `InitServos()` | 音频芯片（I2C→I2S→ES8311）和 5 路 LEDC PWM 就位 |
| ④ | `PlayBootTone()` | 立刻"叮咚"一声开机提示音（不联网也要响） |
| ⑤ | `InitWiFi()` + `WaitForWiFi(10s)` | 连已保存的 WiFi；**连不上或没存过** → 开 `Dino-XXXX` 配网热点 |
| ⑥ | `flash_audio_init()` | 读外置 Flash 的 TOC（音频文件目录），常驻内存 |
| ⑦ | `StartHttpServer()` | 起 HTTP 服务，浏览器面板立即可用（同步下载期间也能访问） |
| ⑧ | （后台任务）音频同步 | 若开启 `AUDIO_SYNC_ENABLE` 且已激活设备：从 Audio Hub 服务器同步音效 |
| ⑨ | `InitAutoRun()` + `TriggerDinoGreeting()` | 动作引擎开跑，随机来一段开机问候（带叫声） |
| ⑩ | `ChatInit()` | 对话模块待命（还**没**连服务器，等双击电源键才连） |

长按电源键 1.5s 松手 → `power.cc` 播关机音 → IO7 拉低 → 断电。

---

## 3. 任务一览表

FreeRTOS 任务是这台玩具的"并发单位"。谁在什么时候跑，看这张表：

| 任务名 | 优先级 | 栈 | 核 | 职责 | 谁创建 |
|--------|-------|-----|-----|------|--------|
| `dino_power` | 1 | 4K | 任意 | 20ms 轮询电源按键（消抖/长按/双击）+ 每 5s 电池采样 | power.cc |
| `dino_auto` | 2 | 6K | 任意 | **动作引擎主循环**，20ms 一帧算姿态、写舵机 | auto_run.cc |
| `dino_play` | 3 | 32K | core1 | 从队列取音效索引 → Flash 读 → OGG 解封装 → Opus 解码 → I2S | audio.cc |
| `audio_sync` | 3 | 12K | 任意 | 一次性：联网后从服务器同步音频到 Flash，同步完自杀 | main.cc |
| `ws_proc` | 6 | 8K | 任意 | 消费 WS 收到的消息：拼 JSON、流式 base64 解码、分发 | realtime_ws.c |
| `ws_tx` | 5 | 8K | 任意 | 异步发送 TTS 反馈消息（playback_started/buffer/finished） | realtime_ws.c |
| `ws_connect` / `ws_reconnect` | 3 | 12K/4K | 任意 | 一次性：登录 + 建 WSS 连接 / 断线后重连 | realtime_ws.c |
| `chat_feed` | 5 | 8K | core0 | 对话开启后：麦克风采样 → 重采样 → 上行 PCM 帧 | chat.cc |
| `chat_resp` | 7 | 8K | core1 | 对话开启后：等 TTS → 播放 → 反馈流控 → 驱动情绪动作 | chat.cc |
| `chat_motion` | 4 | 3K | 任意 | 对话期间 50Hz 用情绪参数驱动舵机（压过 auto_run） | chat.cc |
| `httpd` | 默认 | — | 任意 | 处理浏览器请求（面板/动作/滑条/上传/配网） | http_server.cc |
| `wifi_portal` / `wifi_dns` | 5 | 4K/3K | 任意 | 配网热点兜底 / DNS 劫持（captive portal 弹窗） | wifi.cc / wifi_config.cc |
| `device_registry` | 4 | 6K | 任意 | Audio Hub 设备注册/心跳（60s 一次） | device_registry.cc |
| esp WS 客户端内部任务 | 4 | 6K | 任意 | esp_websocket_client 自带的收发任务 | IDF 组件 |

**铁律**：舵机的写入权是"聊天时归 `chat_motion`，平时归 `dino_auto`"，
网页滑条在聊天中会被拒（`reason:"chat_active"`）；关机时 `power` 直接拉 IO7，不仲裁。

---

## 4. 动作库怎么用（重点）

### 4.1 三行代码调一个动作

动作的一切入口都在 `auto_run.h`。所有动作都是**非阻塞**的——调用后立即返回，
由 `dino_auto` 任务在后台排队表演：

```cpp
#include "auto_run.h"

// ① 纯动作（不叫）
TriggerDinoAction(DINO_ACTION_EAT);

// ② 动作 + 自动配一段音效（从 Flash 里" animal"分类中按名称/时长匹配）
TriggerDinoActionWithAutoSound(DINO_ACTION_HAPPY);

// ③ 有了音效反过来选动作：随机抽一段"animal"音效，
//    按文件名关键词/时长决定做什么动作
TriggerDinoSoundAction(flash_audio_get_random_in_category("animal"));

// ④ 开机问候：随机挑一段 ≤5s 的"叫声/call/roar"音效 + 配套动作
TriggerDinoGreeting();
```

### 4.2 八个动作都是什么（白话版）

`DinoAction` 枚举（`auto_run.h`），每个动作在编舞池里有 3~4 段编舞，每次随机不重复抽：

| 枚举值 | 白话 | 编舞池内容 |
|--------|------|-----------|
| `DINO_ACTION_DISCOVER` | 好奇发现 | 正面发现 / 先缩后探 / 仰望追踪 |
| `DINO_ACTION_AFFECTION` | 亲近贴贴 | 鞠躬问候 / 大弧贴近 / 脸颊轻蹭 / 害羞贴近 |
| `DINO_ACTION_HAPPY` | 高兴 | 挺胸展示 / 大斜线摆动 / 空间画圆 / 追尾游戏 |
| `DINO_ACTION_PROUD_CALL` | 炫耀长鸣 | 仰天长鸣 / 高扫巡视 / 慢速巡视 / 双段长啸 |
| `DINO_ACTION_EAT` | 进食 | 低啄三口 / 先嗅后咬 / 咬完抬头 / 贴地横吃 |
| `DINO_ACTION_LISTEN` | 倾听定位 | 定住定位 / 循声跟踪 / 二次确认 / 高空声源 |
| `DINO_ACTION_STARTLED` | 受惊 | 后缩评估 / 对角弹跳 / 低伏躲避 |
| `DINO_ACTION_SLEEPY` | 犯困 | 慢呼吸趴下 / 两次点头打盹 / 绕身入睡 |

### 4.3 动作库的结构（改动作只看一个文件）

动作"库本体"在 `main/auto_run_data.cc`（引擎在 `auto_run.cc`，两者用
`auto_run_data.h` 连接）。数据是从"角度"到"动作"的四层漏斗：

```
DinoPose      一个时间点的五轴姿态角度
    │  {neck_tilt: 脖子俯仰, neck_lean: 脖颈前后, head_turn: 小头左右,
    │   tail_ud: 尾巴上下(0=上,125=下), tail_lr: 尾巴左右}
    ▼
Keyframe      一个时间点：{duration_ms 多久到达, target 目标姿态, ease 缓动}
    │  duration_ms 为 0 的 kKeep 表示"保持上一帧"——这是"动物感"的关键：
    │  头先动 → 脖子跟上 → 尾巴最后交代情绪，三段错开而不是同时转
    ▼
MotionClip    一段完整编舞（名字 + 关键帧序列 + 是否跟着音频循环）
    ▼
ClipSet       一个动作的编舞随机池（每次触发从池里不重复随机抽一段）
```

改动作的三个典型操作（都在 `auto_run_data.cc`）：

1. **微调一段编舞**：找到 `kXxxFrames[]` 表，改某个 Keyframe 的
   `duration_ms`（时长）或 `DinoPose`（角度）。
2. **加一段编舞**：照抄一段现有 `kXxxFrames[]` 改名，在文件底部的
   `CLIP(kXxxClips, ...)` 池里加一条 `CLIP(...)`，并把新表 extern 到
   `auto_run_data.h`。
3. **加一个全新动作**：在 `auto_run.h` 的 `DinoAction` 枚举里插入
   （`DINO_ACTION_COUNT` 之前的任意位置），再走第 2 步。总表
   `kActionClips[]` 按枚举下标索引，插在哪就填到哪。

缓动曲线 `Ease` 三选一：`EASE_SMOOTH` 平滑 S 曲线（默认）、
`EASE_FAST_OUT` 快出慢收（啄食/惊跳的"脆"）、`EASE_LINEAR` 匀速。

引擎侧还会给每帧加 ±18%~54% 的随机 tempo（118+hash%37），
所以同一段编舞每次演出来略有差异——这是"每只恐龙性格不同"的来源。

### 4.4 音效怎么配动作（文件名决定行为）

`auto_run.cc` 的 `action_for_sound()` 按文件名关键词匹配（中文/英文都认）：

| 文件名含 | 触发动作 |
|----------|---------|
| 咀嚼 / eat / chew | `EAT` 进食 |
| 脚步 / step / foot | `LISTEN` 倾听 |
| 入睡 / sleep | `SLEEPY` 犯困 |
| 警觉 / startle | `STARTLED` 受惊 |
| 雀跃 / happy / joy | `HAPPY` 高兴 |
| 亲近 / comfort / nuzzle | `AFFECTION` 亲近 |

名字都不匹配时按**时长**兜底：≤1.5s → `DISCOVER`（短叫好奇），
≤2.7s → `HAPPY`（中长叫），更长 → `PROUD_CALL`（长鸣炫耀）。

**所以给音效起名就是给它选动作**：想让它边吃边叫就传一段叫"咀嚼-脆响.opus"的音效。

### 4.5 谁在什么时候自动调用动作

| 场景 | 谁触发 | 做什么 |
|------|--------|--------|
| 开机 | `main.cc` → `TriggerDinoGreeting()` | 随机问候 |
| 待机空闲 | `dino_auto` 状态机每 4~9s | 随机播一段"animal"音效 + 语义动作 |
| 空闲 5 分钟 | `dino_auto` 的 `next_relax` | 播一段"ambient"环境音 + 自然场景动作 |
| 对话中 | `chat_motion`（情绪驱动） | auto_run 暂停让位，情绪参数驱动五轴 |
| 手机面板 | `POST /api/action {"action":N}` | 手动触发枚举 N 的动作（聊天中返回 409 语义的 `chat_active`） |
| 网页滑条 | `POST /api/servo {"angles":[...]}` | 直接写 5 轴（聊天中拒绝） |

---

## 5. 音频库怎么用（重点）

### 5.1 Flash 上的音频文件系统

外置 Flash（W25N01GV NAND 128MB）第一块擦除单元放"目录"（TOC），后面放数据：

```
擦除单元 0:  TOC（只占 4KB）
  [0..3]   魔数 "PNDA"
  [4..7]   版本 = 2
  [8..11]  文件数 N
  [12...]  N × 96 字节条目：
           [0..63]  文件名（UTF-8，无后缀）
           [64..67] 数据区偏移  [68..71] 大小
           [72..75] 采样率      [76..79] 时长 ms
           [80..95] 分类（"animal" 动物音 / "ambient" 环境音）
其余:  音频数据区（每文件 4KB 对齐）
```

TOC 常驻内存（`g_files[]`），读写走 `flash_audio.h` 的 API；
**TOC 有互斥锁**，任何任务在任何时刻增删查都是安全的（2.x 修复后）。

### 5.2 加音频的三种方式

| 方式 | 操作 | 适用 |
|------|------|------|
| 网页上传 | 浏览器开 `http://<设备IP>/flash` → 选 .opus 上传（分类固定为 animal） | 调试/临时加音效 |
| Audio Hub 同步 | 开 `AUDIO_SYNC_ENABLE=1`，设备激活后自动从服务器全量同步（对比文件名+大小，增量下载） | 量产部署 |
| 烧录 bin | 用 `build_opus_bin.py` 打包成 TOC+数据 bin，烧到 Flash | 工厂预置 |

**注意**：播放中（`IsAudioPlaying()`）上传/擦除会被拒绝（HTTP 409），
同步任务也会先停播再动 Flash——TOC 和数据区不能边播边改。

### 5.3 命名规范（名字 = 行为）

文件名不含 `.opus` 后缀进 TOC。**中文名关键词直接决定动作配对**（见 4.4 的表）；
"叫声/call/roar" 开头且 ≤5s 的文件会被开机问候优先选中。
环境音分类 `ambient` 只用于 5 分钟一次的自然场景，不参与随机互动。

### 5.4 播放管线一图流

```
flash_audio (4KB 分块读) → ogg_demuxer (找 OggOpus 包) → esp_audio_codec (Opus→PCM)
   → I2S → ES8311 → 功放 → 扬声器
         │
         └─ 每块顺手算平均幅度 → 双包络(audio_tone.h) → onset 起音
            → dino_auto 把起音强度加到动作上（"叫得响头扬得高"）
```

`dino_play` 任务通过 FreeRTOS 队列接收播放请求（深度 8），一次只播一段；
`AudioStopCurrent()` 打断当前，`FlushAudioQueue()` 清空排队的。
提示音（开机叮/关机咚/对话提示音）不经过这条队列，由 `audio_tone.cc`
的合成器直接写 codec（拿同一个 `audio_mutex_` 串行，保证不插花 TTS）。

---

## 6. LLM 实时对话链路

双击电源键 → `ChatToggle()` → `ChatStart()`：

```
① 连接建立
   ws_connect_task: 等系统时间同步(注入的 WiFiWaitForTimeSync)
     → ws_auth.c 登录拿 cookie (最多试3次)
     → WSS 握手 (cookie + Origin + CA 证书校验)
     → 发 hello (声明上行 PCM 格式)

② 上行 (chat_feed 任务, core0)
   麦克风 48k 采样 → 1/3 抽取到 16k → 40ms 帧 (640样本)
   → WS 二进制帧发送。听到 ASR 结果后服务端开始思考。

③ 服务端处理
   ASR → LLM → 情绪识别 (随 user message 返回 emotion_label 等)
   → TTS 合成，按 seq 分多个流下发

④ 下行 (ws_proc 任务收, chat_resp 任务播)
   新协议: tts_audio_start(声明格式/流ID) → tts_audio_chunk(base64 PCM ×N)
   → tts_audio_end        旧协议: 一整个巨大 tts_audio JSON(WAV base64)
   流式解码边收边进 g_tts_queue(1024 槽, PSRAM)

⑤ 流控反馈 (realtime_ws_tts.c, ws_tx 任务异步发)
   每个流(seq)独立 16 槽状态: playback_started → tts_playback_buffer(每200ms水位)
   → playback_finished。服务端靠这个控制 TTS 下发窗口；
   反馈丢了服务端会以为缓冲没释放，停止下发并断连——所以 playback_finished
   会重试 3 次且队列满时宁可阻塞也不丢。

⑥ 播放 + 情绪
   chat_resp 预缓冲 1.2s (TTS_PREBUFFER_MS，实测 0.74x 下发速度会欠载)
   → 播放，边播边算音量包络 → chat_motion 任务把情绪参数
   (speed/head_bias/lr_bias/ud_bias/shake) 混成 50Hz 舵机角度
   → 恐龙边说边摇头摆尾。
```

对话结束（再双击）→ `ChatStop()`：打断播放、清流状态、断 WSS、
交还舵机给 auto_run。**对话期间** `ChatIsActive()` 为 true，
网页的动作/滑条请求会被拒绝，`dino_auto` 暂停。

三个 realtime_ws 文件的分工（Phase 4.2 拆分后）：

| 文件 | 行数 | 只管 |
|------|------|------|
| `realtime_ws.c` | ~460 | 连接生命周期：init/connect/disconnect/事件回调/会话缓冲清理 |
| `realtime_ws_protocol.c` | ~600 | 收包：JSON 拼接/解析、双协议 TTS 流式 base64 解码、情绪字段 |
| `realtime_ws_tts.c` | ~560 | 发送：16 槽流状态、playback 反馈、异步 TX 队列、hello/audio |

三者通过 `realtime_ws_internal.h` 共享内部状态（仅限这三个文件 include）；
对外永远只暴露 `realtime_ws.h`。

---

## 7. WiFi 与配网

**正常联网**：有 NVS 凭据 → STA 连接 → 10s 内拿到 IP → 通知 SNTP 对时
（TLS 证书校验需要正确时间）。

**配网流程**（没存过凭据，或凭据 10s 连不上）：

```
开 SoftAP "Dino-XXXX" (XXXX=MAC 后两字节, 密码 12345678)
  + 起 DNS 劫持任务: 任何域名都解析到 192.168.4.1
  → 手机连上热点后自动弹 captive portal（或手动访问 192.168.4.1）
  → 配网页列出周边 WiFi 扫描结果 (/api/wifi/scan)
  → 用户选网络输密码 → /api/wifi/configure
  → NVS 保存凭据 → 关热点 → STA 重连 → 连上
```

配网期间 HTTP 服务照常跑（同一个 httpd 实例，AP/STA 共存），
所以面板仍然可用，页面右上角有"WiFi 设置"入口主动进配网。

**省电策略**：平时 modem sleep 开着；大文件下载（音频同步）期间关闭
（DTIM 批量收包会让吞吐减半），下载完恢复——开关都收敛在 `sync_audio.cc`
和 `chat.cc` 各自的"使用期"里，main.cc 不再操心。

---

## 8. 并发规则（修复后的契约）

**谁持有什么锁**：

| 锁 | 保护什么 | 规则 |
|----|---------|------|
| `audio_mutex_` (audio.cc) | codec 写入口 | Flash 播放 / 对话 TTS / 提示音全走它串行 |
| TOC 互斥锁 (flash_audio.cc) | 文件目录 `g_files[]` | 增删查全取锁；长 SPI 读在锁外（快照语义） |
| `g_tts_flow_mux` (realtime_ws) | 16 槽 TTS 流状态 | portMUX 临界区，持锁内只做短操作 |
| `g_conn_mutex` / `g_ws_tx_mutex` | WS client 指针 / 发送串行 | disconnect 与 send 互斥，防 use-after-free |
| `g_resp_mutex` | 响应文本/情绪缓存 | 短临界区 |
| w25n01gv / w25q256 内部递归锁 | SPI 总线事务 | 驱动层自持，上层无感 |

**舵机写入者仲裁表**：

| 场景 | 谁可以写五轴 | 谁被拒 |
|------|-------------|--------|
| 对话中 | `chat_motion`（情绪驱动） | 网页滑条/动作按钮（返回 `chat_active`），auto_run 暂停 |
| 平时 | `dino_auto` + 网页 | — |
| 关机 | `power` 直接拉 IO7 断电 | 不仲裁，最高优先级 |

**单写多读契约**（volatile，不加锁）：chat 的 `g_state` 只有 `chat_resp`
写、`feed/motion` 读；`g_emo_target` 只有 `chat_resp` 写、`chat_motion`
通过 `chat_get_emo_snapshot()` 读快照。50Hz 实时路径，锁开销大于收益，
最坏读到一帧旧情绪参数，无害。

---

## 9. 调参速查（config.h）

| 参数 | 默认 | 含义 / 改大会怎样 |
|------|------|------------------|
| `OFFLINE_DEMO` | 0 | 1 = 跳过 WiFi 秒开机（演示用） |
| `ENABLE_AUTO_RUN` | 1 | 0 = 整个动作系统不编译 |
| `AUTO_RUN_DEFAULT_ON` | 1 | 0 = 上电动作暂停（网页可开） |
| `AUDIO_SYNC_ENABLE` | 0 | 1 = 开机后台同步音频（默认关，对话不受影响） |
| `POWER_LONG_PRESS_MS` | 1500 | 长按关机阈值 ms |
| `SERVO_*_DEFAULT` | 70/90/90/90/90 | 上电初始五轴角度；IO17=70 是"昂首"姿态 |
| `AUDIO_SAMPLE_RATE` | 48000 | Opus 源采样率（一般不动） |
| `AUDIO_OUTPUT_VOLUME` | 80 | 0-100 |
| `AUDIO_SILENT_INTERVAL_MIN/MAX_S` | 4/6 | 待机随机音效的间隔秒数 |
| `WIFI_STA_TIMEOUT_S` | 10 | 连不上 WiFi 多久后转配网 |
| `SYNC_SERVER_IP/PORT` | 192.168.1.7:5000 | Audio Hub 服务器地址（**部署时改**） |
| `CHAT_WS_URL` | mmemoryy.xyz…141 | LLM 实时对话 WSS 入口（**换服务器/会话时改**） |
| `AUTH_USERNAME/PASSWORD` | admin/admin123 | 对话服务登录凭据（⚠️明文，量产前改 NVS 方案） |

chat.cc 里的对话调参（改前先看注释里的实测数据）：
`TTS_PREBUFFER_MS 1200`（预缓冲，小了会"TTS gap"断音，大了开始慢）、
`TTS_STEADY_BUFFER_MS 3000`（稳态水位，服务端按它放量）。

---

## 10. 常见改动指南

**加一个动作** → 只动 `auto_run_data.cc` + `auto_run_data.h`：
1. 在 `auto_run_data.cc` 照抄一段 `kXxxFrames[]` 起名 `kMyFrames[]`；
2. 文件底部找到所属动作的 `CLIP(...)` 池，加一条 `CLIP(kMyFrames, ...)`；
3. 若是全新动作：先在 `auto_run.h` 的 `DinoAction` 枚举插入，
   再在 `auto_run_data.cc` 的 `kActionClips[]` 里加对应 `CLIP_SET`，
   并在 `auto_run_data.h` 补 extern 声明。

**加一段音效** →
1. 准备 .opus（48kHz）；文件名里放中文关键词（咀嚼/脚步/雀跃…）决定配的动作；
2. 设备联网后开 `http://<IP>/flash` 上传（或走 Audio Hub 同步）；
3. 等下一次待机随机触发，或网页手动点动作验证。

**换 LLM 服务器** → 改 `config.h` 的 `CHAT_WS_URL`（session_id 在 URL 里）+
`AUTH_API_URL`/凭据 + `chat_cert.h` 里的 CA 证书。

**换 WiFi** → 手机连 `Dino-XXXX` 热点（或面板右上角"WiFi 设置"）重新配网；
凭据存 NVS，不用改代码。

**加一个 Web 按钮** →
1. `main/web_assets/panel.html` 加按钮和 fetch 调用（改网页不用动 C）；
2. `http_server.cc` 加 handler + 路由（注意 max_uri_handlers=20 上限，当前 11+5）；
3. handler 里调用对应模块的公共 API（`chat.h`/`auto_run.h`/`flash_audio.h`…）。

**改配网页面** → `main/web_assets/` 下三个 html 分别是：面板、Flash 管理、
音频验证。IDF 的 EMBED_FILES 会把它们链进固件，改完重编即可。
