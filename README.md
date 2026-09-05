# Dinosaur Pet Toy 恐龙宠物玩具

基于 ESP32-S3 (ESP-IDF v5.5.4) 的智能恐龙宠物玩具固件：5 路舵机有机动作引擎、
Flash Opus 音效、LLM 实时语音对话（ASR→LLM→TTS）、Web 控制面板、WiFi 配网。

**新手请先读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** —— 中文全景架构文档：
模块地图、动作库怎么用、音频库怎么用、对话链路、常见改动指南。

深入文档：
- 动作系统 V3.3 波形设计与实机舵机映射：[docs/elegant_motion_v2.md](docs/elegant_motion_v2.md)
- 全部 29 个互动动作 + 5 个环境音场景目录：[docs/dinosaur_action_catalog_v2_8.md](docs/dinosaur_action_catalog_v2_8.md)
- 音效与动作的配对规则：[docs/audio_and_action.md](docs/audio_and_action.md)

参考项目：[tailRedPanda](../tailRedPanda/)（小熊猫宠物玩具）和 [action_cat](../CAT/action_cat_success_demo/)（猫运动测试平台）。

## 硬件引脚总览

### 舵机 (5 × 180° 舵机，LEDC PWM 50Hz)

| GPIO | 舵机 | 角度含义 | 默认值 |
|------|------|----------|--------|
| **IO15** | 头左右转 (Head Turn) | 0°=最右，90°=朝前，180°=最左 | 90 |
| **IO16** | 脖子左右倾 (Neck Lean) | 0°=右倾，90°=中位，180°=左倾 | 90 |
| **IO17** | 脖子上下 (Neck UD) | 0°=最上/仰天，90°=中位，180°=最下/进食 | 70（默认抬高20°） |
| **IO18** | 尾巴左右 (Tail LR) | 0°=最左，90°=中位，180°=最右 | 90 |
| **IO8** | 尾巴上下 (Tail UD) | 55°=最下安全位，90°=中位，180°=最上 | 90 |
| **IO4** | 舵机电源控制 | HIGH=通电 | HIGH |

### 电源管理

| GPIO | 功能 | 说明 |
|------|------|------|
| **IO7** | 电源锁存 | HIGH=保持供电，LOW=断电关机 |
| **IO6** | 电源按钮 ADC | ADC1_CH5，<1.0V 判定为按下 |
| **IO3** | 电池电压 ADC | ADC1_CH2，分压比 2k:4.7k |

- **长按关机**: 按住按键 ≥1.5 秒后松手 → 播关机音 → IO7 拉低 → 系统断电
- **双击**: 开关 LLM 实时语音对话（`PowerSetButtonCallback(ChatToggle)` 接线）
- **消抖**: 对称迟滞消抖，50ms 窗口，20ms 轮询周期
- **电量监测**: 32× 过采样 + 一阶低通滤波，每 5 秒刷新

### 音频 (ES8311 Codec)

| 类型 | GPIO | 说明 |
|------|------|------|
| I2C SDA | **IO2** | ES8311 控制接口 |
| I2C SCL | **IO38** | ES8311 控制接口 |
| I2S WS | **IO13** | 字选信号 |
| I2S BCLK | **IO48** | 位时钟 |
| I2S DOUT | **IO46** | 数据输出 (ESP32 → Codec) |
| I2S DIN | **IO14** | 数据输入 (Codec → ESP32，麦克风录音) |
| I2S MCLK | NC | 未使用（从 BCLK 内部 PLL） |

参数: 48kHz 采样率, 16-bit, 单声道, 音量 80%

### 外接 SPI Flash

`config.h` 里 `EXTERNAL_FLASH_TYPE=2` → **W25N01GVZEIG（SPI NAND, 128MB）**，
存音频文件系统（TOC 目录 + 数据区）。

| GPIO | 说明 |
|------|------|
| **IO10** | CS (片选) |
| **IO9** | CLK (时钟) |
| **IO47** | MOSI (数据输出) |
| **IO21** | MISO (数据输入) |

SPI 模式 0, 40MHz 时钟, 手动 CS 控制。（W25Q256 NOR 64MB 驱动仍保留，宏切换）

### WiFi

| 参数 | 值 |
|------|-----|
| 模式 | STA，凭据存 NVS（配网页面输入，不写死在代码里） |
| 配网热点 | 连不上时自动开 `Dino-XXXX`（XXXX=MAC 后两字节，密码 12345678） |
| captive portal | DNS 劫持，手机连上热点自动弹配网页 |
| 连接超时 | 10 秒（超时后开热点或离线运行） |

## 项目结构

```
dinosaur/
├── CMakeLists.txt              # 项目级 CMake, project(dinosaur)
├── partitions.csv              # 分区表: nvs + phy_init + factory(12MB)
├── docs/
│   ├── ARCHITECTURE.md         # ⭐ 全景架构文档（先读这个）
│   ├── elegant_motion_v2.md    # 动作引擎波形设计
│   └── dinosaur_action_catalog_v2_8.md # 动作目录
└── main/
    ├── CMakeLists.txt          # 主组件: 源文件 + EMBED_FILES(网页)
    ├── idf_component.yml       # 依赖: esp_codec_dev, esp_audio_codec
    ├── config.h                # ⭐ 全部参数集中地（引脚/开关/服务器地址）
    ├── main.cc                 # app_main() 入口：按序拉起各模块
    ├── web_assets/             # panel.html / flash.html / audio.html（EMBED 链入）
    │
    ├── servo.h / servo.cc      # 5 通道舵机 LEDC PWM 驱动
    ├── power.h / power.cc      # 电源锁存、按键(长按/双击)、电池监测
    │
    ├── audio.h / audio.cc      # 播放(Flash→OGG→Opus→I2S) + 录音 + 提示音入口
    ├── audio_tone.h / .cc      # 共享的提示音合成 + 音量包络算法
    ├── ogg_demuxer.h / .cc     # OGG 容器解析状态机
    ├── flash_audio.h / .cc     # Flash 音频文件系统 (TOC v2 + 分类 + 互斥锁)
    ├── flash_upload_server.h/.cc # Flash 管理页/上传/擦除 Web API
    ├── sync_audio.h / .cc      # 从 Audio Hub 服务器同步音频（流式写 Flash）
    ├── w25n01gv.h / .c         # W25N01GV SPI NAND 驱动（递归锁）
    ├── w25q256.h / .cc         # W25Q256 SPI NOR 驱动（备用）
    │
    ├── auto_run.h              # 动作引擎公共 API（TriggerDinoAction 系列入口）
    ├── auto_run_data.h / .cc   # ⭐ 动作库本体（29 组关键帧 + 8 个编舞池）
    ├── auto_run.cc             # 动作引擎（有机波形/状态机/音效语义匹配）
    │
    ├── chat.h / chat.cc        # AI 对话总管（录音上行/播放下行/情绪驱动）
    ├── realtime_ws.h / .c      # WSS 连接生命周期（cookie+Origin 鉴权）
    ├── realtime_ws_internal.h  # 三个 realtime_ws 文件的内部共享声明
    ├── realtime_ws_protocol.c  # 收包解析（双协议 TTS 流式 base64）
    ├── realtime_ws_tts.c       # TTS 流控反馈 + 异步发送队列
    ├── ws_auth.h / .c          # 登录换会话 cookie
    ├── chat_cert.h             # TLS CA 证书
    │
    ├── wifi.h / wifi.cc        # WiFi STA、对时、省电模式
    ├── wifi_config.h / .cc     # 配网热点 + DNS 劫持 captive portal
    ├── http_server.h / .cc     # Web 面板 + 本地 API（舵机/动作/配网）
    ├── device_registry.h / .cc # Audio Hub 设备注册/激活/心跳
    └── display.h               # 日志垫片（仅打印）
```

## 启动流程

```
app_main()（详见 docs/ARCHITECTURE.md 第 2 章）
  ├─ nvs_flash_init()             # 初始化 NVS
  ├─ InitPower()                  # ① IO7 锁存供电 + 电源监控任务
  ├─ PowerSetButtonCallback(ChatToggle)  # ② 双击电源键 → 开关对话
  ├─ InitAudio() / InitServos()   # ③ 音频芯片 + 5 路 PWM
  ├─ PlayBootTone()               # ④ 开机提示音
  ├─ InitWiFi() + WaitForWiFi(10s) # ⑤ 连 WiFi；失败 → 开 Dino-XXXX 配网热点
  ├─ flash_audio_init()           # ⑥ 读 Flash TOC 音频目录
  ├─ StartHttpServer()            # ⑦ Web 面板立即可用
  ├─ (后台) 音频同步               # ⑧ AUDIO_SYNC_ENABLE=1 时从 Audio Hub 同步
  ├─ InitAutoRun() + TriggerDinoGreeting()  # ⑨ 动作引擎 + 开机问候
  └─ ChatInit()                   # ⑩ 对话模块待命（双击电源键才连服务器）
```

## 编译与烧录

**前置条件:** ESP-IDF v5.5.x，目标 esp32s3。

```bash
cd dinosaur
idf.py set-target esp32s3    # 首次
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 配置参数

全部集中在 [main/config.h](main/config.h)：功能开关（`OFFLINE_DEMO` /
`ENABLE_AUTO_RUN` / `AUDIO_SYNC_ENABLE`）、引脚、舵机默认角度、音频参数、
WiFi 超时、音频同步服务器（`SYNC_SERVER_IP/PORT`，部署时改）、
对话 WSS 入口（`CHAT_WS_URL`）与登录凭据。每个参数的含义见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 第 9 章的调参速查表。

## 音频文件部署

三种方式（详见 ARCHITECTURE.md 第 5 章）：

1. **网页上传** — 浏览器开 `http://<设备IP>/flash`，选 .opus 上传；
   文件名中的中文关键词（咀嚼/脚步/雀跃/亲近/警觉/入睡…）决定配对的动作。
2. **Audio Hub 自动同步** — `AUDIO_SYNC_ENABLE=1` 且设备激活后，
   从服务器增量同步（对比文件名+大小）。
3. **烧录 bin** — `build_opus_bin.py` 打包 TOC+数据，串口烧到 Flash。

**命名规范**：TOC 内不含 .opus 后缀；分类 animal（互动音）/ ambient（环境音）。

## Audio Hub 设备绑定

联网模式下，`main/device_registry.cc` 使用 ESP32 出厂 MAC 生成唯一设备 ID。
首次启动时向 Audio Hub 注册并在串口输出六位激活码；管理员在服务端后台绑定后，
设备把正式令牌保存到 NVS，之后每 60 秒上报一次心跳。未激活不阻塞对话，
只跳过音频同步。完整协议见服务端仓库的 `docs/DEVICE_ONBOARDING.md`。

## 技术亮点

1. **有机运动合成** — 相位扭曲 + 谐波叠加 + 中心漂移 + 随机 tempo，
   每段编舞每次演出略有差异，避免机械感
2. **动作库与引擎分离** — 改动作只动 `auto_run_data.cc`（纯数据表），
   引擎逻辑在 `auto_run.cc`，互不干扰
3. **流式音频** — OGG 解封装 + Opus 解码全程流式，4KB 分块读 Flash，不占大 RAM
4. **无 RAM 缓冲下载** — 网络音频流式直写 Flash，擦→写→TOC 更新一站式
5. **LLM 实时对话** — 16 槽 TTS 流状态 + playback 流控反馈 + 预缓冲，
   服务端按水位放量，长回复不断音；情绪标签实时驱动舵机
6. **配网零门槛** — 无凭据/连不上都自动开热点，DNS 劫持 captive portal 弹窗
7. **并发安全** — TOC 互斥锁、音频互斥、舵机写入仲裁（聊天期间网页请求拒绝）、
   WS 发送互斥，均见 ARCHITECTURE.md 第 8 章的契约表
