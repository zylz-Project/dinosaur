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

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <esp_heap_caps.h>

static const char *TAG = "dino_audio";

static esp_codec_dev_handle_t dev_ = nullptr;
static i2c_master_bus_handle_t i2c_bus_ = nullptr;
static i2s_chan_handle_t tx_chan_ = nullptr;
static QueueHandle_t sound_queue_ = nullptr;
static volatile bool is_playing_ = false;

#define I2S_CHUNK_SAMPLES 240

static void AudioPlayTask(void *arg) {
  int type;
  while (true) {
    if (xQueueReceive(sound_queue_, &type, portMAX_DELAY) != pdTRUE) continue;

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
        esp_codec_dev_write(dev_, ((int16_t *)pcm) + off, chunk_n * sizeof(int16_t));
        vTaskDelay(pdMS_TO_TICKS(3));
      }
    });

    printf("I (%lu) %s: Playing #%d: %s (%lu bytes)\n",
           (unsigned long)esp_log_timestamp(), TAG,
           idx, info.name, (unsigned long)info.size);
    is_playing_ = true;

    // Stream file from SPI Flash in 4KB chunks
    uint8_t chunk[4096];
    uint32_t f_off = 0;
    while (f_off < info.size && !dec_fail) {
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
  if (i2s_new_channel(&ch, &tx_chan_, nullptr) != ESP_OK) return;

  i2s_std_config_t st = {};
  st.clk_cfg.sample_rate_hz = 48000; st.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  st.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  st.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  st.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO; st.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  st.gpio_cfg.mclk = AUDIO_I2S_GPIO_MCLK; st.gpio_cfg.bclk = AUDIO_I2S_GPIO_BCLK;
  st.gpio_cfg.ws = AUDIO_I2S_GPIO_WS; st.gpio_cfg.dout = AUDIO_I2S_GPIO_DOUT;
  st.gpio_cfg.din = AUDIO_I2S_GPIO_DIN;
  if (i2s_channel_init_std_mode(tx_chan_, &st) != ESP_OK) return;
  if (i2s_channel_enable(tx_chan_) != ESP_OK) return;

  audio_codec_i2s_cfg_t is = {.port = I2S_NUM_0, .tx_handle = tx_chan_};
  auto *d = audio_codec_new_i2s_data(&is);
  audio_codec_i2c_cfg_t icc = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus_};
  auto *c = audio_codec_new_i2c_ctrl(&icc);
  auto *g = audio_codec_new_gpio();

  es8311_codec_cfg_t es = {};
  es.ctrl_if = c; es.gpio_if = g; es.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es.pa_pin = GPIO_NUM_NC; es.use_mclk = false;
  es.hw_gain.pa_voltage = 5.0f; es.hw_gain.codec_dac_voltage = 3.3f; es.no_dac_ref = true;

  esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es), .data_if = d};
  dev_ = esp_codec_dev_new(&dc);
  esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = 48000, .mclk_multiple = I2S_MCLK_MULTIPLE_256};
  esp_codec_dev_open(dev_, &fs);
  esp_codec_dev_set_out_vol(dev_, AUDIO_OUTPUT_VOLUME);

  ESP_LOGI(TAG, "Ready: ES8311 48kHz vol %d%%", AUDIO_OUTPUT_VOLUME);

  sound_queue_ = xQueueCreate(8, sizeof(int));
  xTaskCreatePinnedToCore(AudioPlayTask, "dino_play", 32768, nullptr, 3, nullptr, 1);
}

bool PlayDinoSound(int t) {
  if (!sound_queue_ || uxQueueMessagesWaiting(sound_queue_) > 0) return false;
  return xQueueSend(sound_queue_, &t, 0) == pdTRUE;
}
bool IsAudioPlaying() { return is_playing_; }
void FlushAudioQueue() {
  if (!sound_queue_) return;
  int dummy; while (xQueueReceive(sound_queue_, &dummy, 0) == pdTRUE) {}
}
