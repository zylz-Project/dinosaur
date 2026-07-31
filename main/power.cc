#include "power.h"
#include "config.h"

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

void InitPower()
{
  // --- POWER_CTRL (IO7): latch HIGH immediately ---
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

  // POWER_OUT (IO6): ADC monitoring via ADC1_CH5
  gpio_config_t pwr_out = {
      .pin_bit_mask = 1ULL << POWER_OUT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_out);
  gpio_hold_en(POWER_OUT_GPIO);

  // ADC init
  adc_oneshot_unit_init_cfg_t adc_cfg = {
      .unit_id = ADC_UNIT_1,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&adc_cfg, &adc_handle_) == ESP_OK)
  {
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_5, &ch);      // IO6 power button
    adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &ch); // IO3 battery

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_5,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cali, &adc_cali_handle_);
  }

  // Power monitor task: symmetric hysteresis debounce + long-press shutdown + battery
  xTaskCreate([](void *)
              {
    vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for stabilization

    // Symmetric hysteresis debounce state
    int pwr_stable = 1;       // Debounced state: 1=released, 0=pressed
    int pwr_debounce = 5;     // Debounce counter
    uint32_t pwr_hold_ms = 0; // Pressed duration
    uint32_t tick = 0;

    const int debounce_max = POWER_DEBOUNCE_MS / POWER_POLL_MS;

    while (true) {
        tick++;

        // --- Read power button ADC ---
        int raw = 0;
        adc_oneshot_read(adc_handle_, ADC_CHANNEL_5, &raw);
        int raw_high = (raw > POWER_ADC_THRESHOLD) ? 1 : 0;  // >1241=released

        // Symmetric hysteresis debounce
        if (raw_high == pwr_stable) {
            if (pwr_debounce < debounce_max) pwr_debounce++;
        } else {
            if (pwr_debounce > 0) pwr_debounce--;
            else { pwr_stable = raw_high; pwr_debounce = 1; }
        }

        // Edge detection
        static int prev_stable = 1;
        if (pwr_stable != prev_stable) {
            if (!pwr_stable) {
                ESP_LOGI(TAG, "Power button PRESSED");
                pwr_hold_ms = 0;
            } else {
                ESP_LOGI(TAG, "Power button RELEASED (held %dms)", (int)pwr_hold_ms);
                // Shutdown on release if long-press threshold met
                if (pwr_hold_ms >= POWER_LONG_PRESS_MS) {
                    ESP_LOGW(TAG, "Long press %dms -> SHUTDOWN (IO%d -> LOW)",
                             POWER_LONG_PRESS_MS, POWER_CTRL_GPIO);
                    gpio_set_level(POWER_CTRL_GPIO, 0);
                }
            }
            prev_stable = pwr_stable;
        }

        // Long-press timing
        if (!pwr_stable) {
            pwr_hold_ms += POWER_POLL_MS;
        }

        // --- Battery voltage (IO3) — every 5s ---
        if (tick % BATTERY_READ_TICKS == 0) {
            int64_t bat_sum = 0;
            for (int n = 0; n < 32; n++) {
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
  ESP_LOGI(TAG, "Power monitor started (IO6 ADC, long press %dms, debounce %dms)",
           POWER_LONG_PRESS_MS, POWER_DEBOUNCE_MS);
}

int GetBatteryLevel() { return battery_level_; }

int GetBatteryVoltageMv() { return battery_vbat_filtered_mv_; }
