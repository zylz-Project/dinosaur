# Dinosaur Pet Toy 恐龙宠物玩具

基于 ESP32-S3 的智能恐龙宠物玩具固件，集成 5 路舵机运动控制、Opus 音频播放、外接 SPI Flash 存储、WiFi 联网下载及 Web 控制面板。

参考项目：[tailRedPanda](../tailRedPanda/)（小熊猫宠物玩具）和 [action_cat](../CAT/action_cat_success_demo/)（猫运动测试平台）。

## 硬件引脚总览

### 舵机 (5 × 180° 舵机，LEDC PWM 50Hz)

| GPIO | 舵机 | 角度含义 | 默认值 |
|------|------|----------|--------|
| **IO15** | 脖子前后倾 (Neck Tilt) | 0°=后仰，90°=中位，180°=前倾 | 90 |
| **IO16** | 脖子左右倾 (Neck Lean) | 0°=右倾，90°=中位，180°=左倾 | 90 |
| **IO17** | 头左右转 (Head Turn) | 0°=右转，90°=中位，180°=左转 | 90 |
| **IO18** | 尾巴上下 (Tail UD) | 0°=上翘，90°=中位，180°=下垂 | 90 |
| **IO8** | 尾巴左右 (Tail LR) | 0°=最右，180°=最左 | 90 |
| **IO4** | 舵机电源控制 | HIGH=通电 | HIGH |

### 电源管理

| GPIO | 功能 | 说明 |
|------|------|------|
| **IO7** | 电源锁存 | HIGH=保持供电，LOW=断电关机 |
| **IO6** | 电源按钮 ADC | ADC1_CH5，<1.0V 判定为按下 |
| **IO3** | 电池电压 ADC | ADC1_CH2，分压比 2k:4.7k |

- **长按关机**: 按住按键 ≥1.5 秒后松手 → IO7 拉低 → 系统断电
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
| I2S DIN | **IO14** | 数据输入 (Codec → ESP32) |
| I2S MCLK | NC | 未使用（从 BCLK 内部 PLL） |

参数: 48kHz 采样率, 16-bit, 单声道, 音量 80%

### 外接 SPI Flash (W25Q256JVEIQ, 32MB)

| GPIO | 说明 |
|------|------|
| **IO10** | CS (片选) |
| **IO9** | CLK (时钟) |
| **IO47** | MOSI (数据输出) |
| **IO21** | MISO (数据输入) |

SPI 模式 0, 40MHz 时钟, 手动 CS 控制。

### WiFi

| 参数 | 值 |
|------|-----|
| 模式 | STA (Station) |
| SSID | `YOUR_WIFI_SSID` |
| 密码 | `YOUR_WIFI_PASSWORD` |
| 连接超时 | 15 秒（超时后离线运行） |

## 项目结构

```
dinosaur/
├── CMakeLists.txt              # 项目级 CMake, project(dinosaur)
├── partitions.csv              # 分区表: nvs + phy_init + factory(12MB)
├── main/
│   ├── CMakeLists.txt          # 主组件编译: 13 个源文件
│   ├── idf_component.yml       # 依赖: esp_codec_dev, esp_audio_codec
│   ├── config.h                # 全局引脚定义、功能开关、参数配置
│   ├── main.cc                 # app_main() 入口函数
│   ├── servo.h / servo.cc      # 5 通道舵机 LEDC PWM 驱动
│   ├── power.h / power.cc      # 电源锁存、按键关机、电池 ADC 监测
│   ├── audio.h / audio.cc      # ES8311 I2S 音频 + Opus 流式解码播放
│   ├── auto_run.h / auto_run.cc # 恐龙自主行为引擎（核心）
│   ├── http_server.h / http_server.cc # HTTP 服务器 + Web 控制面板
│   ├── dino_samples.h / dino_samples.cc # 音效索引运行时查找
│   ├── wifi.h / wifi.cc        # WiFi STA 连接、IP 等待、省电模式
│   ├── w25q256.h / w25q256.cc  # W25Q256 SPI NOR Flash 完整驱动
│   ├── flash_audio.h / flash_audio.cc # Flash 文件系统 (TOC + 数据区)
│   ├── ogg_demuxer.h / ogg_demuxer.cc # OGG 容器解析状态机
│   ├── flash_upload_server.h / flash_upload_server.cc # Flash 管理 Web API
│   └── sync_audio.h / sync_audio.cc # 网络音频同步客户端
└── managed_components/         # ESP-IDF 托管组件
    ├── espressif__esp_codec_dev/    # ES8311 等 Codec 驱动
    └── espressif__esp_audio_codec/  # Opus/MP3/AAC 等解码器
```

## 启动流程

```
app_main()
  ├─ nvs_flash_init()            # 初始化 NVS 非易失存储
  ├─ InitPower()                 # ① IO7 拉高锁存供电, 启动电源监控任务
  ├─ InitAudio()                 # ② 初始化 I2C→I2S→ES8311, 启动音频播放任务
  ├─ InitServos()                # ③ IO4 拉高舵机电源, 配置 5 通道 LEDC PWM
  ├─ InitWiFi()                  # ④ 启动 WiFi STA 连接
  ├─ WaitForWiFi(15s)            # ⑤ 阻塞等待 WiFi 获取 IP (可超时离线)
  ├─ flash_audio_init()          # ⑥ 初始化 W25Q256 SPI Flash, 读取 TOC
  ├─ StartHttpServer()           # ⑦ 启动 HTTP 服务器 (含 Web UI + Flash API)
  ├─ [online] sync_audio_files()  # ⑧ WiFi 在线: 从服务器同步音频文件到 Flash
  └─ InitAutoRun()               # ⑨ 启动自主行为引擎 (FreeRTOS task)
```

## 核心模块详解

### 1. 舵机控制 (servo.cc)

**PWM 参数:**
- 定时器: `LEDC_TIMER_0`, 低速模式
- 频率: 50Hz, 13-bit 分辨率 (0–8191)
- 脉冲映射: `pulse_us = 500 + angle × 2000 / 180`
  - 0° → 500μs, 90° → 1500μs, 180° → 2500μs

**初始化顺序 (防抖设计):**
1. IO4 拉高 → 舵机通电
2. 所有舵机 GPIO 设为输出 LOW → 防止浮空导致乱跳
3. 配置 LEDC 定时器 + 5 通道
4. 逐一设置默认角度 (90°)

**API:**
```c
void InitServos();
void SetServoAngle(int idx, int angle);  // idx: 0-4, angle: 0-180
```

### 2. 电源管理 (power.cc)

**硬件原理:**
- IO7 驱动 PMOS 栅极 → HIGH 时系统供电 → LOW 时断电
- IO6 接分压电阻 + 按键 → 按键按下时电压拉低 → ADC 检测

**监控任务** (`dino_power`, 优先级 1, 4KB 栈):
- 20ms 轮询 IO6 ADC 值
- 对称迟滞消抖 (5 次确认 = 100ms)
- 记录按压持续时间
- 松手时判断是否达到长按阈值 (1500ms) → 关断 IO7
- 每 5 秒采集电池电压 (32 次过采样 + 低通滤波)
- 电量百分比: `(Vbat - 3200mV) / (4200mV - 3200mV) × 100`

### 3. WiFi 连接 (wifi.cc)

**工作模式:** STA (Station)

**事件处理:**
- `WIFI_EVENT_STA_START` → 发起连接
- `WIFI_EVENT_STA_DISCONNECTED` → 自动重连（reason 2 除外）
- `IP_EVENT_STA_GOT_IP` → 记录 IP，设置连接事件标志

**省电模式切换:**
- `WiFiPowerSave(false)` → 关闭 Modem 睡眠 (大文件下载时)
- `WiFiPowerSave(true)` → 开启最小 Modem 睡眠 (省电)

### 4. 音频系统 (audio.cc + ogg_demuxer.cc + flash_audio.cc)

**播放管道:**

```
SPI Flash → 4KB 分块读取 → OggDemuxer 解封装 → Opus 解码器 → I2S 输出 → ES8311 Codec → 扬声器
```

**OggDemuxer** — 自定义状态机 (零堆分配):
- `FIND_PAGE → PARSE_HEADER → PARSE_SEGMENTS → PARSE_DATA`
- 自动过滤 OpusHead / OpusTags 包头
- 支持跨页数据包 (continuation)
- 8KB 固定包缓冲区

**AudioPlayTask** (`dino_play`, 优先级 3, 32KB 栈, Core 1):
- 从 FreeRTOS 队列接收音效索引
- 按 TOC 索引查找 Flash 中的文件信息
- 流式读取 + 解封装 + 解码 + I2S 输出
- 每次最多播放一个文件 (非阻塞，队列深度 8)

**API:**
```c
void InitAudio();
bool PlayDinoSound(int type);   // 非阻塞，立即返回
bool IsAudioPlaying();           // 查询播放状态
void FlushAudioQueue();          // 清空播放队列
```

### 5. SPI Flash 文件系统 (flash_audio.cc + w25q256.cc)

**Flash 布局:**

| 地址 | 大小 | 内容 |
|------|------|------|
| `0x000000` | 4KB | TOC (Table of Contents) 索引表 |
| `0x001000`+ | ~32MB | Opus 音频数据 (每文件 4KB 对齐) |

**TOC 格式 (Sector 0):**
```
[0..3]   Magic: "PNDA" (0x41444E50)
[4..7]   Version: 1
[8..11]  File count: N
[12..]   File entries (each 80 bytes):
   [0..63]   Filename (UTF-8, null-padded, no extension)
   [64..67]  Offset (from data area start)
   [68..71]  Size in bytes
   [72..75]  Sample rate (e.g. 48000)
   [76..79]  Duration in milliseconds (estimated)
```

**流式写入** — 网络下载直接写入 Flash，不占用 RAM:
```c
flash_audio_stream_begin()   // 擦除目标扇区
flash_audio_stream_write()   // 分块写入
flash_audio_stream_end()     // 更新 TOC
```

### 6. 自主行为引擎 (auto_run.cc) — 核心

**设计理念:** 模拟恐龙的肌肉感和生命感，而非机械的周期运动。

**运行参数:**
- FreeRTOS 任务 `dino_auto`, 优先级 2, 4KB 栈
- 帧率 50FPS (20ms tick)
- 3 秒 smoothstep 渐变切换 (crossfade)

**波形生成:**

普通正弦波会让动作看起来像"匀速晃动的机器"。有机正弦 (`organic_sin`) 通过两个技术解决这个问题:

1. **相位扭曲** (`warp_phase`): smoothstep 扭曲让正弦波在极值处"停顿"更久、过零点快速穿过，模拟真实肌肉的"快收缩、慢放松"特征
2. **谐波叠加**: `sin(x) + 0.12·sin(2x+0.5) + 0.05·sin(3x+1.2)` 产生不对称波形

**中心漂移:** 各轴在 ±2–3° 范围内以 50–70 秒周期缓慢漂移，避免每次都回到同一个点。

**动作模式:**

#### 脖子模式 (NeckMode) — IO15 前后 + IO16 左右

| 模式 | 前后幅度 | 左右幅度 | 周期 | 描述 |
|------|---------|---------|------|------|
| BREATHE | 20 | 12 | 5.0s | 呼吸般温和振荡 |
| IDLE | 10 | 8 | 4.0s | 微小待机动作 |
| NOD | 35 | 8 | 3.0s | 大幅度前后点头 |
| SWAY | 22 | 25 | 3.5s | 画圆般的颈部摆动 (phase=0.25) |
| LOOK_LEFT | 12 | 25 | 5.0s | 向左看 (中心偏左 125) |
| LOOK_RIGHT | 12 | 25 | 5.0s | 向右看 (中心偏右 55) |
| ALERT | 10 | 6 | 5.5s | 警惕: 身体前倾 (中心 115) |
| SLEEP | 8 | 5 | 7.0s | 睡眠: 后仰放松 (中心 65) |
| PECK | 40 | 6 | 1.8s | 快速啄食 |
| CURIOUS | 18 | 20 | 4.0s | 好奇探索 |

#### 头部模式 (HeadMode) — IO17 左右转

| 模式 | 中心 | 幅度 | 周期 | 描述 |
|------|------|------|------|------|
| CENTER | 90 | 5 | 5.0s | 基本不动 |
| LOOK_LEFT | 135 | 8 | 5.0s | 持续向左看 |
| LOOK_RIGHT | 45 | 8 | 5.0s | 持续向右看 |
| SCAN | 90 | 55 | 4.0s | 全景扫描 |
| TILT_CURIOUS | 90 | 35 | 3.5s | 好奇歪头 |

#### 尾巴模式 (TailMode) — IO18 上下 + IO8 左右

| 模式 | 上下幅 | 左右幅 | 周期 | 描述 |
|------|-------|-------|------|------|
| RELAX | 20 | 25 | 4.0s | 温和摇摆 |
| WAG | 0 | 65 | 1.2s | 快速左右摇 |
| RAISE | 10 | 15 | 5.0s | 尾巴竖起 (中心 35) |
| DROOP | 10 | 15 | 5.0s | 尾巴下垂 (中心 145) |
| CIRCLE | 40 | 40 | 2.5s | 画圈 (phase=0.25) |
| HAPPY | 0 | 55 | 0.8s | 高兴快摇 |
| ALERT | 8 | 8 | 6.0s | 警惕竖起 |
| TWITCH | 12 | 40 | 1.5s | 快速抽动 |

#### 运行状态机

```
           ┌─────────────────────────────────┐
           │          IDLE 待机               │
           │  • 10-20s 动作轮换               │
           │  • 15-35s 自发微动作              │
           │  • 20-40s 随机音效                │
           └──────┬──────────┬───────────────┘
                  │          │
         音效触发 │          │ 120s 定时
                  ▼          ▼
    ┌─────────────┐   ┌─────────────────┐
    │  REACTION   │   │  RELAX 舒缓      │
    │  反应模式    │   │  • 自然白噪音     │
    │  • 音效联动  │   │  • 3-6s 动作轮换  │
    │  • 4-7s 保持 │   │  • 多频有机晃动   │
    │  • 幅度衰减  │   │  • 幅度 70-100%   │
    └──────┬──────┘   └────────┬────────┘
           │                   │
           ▼                   ▼
        回到 IDLE            回到 IDLE
```

**音效→动作联动:** 根据不同类型的恐龙叫声（呼叫/进食/幼崽等），随机选择对应的动作组合（警觉扫描/啄食/摇摆等），每次触发有概率差异保证多样性。

**硬摆模式** (`hard_swing`): 通过 Web API 或 `AUTO_RUN_DEFAULT_HARD` 宏启用。对波形施加 smoothstep 锐化 (`harden`) 并 4× 加速。

### 7. HTTP 服务器与 Web 控制面板

**API 端点:**

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/` | 恐龙控制面板 Web UI |
| POST | `/api/servo` | 设置舵机角度 `{"angles":[a0,a1,a2,a3,a4]}` |
| GET | `/api/battery` | 电池状态 `{"voltage_mv":X,"level":X}` |
| GET/POST | `/api/autoplay` | 查询/切换自主运行状态 |
| GET | `/flash` | Flash 文件管理页面 |
| GET | `/api/flash/status` | Flash 文件列表 JSON |
| POST | `/api/flash/upload` | 上传 .opus 文件 (multipart) |
| POST | `/api/flash/erase` | 擦除全部音频文件 |

**Web 控制面板** (内嵌 HTML):

每个舵机配有独立滑块 (0°–180°)，实时预览角度值，40ms 节流发送。包含快捷预设按钮（中位/后仰/前倾/右倾/左倾/右转头/左转头/尾巴上翘/尾巴下垂/尾巴最左）。电池状态每 5 秒自动刷新。

### 8. 音频同步 (sync_audio.cc)

从局域网服务器同步 Opus 音频文件到 SPI Flash:

```
① GET /api/files → 获取服务端文件清单 (JSON)
② 与本地 Flash TOC 对比 (文件名 + 大小)
③ 删除本地多余文件
④ 流式下载新增/变更文件 → 直接写入 Flash (不缓存于 RAM)
```

每个文件有 1 次重试机会 (2s 间隔)。服务器不可达时返回 `ESP_FAIL` 不修改 Flash。

## 配置参数

所有可调参数集中在 [main/config.h](main/config.h):

```c
// 功能开关
#define ENABLE_AUTO_RUN 1         // 编译自主运行功能
#define AUTO_RUN_DEFAULT_ON 1     // 上电自动启动自主运行
#define AUTO_RUN_DEFAULT_HARD 0   // 默认柔和模式

// 电源
#define POWER_LONG_PRESS_MS 1500  // 长按关机阈值
#define POWER_DEBOUNCE_MS 50      // 消抖窗口

// 舵机
#define SERVO_FREQ_HZ 50          // PWM 频率
#define SERVO_DUTY_RES LEDC_TIMER_13_BIT  // 13-bit 分辨率

// 音频
#define AUDIO_SAMPLE_RATE 48000   // Opus 源采样率
#define AUDIO_OUTPUT_VOLUME 80    // 音量 0-100%
#define AUDIO_SILENT_INTERVAL_MIN_S 20  // 音效最小间隔
#define AUDIO_SILENT_INTERVAL_MAX_S 40  // 音效最大间隔

// WiFi
#define WIFI_STA_SSID "YOUR_WIFI_SSID"
#define WIFI_STA_PASSWORD "YOUR_WIFI_PASSWORD"
#define WIFI_STA_TIMEOUT_S 15

// 音频同步服务器
#define SYNC_SERVER_IP "192.168.1.100"
#define SYNC_SERVER_PORT 5000
```

## 编译与烧录

**前置条件:** ESP-IDF v5.5+, ESP32-S3 目标芯片。

```bash
# 进入项目目录
cd dinosaur

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录 (USB 串口)
idf.py -p /dev/ttyUSB0 flash

# 查看日志
idf.py -p /dev/ttyUSB0 monitor
```

**音频文件部署:** 使用 tailRedPanda 项目中的 Python 脚本:

- `scripts/build_opus_bin.py` — 将 .opus 文件打包为 TOC + 数据 bin，可通过串口烧录到 SPI Flash
- `scripts/flash_opus.py` — 通过 WiFi HTTP 上传 .opus 文件到设备

或启动后连接 WiFi，设备自动从 `SYNC_SERVER` 同步音频文件。

## 技术亮点

1. **有机运动合成** — 相位扭曲 + 谐波叠加 + 中心漂移，避免了机械感，模拟真实生物的运动特征
2. **流式音频播放** — OGG 解封装 + Opus 解码全程流式处理，4KB 分块从 SPI Flash 读取，不占用大量 RAM
3. **无 RAM 缓冲下载** — 网络音频文件流式写入 SPI Flash，擦除→写入→TOC 更新一站式
4. **3 秒 smoothstep 渐变** — 动作间丝滑过渡，无突变
5. **多频段自然晃动** — 舒缓模式下叠加多个非整数比频率的有机晃动，模拟呼吸/微动
6. **防浮空 PWM 初始化** — 先拉低 GPIO 再配置 LEDC，避免上电瞬间舵机乱跳

## Audio Hub 设备绑定

联网模式下，`main/device_registry.cc` 会使用 ESP32 出厂 MAC 生成唯一设备 ID。首次启动时向 Audio Hub 注册，并在串口输出六位激活码；管理员在服务端后台绑定后，设备把正式令牌保存到 NVS，并每 60 秒上报一次心跳。

启用前将 `main/config.h` 的 `OFFLINE_DEMO` 改为 `0`，正确填写 Wi-Fi、`SYNC_SERVER_IP` 和 `SYNC_SERVER_PORT`。完整协议见服务端仓库的 `docs/DEVICE_ONBOARDING.md`。
