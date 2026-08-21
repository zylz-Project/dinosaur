# 恐龙宠物玩具 — 音频与动作控制详解

## 目录

- [一、音频系统](#一音频系统)
  - [1.1 硬件架构](#11-硬件架构)
  - [1.2 音频文件存储 (SPI Flash)](#12-音频文件存储-spi-flash)
  - [1.3 播放管道](#13-播放管道)
  - [1.4 OggDemuxer — OGG 解封装状态机](#14-oggdemuxer--ogg-解封装状态机)
  - [1.5 Opus 解码与 I2S 输出](#15-opus-解码与-i2s-输出)
  - [1.6 队列与非阻塞调度](#16-队列与非阻塞调度)
  - [1.7 音频文件部署](#17-音频文件部署)
- [二、动作控制系统](#二动作控制系统)
  - [2.1 舵机 PWM 底层](#21-舵机-pwm-底层)
  - [2.2 波形生成算法](#22-波形生成算法)
  - [2.3 动作模型体系](#23-动作模型体系)
  - [2.4 运行状态机](#24-运行状态机)
  - [2.5 交叉渐变 (Crossfade)](#25-交叉渐变-crossfade)
  - [2.6 音效→动作联动](#26-音效动作联动)
  - [2.7 Web 控制面板](#27-web-控制面板)

---

## 一、音频系统

### 1.1 硬件架构

```
ESP32-S3                    ES8311 Codec              扬声器
┌──────────┐   I2C(SDA/SCL)  ┌───────────┐
│  IO2 ──────► SDA           │           │
│  IO38 ─────► SCL           │  DAC +    │  ────►  🔊
│          │                 │  PA       │
│  IO13 ──────► WS (字选)    │           │
│  IO48 ──────► BCLK (位时钟)│           │
│  IO46 ──────► DOUT (数据)  └───────────┘
│          │
│  IO10 ──► CS   ┌─────────────┐
│  IO9  ──► CLK  │  W25Q256    │
│  IO47 ──► MOSI │  32MB NOR   │
│  IO21 ──► MISO │  Flash      │
└──────────┘     └─────────────┘
```

**关键参数:**

| 参数 | 值 | 说明 |
|------|-----|------|
| I2C 总线 | I2C_NUM_0, 标准模式 | ES8311 控制接口 |
| I2S 模式 | 主机, 标准 (Philips) | 时钟由 ESP32 产生 |
| 采样率 | 48000 Hz | 与 Opus 源文件一致 |
| 位深 | 16-bit | I2S_DATA_BIT_WIDTH_16BIT |
| 声道 | 单声道 | 代码中为 `ESP_AUDIO_MONO` |
| I2S 时隙 | 立体声 (复制到两声道) | `I2S_SLOT_MODE_STEREO`, `I2S_STD_SLOT_BOTH` |
| MCLK | 无 (NC) | ES8311 从 BCLK 内部 PLL 产生 |
| DMA | 6 描述符 × 240 帧 | `auto_clear_after_cb = true` |
| 音量 | 80% | 可通过 `AUDIO_OUTPUT_VOLUME` 调节 |

**ES8311 配置细节:**

```c
es8311_codec_cfg_t es = {};
es.codec_mode   = ESP_CODEC_DEV_WORK_MODE_DAC;  // 仅 DAC(播放), 不录音
es.pa_pin       = GPIO_NUM_NC;                    // 无独立 PA 控制引脚
es.use_mclk     = false;                          // 无外部 MCLK, 用 BCLK PLL
es.hw_gain.pa_voltage         = 5.0f;             // 功放供电 5V
es.hw_gain.codec_dac_voltage  = 3.3f;             // DAC 参考电压 3.3V
es.no_dac_ref   = true;                           // 不使用外部 DAC 参考
```

### 1.2 音频文件存储 (SPI Flash)

使用 W25Q256JVEIQ (32MB / 256Mbit) SPI NOR Flash 存储 Opus 编码的音频文件。

#### Flash 布局

```
┌────────────────────────────────────────────┐
│ Sector 0 (0x000000, 4KB): TOC 索引表        │
│ ┌──────────────────────────────────────┐   │
│ │ Magic: "PNDA" (0x41444E50)   4 bytes │   │
│ │ Version: 1                   4 bytes │   │
│ │ File count: N                4 bytes │   │
│ │ Entry[0]: name[64] + offset + size  │   │
│ │         + sample_rate + duration_ms │   │
│ │         (每个文件条目 80 bytes)       │   │
│ │ Entry[1]: ...                       │   │
│ │ ...                                  │   │
│ │ Entry[31]: ... (最大 32 个文件)       │   │
│ └──────────────────────────────────────┘   │
├────────────────────────────────────────────┤
│ Sector 1+ (0x001000+): 音频数据区            │
│ ┌──────────────────────────────────────┐   │
│ │ File 0: Opus 数据 (4KB 边界对齐)      │   │
│ ├──────────────────────────────────────┤   │
│ │ File 1: Opus 数据                     │   │
│ ├──────────────────────────────────────┤   │
│ │ File N: Opus 数据                     │   │
│ └──────────────────────────────────────┘   │
└────────────────────────────────────────────┘
```

#### TOC 条目格式 (每条 80 bytes)

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 64 | filename | UTF-8 文件名，不含扩展名，null 填充 |
| 64 | 4 | offset | 在数据区中的偏移 (从 0x001000 起算) |
| 68 | 4 | size | 文件大小 (字节) |
| 72 | 4 | sample_rate | 采样率 (如 48000) |
| 76 | 4 | duration_ms | 估算时长 (毫秒)，基于 48kbps 码率估算 |

#### TOC 读写 API

```c
// 初始化 — 从 Flash 读取 TOC, 或创建空的
esp_err_t flash_audio_init(void);

// 获取文件数量
int flash_audio_get_file_count(void);

// 获取第 idx 个文件的元信息
esp_err_t flash_audio_get_file_info(int idx, flash_audio_info_t *info);

// 按文件名查找索引
int flash_audio_find_file(const char *filename);

// 分块读取文件数据 — 播放时按 4KB 块流式读取
esp_err_t flash_audio_read_file(int idx, uint32_t offset,
                                uint8_t *buf, size_t len);

// 写入整个文件 — 用于 Web 上传
esp_err_t flash_audio_write_file(const char *filename,
                                  const uint8_t *data, size_t len,
                                  uint32_t sample_rate);

// 流式写入 — 用于网络下载, 不占用 RAM 缓冲
esp_err_t flash_audio_stream_begin(flash_audio_stream_t *s,
                                    const char *filename,
                                    uint32_t total_size, uint32_t sample_rate);
esp_err_t flash_audio_stream_write(flash_audio_stream_t *s,
                                    const uint8_t *data, size_t len);
esp_err_t flash_audio_stream_end(flash_audio_stream_t *s);

// 删除/擦除
esp_err_t flash_audio_delete_file(const char *filename);
esp_err_t flash_audio_erase_all(void);
```

### 1.3 播放管道

完整的音频播放数据流:

```
SPI Flash (W25Q256)        OggDemuxer           Opus Decoder         I2S + ES8311
┌──────────┐   4KB块   ┌────────────┐  Opus包  ┌──────────┐  PCM    ┌──────────┐
│ Opus 文件 │ ──────► │ 解封装      │ ──────► │ Opus解码  │ ─────► │ I2S 输出  │ ──► 🔊
│ (OGG容器) │         │ 状态机      │ 回调    │ 60ms帧   │ 240采样│ DMA→Codec │
└──────────┘         └────────────┘         └──────────┘ /chunk └──────────┘
      ↑                    ↑                      ↑                   ↑
 flash_audio         OggDemuxer           esp_opus_dec         esp_codec_dev
 _read_file()        ::Process()          _decode()            _write()
     4KB chunk                            延迟初始化
```

**流式播放流程 (audio.cc: AudioPlayTask):**

```
1. xQueueReceive(sound_queue_, &type)           // 阻塞等待播放请求
2. flash_audio_get_file_info(idx, &info)        // 从 TOC 获取文件元信息
3. 分配 PCM 输出缓冲区 (5760 bytes = 60ms@48kHz mono)
4. 创建 OggDemuxer, 注册回调 on_demuxer_finished
5. while (未读完文件 && 无解码错误):
   │
   ├─ flash_audio_read_file(idx, offset, 4KB)
   ├─ OggDemuxer::Process(4KB_chunk)             // 喂入 OGG 数据
   │   └─ 回调: 收到完整 Opus 音频包时触发
   │       ├─ [首次] esp_opus_dec_open()          // 延迟初始化解码器
   │       ├─ esp_opus_dec_decode()               // Opus → PCM
   │       └─ esp_codec_dev_write() × N           // PCM → I2S 分块输出
   │           (每块 240 采样 ≈ 5ms, 块间延迟 3ms)
   └─ offset += 4KB
6. esp_opus_dec_close(dec)                       // 关闭解码器
7. 释放 PCM 缓冲区
```

### 1.4 OggDemuxer — OGG 解封装状态机

`ogg_demuxer.cc` 实现了一个**零堆分配**的 OGG 容器解析器，专门为嵌入式环境优化。

#### 状态机

```
                    ┌──────────────────────────────┐
                    │                              │
                    ▼                              │
              ┌──────────┐                         │
      ┌─────►│ FIND_PAGE │  在字节流中搜索 "OggS"    │
      │      └─────┬────┘                         │
      │            │ 找到                          │
      │            ▼                              │
      │      ┌─────────────┐                      │
      │      │ PARSE_HEADER│  读取 27 字节页头       │
      │      └──────┬──────┘                      │
      │             │ 解析完成                     │
      │             ▼                             │
      │      ┌───────────────┐                    │
      │      │ PARSE_SEGMENTS│  读取段表 (≤255字节)  │
      │      └───────┬───────┘                    │
      │              │ 解析完成                    │
      │              ▼                            │
      │      ┌─────────────┐                      │
      │      │  PARSE_DATA │  按段表提取数据包       │
      │      └──────┬──────┘                      │
      │             │                             │
      │             ├─ 段值 < 255: 包结束 → 回调输出  │
      │             │   • OpusHead → 提取采样率      │
      │             │   • OpusTags → 跳过           │
      │             │   • 音频数据 → 输出给解码器      │
      │             │                             │
      │             ├─ 段值 = 255: 包继续 (跨段)     │
      │             │                             │
      │             └─ 所有段处理完 → 回到 FIND_PAGE ─┘
      │
      └─── 段值 = 0: 空页, 直接回到 FIND_PAGE
```

#### 核心数据结构

```cpp
struct context_t {
    bool     packet_continued;   // 当前包是否跨多个段 (段值=255)
    uint8_t  header[27];         // OGG 页头缓冲区
    uint8_t  seg_table[255];     // 段表 (每段 1 字节, 最大 255 段)
    uint8_t  packet_buf[8192];   // 包数据累积缓冲区 (8KB)
    size_t   packet_len;         // 当前包已累积的长度
    size_t   seg_count;          // 当前页段数
    size_t   seg_index;          // 当前处理的段索引
    size_t   bytes_needed;       // 当前状态还需要多少字节
    size_t   seg_remaining;      // 当前段剩余未读字节数
    size_t   body_size;          // 当前页数据体总大小
    size_t   body_offset;        // 已读取的数据体字节数
};
```

**关键约束:** 全部使用栈上的固定大小缓冲区，`malloc`/`free` 调用次数为**零**。

#### 跨页包处理

OGG 规范允许一个逻辑包被分割到多个物理页中。段值 = 255 (`0xFF`) 表示"包继续，前面页面还有更多数据":

```
Page N:       [seg=255] [seg=255] [seg=200]
                ~~~~~~~   ~~~~~~~   ~~~~~~~
                继续      继续      包结束

Page N+1:     [seg=180] [seg=255] [seg=50]
                ~~~~~~~   ~~~~~~~   ~~~~~
                继续      继续      包结束
```
`packet_continued = true` 时，`packet_buf` 和 `packet_len` 在页面切换时**不清零**，下一页面继续追加。

### 1.5 Opus 解码与 I2S 输出

#### 解码器配置

```c
esp_opus_dec_cfg_t cfg = {};
cfg.sample_rate    = 48000;                              // 48kHz
cfg.channel        = ESP_AUDIO_MONO;                     // 单声道
cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;  // 60ms 帧
cfg.self_delimited = false;                              // 外部组帧
```

**延迟初始化:** 解码器在收到第一个 Opus 音频包时才创建 (`esp_opus_dec_open`)。OggDemuxer 过滤掉 OpusHead 和 OpusTags 包头，回调只收到纯音频数据包。

#### I2S 输出策略

PCM 数据通过 `esp_codec_dev_write()` 以 **240 采样/块 (5ms)** 的粒度输出到 I2S，每块之间 `vTaskDelay(3ms)`:

```c
#define I2S_CHUNK_SAMPLES 240  // 5ms @ 48kHz

for (size_t off = 0; off < total_samples; off += I2S_CHUNK_SAMPLES) {
    size_t n = min(total_samples - off, I2S_CHUNK_SAMPLES);
    esp_codec_dev_write(dev_, pcm + off, n * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(3));  // 让出 CPU 给其他任务
}
```

**为什么 5ms 分块 + 3ms 延迟?**

- **DMA 缓冲区保护:** I2S DMA 以帧为单位消费数据，一次性写入太大的数据块可能在 DMA 完成前被覆盖
- **CPU 让出:** `vTaskDelay(3ms)` 让 FreeRTOS 调度器将 CPU 分配给同优先级的其他任务（如舵机控制）
- **播放流畅性:** 5ms 的分块足够小，人耳听不到间断；3ms 的间隙被 ES8311 的内部 FIFO 缓冲吸收

### 1.6 队列与非阻塞调度

#### AudioPlayTask 任务参数

| 参数 | 值 |
|------|-----|
| 任务名 | `dino_play` |
| 优先级 | 3 (实时) |
| 栈大小 | 32768 bytes (32KB) |
| 核心 | Core 1 (独立于主循环) |
| 队列深度 | 8 (最多排队 8 个播放请求) |

#### API

```c
// 初始化音频硬件和播放任务
void InitAudio();

// 非阻塞播放 — 将请求入队, 立即返回
// 如果队列非空(正在播放), 返回 false 丢弃请求
// 返回 true 表示已入队
bool PlayDinoSound(int type);

// 查询播放状态
bool IsAudioPlaying();

// 清空队列 — 丢弃所有排队中的播放请求
void FlushAudioQueue();
```

**设计要点:**
- `PlayDinoSound()` 在队列非空时拒绝新请求（不覆盖正在播放的声音）
- `FlushAudioQueue()` 被自主行为引擎在进入舒缓模式前调用，确保自然白噪音立即播放
- 播放任务绑定到 Core 1，与主循环 (Core 0) 解耦

### 1.7 音频文件部署

三种部署方式:

| 方式 | 工具/接口 | 说明 |
|------|----------|------|
| **串口烧录** | `scripts/build_opus_bin.py` | 将 .opus 文件打包为 TOC + 数据 bin，通过 `esptool` 烧录到 SPI Flash |
| **WiFi 上传** | `POST /api/flash/upload` | 浏览器网页直接上传 .opus 文件 (≤64KB/次) |
| **WiFi 同步** | `sync_audio_files()` | 上电自动从局域网服务器对比+下载 |

**网络同步流程 (sync_audio.cc):**

```
1. GET http://<SERVER_IP>:5000/api/files
   └─ 获取服务端 JSON 清单: [{"name":"xxx.opus","size":12345}, ...]

2. 对比本地 TOC 与服务端清单:
   ├─ 本地有, 服务端无 → 标记删除
   ├─ 本地有, 大小不同 → 标记更新 (先删后下)
   └─ 本地无, 服务端有 → 标记新增

3. 删除标记的文件 (flash_audio_delete_file)

4. 逐个流式下载新增/变更文件:
   GET http://<SERVER_IP>:5000/api/download-idx/<N>
   ├─ flash_audio_stream_begin() → 预先擦除所需扇区
   ├─ 4KB 分块接收 → flash_audio_stream_write() → 直接写 Flash
   └─ flash_audio_stream_end() → 更新 TOC

5. 每文件 1 次重试机会 (2s 间隔)
```

---

## 二、动作控制系统

### 2.1 舵机 PWM 底层

#### 硬件配置

| 参数 | 值 |
|------|-----|
| PWM 定时器 | `LEDC_TIMER_0`, 低速模式 |
| 频率 | 50 Hz |
| 分辨率 | 13-bit (0–8191) |
| 周期 | 20,000 μs |
| 通道 | `LEDC_CHANNEL_0` ~ `LEDC_CHANNEL_4` |

#### 角度→脉宽→占空比映射

```c
// 步骤 1: 角度 → 脉宽 (μs)
pulse_us = 500 + angle × 2000 / 180
//   0° → 500μs
//  90° → 1500μs
// 180° → 2500μs

// 步骤 2: 脉宽 → 占空比 (13-bit)
duty = pulse_us × 8192 / 20000
//  500μs → 205
// 1500μs → 614
// 2500μs → 1024
```

#### 初始化防抖序列

舵机上电瞬间若 GPIO 浮空，会收到随机 PWM 信号导致猛跳。解决:

```
① IO4 拉高            → 舵机供电 MOSFET 导通
② 所有舵机 GPIO → OUTPUT LOW  → 禁止浮空
③ 配置 LEDC 定时器 + 5 通道    → PWM 信号就绪
④ SetServoAngle(i, 90°) × 5   → 明确设置为中位
```

#### 5 路舵机索引

```cpp
enum ServoIndex {
    SERVO_NECK_TILT = 0,  // IO17 — 脖子上下
    SERVO_NECK_LEAN = 1,  // IO16 — 脖子左右倾
    SERVO_HEAD_TURN = 2,  // IO15 — 头左右转
    SERVO_TAIL_UD   = 3,  // IO8  — 尾巴上下
    SERVO_TAIL_LR   = 4,  // IO18 — 尾巴左右
};

void SetServoAngle(int idx, int angle);  // angle ∈ [0, 180]
```

### 2.2 波形生成算法

机械式的正弦运动看起来是"机器在晃"，而不是"动物在动"。自主行为引擎使用三种技术打破机械感:

#### 2.2.1 相位扭曲 (Phase Warp)

```cpp
inline float warp_phase(float linear) {
    float s = linear * linear * (3.0f - 2.0f * linear); // smoothstep
    return linear * 0.25f + s * 0.75f;
}
```

**原理:** 在正弦波的一个周期内，用 smoothstep 函数重新分配时间密度:
- 极值附近 (顶部/底部): 时间被拉伸 → 动作"停顿"更久 → 像肌肉在末端减速
- 过零点附近: 时间被压缩 → 动作快速穿过 → 像肌肉快速收缩/弹回

**效果对比:**

```
标准正弦:    ╱‾‾╲      ╱‾‾╲     — 匀速来回, 机械
             ╱    ╲    ╱    ╲
            ╱      ╲══╱      ╲

相位扭曲:    ╱‾‾‾╲     ╱‾‾‾╲    — 极值停久, 中间快速
            ╱      ╲   ╱      ╲
           ╱        ╲═╱        ╲
```

#### 2.2.2 谐波叠加 (Harmonic Overlay)

```cpp
inline float organic_sin(float x) {
    float wx = warp_phase(phase) × 2π;
    return sin(wx)                           // 基频 100%
         + 0.12 × sin(2·wx + 0.5)            // 2倍谐波 12%, 相位偏移
         + 0.05 × sin(3·wx + 1.2);           // 3倍谐波 5%, 相位偏移
}
```

**原理:** 添加小幅度的 2 倍、3 倍频率谐波并各自偏移相位，使波形**不对称**:
- 上升沿和下降沿形状不同 → 模拟"发力方向"和"放松方向"的速度差异
- 波形顶部不是完美的圆弧 → 模拟肌肉颤动

#### 2.2.3 中心漂移 (Center Drift)

```cpp
float drift_lr = 3.0f × sin(t × 0.12 + 0.8);  // ~52s 周期, 幅度 ±3°
float drift_ud = 2.5f × sin(t × 0.09 + 2.1);  // ~70s 周期, 幅度 ±2.5°
```

**原理:** 每个关节的中心位置不是固定的，而是在一个很缓慢的周期内小幅漂移。

**效果:** 动物不会永远回到同一个精确的点，而是像真动物一样每次停的位置略不同。

### 2.3 动作模型体系

动作系统将恐龙分解为三个部位组，每个部位组有多组运动模式:

```
恐龙动作模型
├── 脖子 (Neck)    — 2 轴: UD(IO17) + Lean(IO16)
│   ├── NECK_BREATHE      呼吸 (温和振荡)
│   ├── NECK_IDLE         待机 (微小动作)
│   ├── NECK_NOD          点头 (大幅度前后)
│   ├── NECK_SWAY         画圆 (前后+左右联动, 相位差 0.25)
│   ├── NECK_LOOK_LEFT    向左看 (重心偏左)
│   ├── NECK_LOOK_RIGHT   向右看 (重心偏右)
│   ├── NECK_ALERT        警觉 (前倾)
│   ├── NECK_SLEEP        睡眠 (后仰)
│   ├── NECK_PECK         啄食 (快速前后)
│   └── NECK_CURIOUS      好奇 (交替方向)
│
├── 头部 (Head)    — 1 轴: Turn(IO15)
│   ├── HEAD_CENTER       正中 (几乎不动)
│   ├── HEAD_LOOK_LEFT    向左看
│   ├── HEAD_LOOK_RIGHT   向右看
│   ├── HEAD_SCAN         全景扫描
│   └── HEAD_TILT_CURIOUS 好奇歪头
│
└── 尾巴 (Tail)    — 2 轴: UD(IO8) + LR(IO18)
    ├── TAIL_RELAX        放松 (温和摇摆)
    ├── TAIL_WAG          摇尾 (快速左右)
    ├── TAIL_RAISE        竖起
    ├── TAIL_DROOP        下垂
    ├── TAIL_CIRCLE       画圈
    ├── TAIL_HAPPY        高兴 (超快摇)
    ├── TAIL_ALERT        警觉 (竖起微晃)
    └── TAIL_TWITCH       抽动
```

#### 脖子模式参数详解

每个模式由 6 个参数定义:

```cpp
struct NeckParam {
    int   ct, at;      // Tilt  (前后): 中心, 幅度
    int   cl, al;      // Lean  (左右): 中心, 幅度
    float period;      // 周期 (秒)
    float phase_offs;  // Tilt 与 Lean 之间的相位差
};
```

| 模式 | tilt中心 | tilt幅 | lean中心 | lean幅 | 周期 | 相位差 | 运动轨迹描述 |
|------|---------|-------|---------|-------|------|--------|------------|
| BREATHE | 90 | 20 | 90 | 12 | 5.0s | 0.0 | 同步振荡，身体随呼吸起伏 |
| IDLE | 90 | 10 | 90 | 8 | 4.0s | 0.0 | 微小抖动，几乎看不出来 |
| NOD | 90 | 35 | 90 | 8 | 3.0s | 0.1 | 大点头 + 极微弱的左右 |
| SWAY | 90 | 22 | 90 | 25 | 3.5s | 0.25 | **画圆运动:** 90° 相位差 |
| LOOK_LEFT | 90 | 12 | **125** | 15 | 5.0s | 0.5 | 重心偏左 + 反向相位 |
| LOOK_RIGHT | 90 | 12 | **55** | 15 | 5.0s | 0.5 | 重心偏右 |
| ALERT | **115** | 10 | 90 | 6 | 5.5s | 0.0 | 身体前倾，几乎定住 |
| SLEEP | **65** | 8 | 90 | 5 | 7.0s | 0.0 | 后仰放松，缓慢呼吸 |
| PECK | 90 | 40 | 90 | 6 | 1.8s | 0.0 | 快速大幅度前后啄 |
| CURIOUS | 90 | 18 | 90 | 20 | 4.0s | 0.2 | 左右交替，不停探索 |

#### 尾巴模式参数详解

```cpp
struct TailParam {
    int   uc, ua;      // UD (上下): 中心, 幅度
    int   lc, la;      // LR (左右): 中心, 幅度
    float period;      // 周期 (秒)
    float phase_offs;  // UD 与 LR 之间的相位差
};
```

| 模式 | UD中心 | UD幅 | LR中心 | LR幅 | 周期 | 相位差 | 描述 |
|------|-------|-----|-------|-----|------|--------|------|
| RELAX | 90 | 20 | 90 | 25 | 4.0s | 0.22 | 温和画椭圆 |
| WAG | 90 | 5 | 90 | **65** | **1.2s** | 0.0 | 纯左右快速摇摆 |
| RAISE | **35** | 10 | 90 | 15 | 5.0s | 0.0 | 尾巴翘起来 (中心偏上) |
| DROOP | **145** | 10 | 90 | 15 | 5.0s | 0.0 | 尾巴垂下去 (中心偏下) |
| CIRCLE | 90 | 40 | 90 | 40 | 2.5s | **0.25** | 标准的圆圈 (90° 相位差) |
| HAPPY | 90 | 5 | 90 | 55 | **0.8s** | 0.0 | 高兴疯摇 (最快) |
| ALERT | **40** | 8 | 90 | 8 | 6.0s | 0.0 | 竖起且几乎不动 |
| TWITCH | 90 | 12 | 90 | 40 | 1.5s | 0.15 | 快速抽动 |

#### 动作池分类

不同运行状态使用不同的动作子集，保证多样性同时约束表现力:

```cpp
// 舒缓模式 — 只选最温柔的动作
static constexpr NeckMode kRelaxNeck[] = {NECK_BREATHE, NECK_IDLE, NECK_SLEEP, NECK_SWAY};
static constexpr TailMode kRelaxTail[] = {TAIL_RELAX, TAIL_DROOP, TAIL_ALERT};

// Idle 模式 — 包含所有非极端动作
static constexpr NeckMode kIdleNeck[] = {
    NECK_BREATHE, NECK_IDLE, NECK_SWAY, NECK_CURIOUS,
    NECK_LOOK_LEFT, NECK_LOOK_RIGHT, NECK_SLEEP};
static constexpr TailMode kIdleTail[] = {
    TAIL_RELAX, TAIL_WAG, TAIL_CIRCLE, TAIL_ALERT, TAIL_TWITCH, TAIL_RAISE};

// 静止姿态 — 用于"动-停交替"中的停
static constexpr NeckMode kStillNeck[] = {NECK_SLEEP, NECK_IDLE,
                                          NECK_LOOK_LEFT, NECK_LOOK_RIGHT};
static constexpr TailMode kStillTail[] = {TAIL_DROOP, TAIL_DROOP, TAIL_ALERT};
//                                        DROOP 权重 ×3 — 大多数时候尾巴是垂着的
```

### 2.4 运行状态机

自主行为引擎以 `dino_auto` 任务运行 (FreeRTOS 优先级 2, 4KB 栈, 50FPS / 20ms tick)。

#### 状态转换图

```
                          ┌─────────────────────────────────────┐
                          │            IDLE (默认状态)            │
                          │                                     │
                          │  • 10–20s 动作模式随机轮换            │
                          │  • 15–35s 自发微动作                  │
                          │  • 20–40s 间隔触发随机音效            │
                          │  • 幅度 65–90%, 5s 衰减至 75%         │
                          └──────┬──────────────┬───────────────┘
                                 │              │
                    音效触发      │              │ 120s 定时器
                    (播放开始)     │              │ (无音效期间累计)
                                 ▼              ▼
              ┌────────────────────┐   ┌──────────────────────────┐
              │  REACTION (反应)    │   │   RELAX (舒缓)            │
              │                    │   │                          │
              │  • 音效→动作联动    │   │  • 播放自然白噪音 (索引0)   │
              │  • 幅度 80–95%     │   │  • 3–6s 动作轮换           │
              │  • 持续 4–7s       │   │  • 幅度 75–100%            │
              │  • 结束后回 IDLE   │   │  • 多频段有机晃动叠加       │
              │  • 硬摆模式可选    │   │  • 音效结束 → 回 IDLE       │
              └────────┬───────────┘   └─────────────┬────────────┘
                       │                             │
                       └────────── 回到 IDLE ─────────┘
```

#### IDLE 模式时间线

```
t=0     t=10s       t=20s      t=25s       t=35s      t=40s       t=50s
│────────│──────────│──────────│──────────│──────────│──────────│
│        │          │          │          │          │          │
│   动作A  │  动作B    │ 微动作   │  动作C    │  音效触发 │   动作D    │
│  (SWAY)  │(LOOK_LEFT│(TWITCH) │ (CURIOUS) │ + 联动    │  (IDLE)   │
│          │          │          │          │           │          │
└─ idle轮换 ─┘       └─ spont ─┘           └─ reaction(4-7s) ────┘
```

#### 幅度衰减

每个新动作从完整幅度开始，5 秒内线性衰减至 75%，模拟动物从"兴奋"回归"平静"的自然过程:

```cpp
constexpr int TICK_DECAY = 5000 / 20;  // 250 ticks = 5 seconds

float decay = 1.0f;
uint32_t dt = tick - amp_start;
if (dt < TICK_DECAY)
    decay = 1.0f - (float)dt / (float)TICK_DECAY × 0.25f;  // 100% → 75%
else
    decay = 0.75f;
```

#### 舒缓模式晃动叠加 (Nature Wobble)

在舒缓模式下，除了当前动作模式外，额外叠加一个多频段的有机晃动:

```cpp
static void apply_nature_wobble(tick, neck_tilt, neck_lean, tail_ud, tail_lr) {
    float nt  = tick × 20ms;
    float w1  = sin(nt × 0.17)      × 18.0;   // 脖子前后: ~37s 周期
    float w2  = sin(nt × 0.35 + 1.0) × 12.0;  // 脖子左右: ~18s 周期
    float w1u = cos(nt × 0.21)      × 10.0;   // 尾巴上下: ~30s 周期
    float w2u = sin(nt × 0.43 + 0.7) × 8.0;   // 尾巴左右: ~15s 周期

    // 混合: 35% 原有动作 + 65% 舒缓摇晃
    neck_tilt = neck_tilt × 0.35 + (90 + w1)  × 0.65;
    neck_lean = neck_lean × 0.35 + (90 + w2)  × 0.65;
    tail_ud   = tail_ud   × 0.35 + (90 + w1u) × 0.65;
    tail_lr   = tail_lr   × 0.35 + (90 + w2u) × 0.65;
}
```

四个频率都是**无理数比例** (0.17, 0.21, 0.35, 0.43)，确保晃动模式不会周期性重复。

### 2.5 交叉渐变 (Crossfade)

动作之间通过 **3 秒 smoothstep** 过渡，避免突变:

```
动作A ──────────────────────╲
                              ╲  smoothstep 混合 (bt 0→1)
                               ╲────────────────────── 动作B
                                ╲
                                 ╲
  t=0                          t=3s

  混合系数 mx = bt² × (3 - 2·bt)     ← smoothstep 曲线
  输出角度 = A × (1 - mx) + B × mx
```

#### 实现细节

```cpp
struct XFade {
    bool     active;           // 渐变是否激活
    uint32_t start_tick;      // 开始 tick
    NeckMode from_neck;       // 旧脖子模式
    HeadMode from_head;       // 旧头部模式
    TailMode from_tail;       // 旧尾部模式
    float    from_neck_amp;   // 旧幅度
    float    from_head_amp;
    float    from_tail_amp;
};
```

渐变计算函数同时计算新旧两种模式的角度，然后按 smoothstep 系数混合:

```cpp
float bt = (tick - xfade.start_tick) × 20ms / 1000.0f;   // 已过秒数
if (bt >= 3.0f) {
    // 渐变完成，直接使用新模式
    return new_angle;
}
float mx = bt × bt × (3.0f - 2.0f × bt);   // smoothstep 0→1
return old_angle + (new_angle - old_angle) × mx;
```

**重要:** 渐变期间同时运行旧模式和新模式的波形计算，这对脖子(2轴)和尾巴(2轴)来说意味着每个关节需要计算 4 个正弦值。

### 2.6 音效→动作联动

`sound_to_action()` 函数将音效类型映射为动作组合，每次调用有内部随机分支保证多样性:

```cpp
static void sound_to_action(int sound, NeckMode &nm, HeadMode &hm, TailMode &tm,
                             float &na, float &ha, float &ta, bool &hd) {
    int r = irnd(100);  // 0–99 随机数

    switch (sound) {
    case 1: case 2:  // 恐龙叫声
        if      (r < 30) { NECK_ALERT, HEAD_SCAN,      TAIL_ALERT,  90%, 80%, 85% }
        else if (r < 55) { NECK_LOOK_LEFT, HEAD_LOOK_LEFT, TAIL_WAG, 85%, 90%, 80% }
        else if (r < 75) { NECK_NOD,  HEAD_CENTER,     TAIL_TWITCH, 80%, 50%, 90%, HARD }
        else             { NECK_SWAY, HEAD_SCAN,       TAIL_CIRCLE, 85%, 80%, 80% }
        break;

    case 3:  // 进食音效
        // 侧重于啄食(PECK)和点头(NOD)
        if      (r < 40) { NECK_PECK, ... }
        else if (r < 70) { NECK_NOD,  ... }
        else             { NECK_CURIOUS, ... }
        break;

    case 4:  // 幼崽音效
        // 更活泼，更高概率触发 HARD 模式
        if      (r < 50) { NECK_SWAY, HEAD_SCAN, TAIL_WAG, ... }
        ...
        break;
    }
}
```

**概率设计:** 比如对于恐龙叫声，30% 概率警觉、25% 看左边、20% 点头+抽尾、25% 画圆。这种分布使得同一音效每次触发产生不同的动作组合。

`hd = true` (硬摆模式) 以较低概率触发，施加 smoothstep 波形锐化 + 4× 速度。

### 2.7 Web 控制面板

内嵌在 `http_server.cc` 中，地址为 `http://<ESP32_IP>/`。

#### 控制面板功能

```
┌──────────────────────────────────────────────┐
│           Dino Pet Controller                 │
│                                              │
│  ┌──────────────────────────────────────┐    │
│  │ Neck 脖子 (IO17: 上下  IO16: 左右)    │    │
│  │ Tilt [═══════●═══════] 90°           │    │
│  │ Lean [═══════●═══════] 90°           │    │
│  └──────────────────────────────────────┘    │
│  ┌──────────────────────────────────────┐    │
│  │ Head 头部 (IO15: 转头)               │    │
│  │ Turn [═══════●═══════] 90°           │    │
│  └──────────────────────────────────────┘    │
│  ┌──────────────────────────────────────┐    │
│  │ Tail 尾巴 (IO8: 上下  IO18: 左右)    │    │
│  │ UD   [═══════●═══════] 90°           │    │
│  │ LR   [═══════●═══════] 90°           │    │
│  └──────────────────────────────────────┘    │
│                                              │
│  Quick Presets:                               │
│  [全部中位] [后仰] [前倾] [右倾] [左倾]        │
│  [头右转] [头左转] [尾巴上翘] [尾巴下垂]        │
│                                              │
│  Battery: 3850mV | Level: 65%                 │
└──────────────────────────────────────────────┘
```

#### HTTP API 规范

| 方法 | 路径 | 请求体 | 响应 | 说明 |
|------|------|--------|------|------|
| GET | `/` | — | HTML | 控制面板 |
| POST | `/api/servo` | `{"angles":[a0,a1,a2,a3,a4]}` | `{"ok":true}` | 设置 5 路舵机 |
| GET | `/api/battery` | — | `{"voltage_mv":3850,"level":65}` | 电池状态 |
| GET | `/api/autoplay` | — | `{"autoplay":true,"hard_swing":false}` | 查询自主运行 |
| POST | `/api/autoplay` | `{"enable":1}` 或 `{"hard_swing":true}` | 同上 | 开关/切换 |
| GET | `/flash` | — | HTML | Flash 文件管理页面 |
| GET | `/api/flash/status` | — | JSON | 文件列表 |
| POST | `/api/flash/upload` | multipart/form-data | `"OK"` | 上传 .opus |
| POST | `/api/flash/erase` | — | `"OK"` | 擦除全部 |

**Web 控制面板特性:**
- 5 个滑块实时控制所有舵机 (0°–180°)
- 40ms 节流 (throttle) 避免网络拥塞
- 快捷预设按钮快速切换姿态
- 电池状态每 5 秒自动轮询更新

---

## 文件索引

| 文件 | 职责 |
|------|------|
| [audio.h](main/audio.h) / [audio.cc](main/audio.cc) | 音频初始化、播放队列、I2S 输出 |
| [ogg_demuxer.h](main/ogg_demuxer.h) / [ogg_demuxer.cc](main/ogg_demuxer.cc) | OGG 容器解封装状态机 |
| [flash_audio.h](main/flash_audio.h) / [flash_audio.cc](main/flash_audio.cc) | SPI Flash 文件系统 (TOC + 数据区) |
| [w25q256.h](main/w25q256.h) / [w25q256.cc](main/w25q256.cc) | W25Q256 SPI NOR Flash 底层驱动 |
| [sync_audio.h](main/sync_audio.h) / [sync_audio.cc](main/sync_audio.cc) | WiFi 局域网音频同步客户端 |
| [flash_upload_server.h](main/flash_upload_server.h) / [flash_upload_server.cc](main/flash_upload_server.cc) | Web Flash 管理 + 音频试听接口 |
| [servo.h](main/servo.h) / [servo.cc](main/servo.cc) | 5 通道 LEDC PWM 舵机驱动 |
| [auto_run.h](main/auto_run.h) / [auto_run.cc](main/auto_run.cc) | 自主行为引擎 (波形+状态机+渐变) |
| [http_server.h](main/http_server.h) / [http_server.cc](main/http_server.cc) | HTTP 服务器 + Web 控制面板 |
| [config.h](main/config.h) | 全局引脚定义和可调参数 |
