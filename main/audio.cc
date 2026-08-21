#include "audio.h"
#include "config.h"
#include "flash_audio.h"
#include "ogg_demuxer.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <es8311_codec.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <decoder/impl/esp_opus_dec.h>
#include <esp_audio_types.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <esp_heap_caps.h>

static const char *TAG = "dino_audio";

static esp_codec_dev_handle_t dev_ = nullptr;
static i2c_master_bus_handle_t i2c_bus_ = nullptr;
static i2s_chan_handle_t tx_chan_ = nullptr;
static i2s_chan_handle_t rx_chan_ = nullptr;
static QueueHandle_t sound_queue_ = nullptr;
static SemaphoreHandle_t audio_mutex_ = nullptr;
static volatile bool is_playing_ = false;
static volatile bool stop_playback_ = false;   // set to interrupt current sound
static volatile float motion_level_ = 0.0f;
static volatile float motion_slow_level_ = 0.0f;
static volatile float motion_attack_ = 0.0f;
static volatile uint32_t motion_elapsed_ms_ = 0;
static volatile uint32_t motion_duration_ms_ = 0;
static volatile int motion_sound_index_ = -1;

#define I2S_CHUNK_SAMPLES 240

static void AudioPlayTask(void *arg) {
  int type;
  while (true) {
    if (xQueueReceive(sound_queue_, &type, portMAX_DELAY) != pdTRUE) continue;

    stop_playback_ = false;  // new sound: clear interrupt flag

    // enum value = TOC index
    int idx = type;
    flash_audio_info_t info;
    if (flash_audio_get_file_info(idx, &info) != ESP_OK) {
      continue;  // file was deleted (e.g. flash erased at runtime), skip silently
    }

    // PCM output buffer (60ms @ 48kHz mono = 2880 samples * 2 bytes = 5760 bytes)
    const uint32_t pcm_buf_sz = 48000 * 60 / 1000 * sizeof(int16_t);
    uint8_t *pcm = (uint8_t *)heap_caps_malloc(pcm_buf_sz, MALLOC_CAP_INTERNAL);
    if (!pcm) continue;

    // Streaming mode: read file from flash in 4KB chunks,
    // OGG-demux on-the-fly, decode & play each packet immediately
    auto dm = std::make_unique<OggDemuxer>();
    void *dec = nullptr;
    int sr = 48000;
    int pkt_count = 0;
    bool dec_fail = false;

    dm->OnDemuxerFinished([&](const uint8_t *d, int s, size_t n) {
      // Lazy-init decoder on first audio packet
      if (!dec) {
        sr = s;
        esp_opus_dec_cfg_t cfg = {};
        cfg.sample_rate    = 48000;
        cfg.channel        = ESP_AUDIO_MONO;
        cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        cfg.self_delimited = false;
        if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK) {
          ESP_LOGE(TAG, "Decoder open fail");
          dec_fail = true;
          return;
        }
      }

      pkt_count++;

      esp_audio_dec_in_raw_t raw = {};
      raw.buffer = (uint8_t *)d;
      raw.len    = (uint32_t)n;
      raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

      esp_audio_dec_out_frame_t out = {};
      out.buffer = pcm;
      out.len    = pcm_buf_sz;

      esp_audio_dec_info_t dec_info = {};
      int ret = esp_opus_dec_decode(dec, &raw, &out, &dec_info);
      if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decode fail pkt %d: %d (len=%zu)", pkt_count, ret, n);
        dec_fail = true;
        return;
      }

      // Output to I2S
      size_t sn = out.decoded_size / sizeof(int16_t);
      for (size_t off = 0; off < sn; off += I2S_CHUNK_SAMPLES) {
        size_t chunk_n = sn - off;
        if (chunk_n > I2S_CHUNK_SAMPLES) chunk_n = I2S_CHUNK_SAMPLES;
        int16_t *samples = ((int16_t *)pcm) + off;

        // Mean absolute PCM level is inexpensive at this 240-sample cadence.
        // Two envelopes separate sustained loudness from a fresh onset so the
        // neck can follow syllables/bites without jittering on every sample.
        uint32_t absolute_sum = 0;
        for (size_t i = 0; i < chunk_n; ++i) {
          int sample = samples[i];
          absolute_sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
        }
        float mean = chunk_n > 0
                         ? static_cast<float>(absolute_sum) / static_cast<float>(chunk_n)
                         : 0.0f;
        float raw_level = mean / 7200.0f;
        if (raw_level > 1.0f) raw_level = 1.0f;
        float fast = motion_level_ * 0.72f + raw_level * 0.28f;
        float slow = motion_slow_level_ * 0.94f + raw_level * 0.06f;
        float onset = (fast - slow) * 3.2f;
        if (onset < 0.0f) onset = 0.0f;
        if (onset > 1.0f) onset = 1.0f;
        motion_level_ = fast;
        motion_slow_level_ = slow;
        motion_attack_ = motion_attack_ * 0.58f + onset * 0.42f;
        motion_elapsed_ms_ += static_cast<uint32_t>(chunk_n * 1000U / 48000U);

        if (stop_playback_) break;
        // Serialized against chat playback via audio_mutex_.
        if (audio_mutex_ && xSemaphoreTake(audio_mutex_, portMAX_DELAY) != pdTRUE)
          continue;
        esp_codec_dev_write(dev_, samples, chunk_n * sizeof(int16_t));
        if (audio_mutex_) xSemaphoreGive(audio_mutex_);
        vTaskDelay(pdMS_TO_TICKS(3));
      }
    });

    printf("I (%lu) %s: Playing #%d: %s (%lu bytes)\n",
           (unsigned long)esp_log_timestamp(), TAG,
           idx, info.name, (unsigned long)info.size);
    motion_level_ = 0.0f;
    motion_slow_level_ = 0.0f;
    motion_attack_ = 0.0f;
    motion_elapsed_ms_ = 0;
    motion_duration_ms_ = info.duration_ms;
    motion_sound_index_ = idx;
    is_playing_ = true;

    // Stream file from SPI Flash in 4KB chunks
    uint8_t chunk[4096];
    uint32_t f_off = 0;
    while (f_off < info.size && !dec_fail && !stop_playback_) {
      size_t n = info.size - f_off;
      if (n > sizeof(chunk)) n = sizeof(chunk);
      if (flash_audio_read_file(idx, f_off, chunk, n) != ESP_OK) {
        ESP_LOGE(TAG, "Flash read fail @ %lu", (unsigned long)f_off);
        break;
      }
      dm->Process(chunk, n);
      f_off += n;
    }

    if (dec) esp_opus_dec_close(dec);
    heap_caps_free(pcm);
    is_playing_ = false;
    motion_level_ = 0.0f;
    motion_slow_level_ = 0.0f;
    motion_attack_ = 0.0f;

    if (!dec_fail) printf("I (%lu) %s: Done #%d: %d pkts, sr=%d\n",
                          (unsigned long)esp_log_timestamp(), TAG, idx, pkt_count, sr);
    else printf("W (%lu) %s: Failed #%d\n",
                (unsigned long)esp_log_timestamp(), TAG, idx);
  }
}

void InitAudio()
{
  ESP_LOGI(TAG, "InitAudio (48kHz)...");

  i2c_master_bus_config_t ic = {};
  ic.i2c_port = I2C_NUM_0; ic.sda_io_num = AUDIO_I2C_SDA_PIN; ic.scl_io_num = AUDIO_I2C_SCL_PIN;
  ic.clk_source = I2C_CLK_SRC_DEFAULT;
  ic.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&ic, &i2c_bus_) != ESP_OK) return;

  if (i2c_master_probe(i2c_bus_, ES8311_CODEC_DEFAULT_ADDR>>1, pdMS_TO_TICKS(200)) != ESP_OK) {
    ESP_LOGW(TAG, "No ES8311"); return;
  }

i2s_chan_config_t ch = {}; ch.id = I2S_NUM_0; ch.role = I2S_ROLE_MASTER;
  ch.dma_desc_num = 6; ch.dma_frame_num = 240; ch.auto_clear_after_cb = true;
  if (i2s_new_channel(&ch, &tx_chan_, &rx_chan_) != ESP_OK) return;

  i2s_std_config_t st = {};
  st.clk_cfg.sample_rate_hz = 48000; st.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  st.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  st.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  st.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO; st.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  st.gpio_cfg.mclk = AUDIO_I2S_GPIO_MCLK; st.gpio_cfg.bclk = AUDIO_I2S_GPIO_BCLK;
  st.gpio_cfg.ws = AUDIO_I2S_GPIO_WS; st.gpio_cfg.dout = AUDIO_I2S_GPIO_DOUT;
  st.gpio_cfg.din = AUDIO_I2S_GPIO_DIN;
  if (i2s_channel_init_std_mode(tx_chan_, &st) != ESP_OK) return;
  if (i2s_channel_init_std_mode(rx_chan_, &st) != ESP_OK) return;
  if (i2s_channel_enable(tx_chan_) != ESP_OK) return;
  if (i2s_channel_enable(rx_chan_) != ESP_OK) return;

  audio_codec_i2s_cfg_t is = {.port = I2S_NUM_0, .rx_handle = rx_chan_, .tx_handle = tx_chan_, .clk_src = 0};
  auto *d = audio_codec_new_i2s_data(&is);
  audio_codec_i2c_cfg_t icc = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus_};
  auto *c = audio_codec_new_i2c_ctrl(&icc);
  auto *g = audio_codec_new_gpio();

  es8311_codec_cfg_t es = {};
  es.ctrl_if = c; es.gpio_if = g; es.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
  es.pa_pin = GPIO_NUM_NC; es.use_mclk = false;
  es.hw_gain.pa_voltage = 5.0f; es.hw_gain.codec_dac_voltage = 3.3f; es.no_dac_ref = true;

  esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = es8311_codec_new(&es), .data_if = d};
  dev_ = esp_codec_dev_new(&dc);
  esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = 48000, .mclk_multiple = I2S_MCLK_MULTIPLE_256};
  esp_codec_dev_open(dev_, &fs);
  esp_codec_dev_set_in_gain(dev_, 42);  // ES8311 麦克风增益最大档 42dB
  esp_codec_dev_set_out_vol(dev_, AUDIO_OUTPUT_VOLUME);

  audio_mutex_ = xSemaphoreCreateMutex();

  ESP_LOGI(TAG, "Ready: ES8311 48kHz duplex (mic+spk), vol %d%%", AUDIO_OUTPUT_VOLUME);

  sound_queue_ = xQueueCreate(8, sizeof(int));
  xTaskCreatePinnedToCore(AudioPlayTask, "dino_play", 32768, nullptr, 3, nullptr, 1);
}

bool PlayDinoSound(int t) {
  if (!sound_queue_ || uxQueueMessagesWaiting(sound_queue_) > 0) return false;
  return xQueueSend(sound_queue_, &t, 0) == pdTRUE;
}

// --- Power on/off chimes -----------------------------------------------------
// Generates a short sine melody in-code and writes it straight to the codec.
// This is deliberately independent of Flash audio so the shutdown chime always
// plays, even after audio files have been unmounted or the TOC is gone.
namespace {
constexpr float kPi = 3.14159265f;
constexpr int kToneAmplitude = 8000;   // gentle, well under 16-bit full scale

void WriteTone(const int *freqs, const int *durs_ms, int count) {
  if (!dev_) return;
  const int sr = AUDIO_SAMPLE_RATE;
  const int fade_n = sr * 5 / 1000;    // 5ms fade in/out to avoid clicks
  for (int k = 0; k < count; ++k) {
    const int n = sr * durs_ms[k] / 1000;
    std::vector<int16_t> buf(n);
    for (int i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) / sr;
      float fade = 1.0f;
      if (i < fade_n) fade = static_cast<float>(i) / fade_n;
      else if (i > n - fade_n) fade = static_cast<float>(n - i) / fade_n;
      buf[i] = static_cast<int16_t>(kToneAmplitude * fade *
                                    sinf(2.0f * kPi * freqs[k] * t));
    }
    esp_codec_dev_write(dev_, buf.data(), n * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}  // namespace

void PlayBootTone() {
  // Rising two-note chirp — "powering on".
  const int freqs[] = {880, 1320};
  const int durs[] = {120, 160};
  WriteTone(freqs, durs, 2);
}

void PlayShutdownTone() {
  // Falling two-note chirp — "powering off".
  FlushAudioQueue();  // drop any queued Flash sound so the chime is clean
  const int freqs[] = {1320, 880};
  const int durs[] = {120, 200};
  WriteTone(freqs, durs, 2);
}

bool IsAudioPlaying() { return is_playing_; }
AudioMotionData GetAudioMotionData() {
  AudioMotionData data{};
  data.playing = is_playing_;
  data.level = motion_level_;
  data.attack = motion_attack_;
  data.elapsed_ms = motion_elapsed_ms_;
  data.duration_ms = motion_duration_ms_;
  data.sound_index = motion_sound_index_;
  return data;
}
void FlushAudioQueue() {
  if (!sound_queue_) return;
  int dummy; while (xQueueReceive(sound_queue_, &dummy, 0) == pdTRUE) {}
}

void AudioStopCurrent() {
  stop_playback_ = true;
}

int AudioReadMic48k(int16_t *buf, int samples) {
  if (!dev_ || !buf || samples <= 0) return 0;
  if (audio_mutex_ && xSemaphoreTake(audio_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  int ret = esp_codec_dev_read(dev_, buf, samples * (int)sizeof(int16_t));
  if (audio_mutex_) xSemaphoreGive(audio_mutex_);
  return ret == ESP_CODEC_DEV_OK ? samples * (int)sizeof(int16_t) : 0;
}

void AudioWritePcm48k(const int16_t *pcm, int samples) {
  if (!dev_ || !pcm || samples <= 0) return;
  // Chat is half-duplex: while TTS is playing the mic task is paused.  Never
  // discard a TTS block merely because the final in-flight mic read still owns
  // the codec briefly; a dropped block is heard as a click or short gap.
  if (audio_mutex_ && xSemaphoreTake(audio_mutex_, portMAX_DELAY) != pdTRUE) return;
  esp_codec_dev_write(dev_, (void *)pcm, samples * (int)sizeof(int16_t));
  if (audio_mutex_) xSemaphoreGive(audio_mutex_);
}

void AudioPlayChatReadyTone() {
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr int kSampleRate = 48000;
  static constexpr int kChunkSamples = 240;
  const int tones[] = {880, 1100};
  const int durations_ms[] = {80, 120};
  int16_t chunk[kChunkSamples];

  // Short ascending "ready" sound.  Generate it in small blocks so the prompt
  // needs no flash access or large allocation.
  for (int t = 0; t < 2; ++t) {
    const int total = kSampleRate * durations_ms[t] / 1000;
    const int fade = total / 8;
    for (int pos = 0; pos < total; pos += kChunkSamples) {
      int count = total - pos;
      if (count > kChunkSamples) count = kChunkSamples;
      for (int i = 0; i < count; ++i) {
        const int sample = pos + i;
        float gain = 1.0f;
        if (sample < fade) gain = (float)sample / (float)fade;
        else if (sample >= total - fade)
          gain = (float)(total - sample - 1) / (float)fade;
        chunk[i] = (int16_t)(sinf(2.0f * kPi * tones[t] * sample /
                                  (float)kSampleRate) * 8000.0f * gain);
      }
      AudioWritePcm48k(chunk, count);
    }
    if (t == 0) vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// 合成提示音（不上 flash，直接生成正弦波 + 包络）
static void ChimeTone(float freq, int dur_ms, float vol) {
  const int sr = AUDIO_SAMPLE_RATE;
  const int chunk = 240;
  const int total = sr * dur_ms / 1000;
  int16_t buf[chunk];
  for (int pos = 0; pos < total; pos += chunk) {
    int n = total - pos;
    if (n > chunk) n = chunk;
    for (int i = 0; i < n; i++) {
      float pi = (float)(pos + i);
      float t = pi / (float)sr;
      float attack = pi < sr * 0.010f ? pi / (sr * 0.010f) : 1.0f;
      float rb = (float)total - sr * 0.040f;
      float release = pi > rb ? (1.0f - (pi - rb) / (sr * 0.040f)) : 1.0f;
      if (release < 0.0f) release = 0.0f;
      float v = vol * attack * release * std::sin(2.0f * (float)M_PI * freq * t);
      buf[i] = (int16_t)(v * 32767.0f);
    }
    AudioWritePcm48k(buf, n);
  }
}

void AudioPlayChime(bool ascending) {
  if (ascending) {
    ChimeTone(784.0f, 130, 0.50f);    // G5
    ChimeTone(1174.66f, 200, 0.45f);  // D6
  } else {
    ChimeTone(1174.66f, 130, 0.50f);
    ChimeTone(784.0f, 200, 0.45f);
  }
}
