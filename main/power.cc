#include "power.h"
#include "config.h"
#include "servo.h"
#include "audio.h"
#include "chat.h"

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "dino_power";

static adc_oneshot_unit_handle_t adc_handle_ = nullptr;
static adc_cali_handle_t adc_cali_handle_ = nullptr;
static int battery_vbat_filtered_mv_ = 0;
static int battery_level_ = 0;

static int AdcToMv(int raw)
{
  if (adc_cali_handle_)
  {
    int mv = 0;
    if (adc_cali_raw_to_voltage(adc_cali_handle_, raw, &mv) == ESP_OK)
      return mv;
  }
  return raw * 3300 / 4096;
}

// --- Power-button state machine ----------------------------------------------
// Mirrors the testBoard reference: debounce on both edges, long-press on the
// PRESSED state, and a boot-hold "armed" guard so that the button being held to
// switch the device on is never mistaken for a shutdown request.
typedef enum {
  BTN_IDLE = 0,
  BTN_DEBOUNCE_PRESS,
  BTN_PRESSED,
  BTN_DEBOUNCE_RELEASE,
} power_btn_state_t;

static power_btn_state_t s_btn_state = BTN_IDLE;
static uint32_t s_btn_state_since = 0;  // ms (esp_log_timestamp)
static uint32_t s_btn_press_start = 0;  // ms
static int s_btn_hold_tip = 0;          // last progress hint (500ms steps)
static uint32_t s_last_click_ms = 0;    // ms of last short press release
static int s_click_count = 0;           // consecutive short presses
// Long-press shutdown is inert until the button has been released once after
// boot — at power-on the button is necessarily held, and we must not fire then.
static bool s_btn_armed = false;
static bool s_btn_boot_hold_logged = false;

static void PowerLatchInit()
{
  gpio_config_t pwr_ctrl = {
      .pin_bit_mask = 1ULL << POWER_CTRL_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_ctrl);
  gpio_set_level(POWER_CTRL_GPIO, 1);
  ESP_LOGI(TAG, "POWER_CTRL IO%d HIGH, power latched", POWER_CTRL_GPIO);
}

// Full shutdown: chime → center servos → cut servo power → release latch.
static void ShutdownSequence()
{
  ESP_LOGW(TAG, "⏻ Long press %dms — running shutdown sequence", POWER_LONG_PRESS_MS);

  // 0. Center ALL servos simultaneously, right now — the whole body returns to
  //    a neutral pose the moment power-off begins. The servo rail stays powered
  //    so the commands can actually take effect.
  for (int i = 0; i < kServoCount; ++i)
    SetServoAngle(i, 90);

  // 1. Power-off chime plays while the servos physically move to centre.
  PlayShutdownTone();

  // 2. Let the servos finish reaching centre.
  vTaskDelay(pdMS_TO_TICKS(300));

  // 3. Cut the servo power rail.
  gpio_set_level(SERVO_POWER_GPIO, 0);
  ESP_LOGI(TAG, "Servo power IO%d LOW", SERVO_POWER_GPIO);
  vTaskDelay(pdMS_TO_TICKS(300));

  // 4. Release the power latch — board powers down.
  gpio_set_level(POWER_CTRL_GPIO, 0);
  ESP_LOGW(TAG, "POWER_CTRL IO%d LOW, system powering off", POWER_CTRL_GPIO);

  while (true)
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void InitPower()
{
  PowerLatchInit();

  // ADC init: IO6 power button (CH5), IO3 battery (CH2).
  adc_oneshot_unit_init_cfg_t adc_cfg = {
      .unit_id = ADC_UNIT_1,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&adc_cfg, &adc_handle_) == ESP_OK)
  {
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_5, &ch);        // IO6 power button
    adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &ch);  // IO3 battery

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_5,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cali, &adc_cali_handle_);
  }

  // Power monitor: button state machine + long-press shutdown + battery.
  xTaskCreate([](void *)
              {
    vTaskDelay(pdMS_TO_TICKS(3000));  // let the boot-time button hold settle

    uint32_t bat_elapsed = 0;

    while (true) {
      const uint32_t now = esp_log_timestamp();

      // --- Power button (IO6 ADC) ---
      int raw = 0;
      adc_oneshot_read(adc_handle_, ADC_CHANNEL_5, &raw);
      bool pressed = (raw < POWER_ADC_THRESHOLD);  // <~1V = pressed
      int hold_ms = 0;

      switch (s_btn_state) {
        case BTN_IDLE:
          if (pressed) {
            s_btn_state = BTN_DEBOUNCE_PRESS;
            s_btn_state_since = now;
          } else if (!s_btn_armed) {
            // First confirmed release after boot arms long-press shutdown.
            s_btn_armed = true;
            s_btn_boot_hold_logged = false;
            ESP_LOGI(TAG, "✅ Power button released — long-press shutdown armed");
          }
          break;

        case BTN_DEBOUNCE_PRESS:
          if (!pressed) {
            s_btn_state = BTN_IDLE;  // glitch, reset
          } else if (now - s_btn_state_since >= POWER_DEBOUNCE_MS) {
            if (!s_btn_armed) {
              // Held continuously since boot — ignore until released.
              if (!s_btn_boot_hold_logged) {
                s_btn_boot_hold_logged = true;
                ESP_LOGI(TAG, "🫣 Boot-time hold ignored (shutdown arms after release)");
              }
              s_btn_state = BTN_IDLE;
              break;
            }
            s_btn_state = BTN_PRESSED;
            s_btn_press_start = now;
            s_btn_hold_tip = 0;
            ESP_LOGI(TAG, "🔘 Power button pressed (raw=%d)", raw);
          }
          break;

        case BTN_PRESSED:
          if (!pressed) {
            s_btn_state = BTN_DEBOUNCE_RELEASE;
            s_btn_state_since = now;
          } else {
            hold_ms = static_cast<int>(now - s_btn_press_start);
            if (hold_ms >= POWER_LONG_PRESS_MS)
              ShutdownSequence();  // long press reached → shutdown
            int tip = hold_ms / 500;
            if (tip != s_btn_hold_tip && tip > 0) {
              s_btn_hold_tip = tip;
              ESP_LOGI(TAG, "⏳ Holding %d.%ds (need %ds, release to cancel)",
                       hold_ms / 1000, (hold_ms % 1000) / 100, POWER_LONG_PRESS_MS / 1000);
            }
          }
          break;

        case BTN_DEBOUNCE_RELEASE:
          if (pressed) {
            s_btn_state = BTN_PRESSED;  // not fully released
          } else if (now - s_btn_state_since >= POWER_DEBOUNCE_MS) {
            int dur = s_btn_press_start ? static_cast<int>(now - s_btn_press_start) : 0;
            ESP_LOGI(TAG, "🔘 Power button released (held %dms)", dur);
            if (dur < POWER_LONG_PRESS_MS) {
              /* 短按 → 双击(400ms内)切换 AI 对话 */
              if (now - s_last_click_ms <= 400) s_click_count++;
              else s_click_count = 1;
              s_last_click_ms = now;
              if (s_click_count >= 2) {
                s_click_count = 0;
                ESP_LOGI(TAG, "DOUBLE CLICK -> toggle chat");
                ChatToggle();
              }
            }
            s_btn_state = BTN_IDLE;
            s_btn_press_start = 0;
          }
          break;
      }

      // --- Battery (IO3, 32× oversample + 1st-order filter, every 5s) ---
      bat_elapsed += POWER_POLL_MS;
      if (bat_elapsed >= BATTERY_READ_TICKS * POWER_POLL_MS) {
        bat_elapsed = 0;
        int64_t bat_sum = 0;
        for (int n = 0; n < 32; ++n) {
          int bat_raw = 0;
          adc_oneshot_read(adc_handle_, BATTERY_ADC_CHANNEL, &bat_raw);
          bat_sum += bat_raw;
        }
        int bat_avg = static_cast<int>(bat_sum / 32);
        int vpin_mv = AdcToMv(bat_avg);
        int vbat_mv = static_cast<int>(vpin_mv * BATTERY_DIVIDER_RATIO);
        if (battery_vbat_filtered_mv_ == 0) {
          battery_vbat_filtered_mv_ = vbat_mv;
        } else {
          battery_vbat_filtered_mv_ += (vbat_mv - battery_vbat_filtered_mv_) / 5;
        }
        int vbat_f = battery_vbat_filtered_mv_;
        battery_level_ = (vbat_f - BATTERY_EMPTY_VOLTAGE_MV) * 100 /
                         (BATTERY_FULL_VOLTAGE_MV - BATTERY_EMPTY_VOLTAGE_MV);
        if (battery_level_ < 0) battery_level_ = 0;
        if (battery_level_ > 100) battery_level_ = 100;
        ESP_LOGI(TAG, "[BAT] %dmV (filt=%dmV) level=%d%%",
                 vbat_mv, vbat_f, battery_level_);
      }

      vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    } }, "dino_power", 4096, nullptr, 1, nullptr);
  ESP_LOGI(TAG, "Power monitor started (long press %dms, debounce %dms)",
           POWER_LONG_PRESS_MS, POWER_DEBOUNCE_MS);
}

int GetBatteryLevel() { return battery_level_; }

int GetBatteryVoltageMv() { return battery_vbat_filtered_mv_; }
