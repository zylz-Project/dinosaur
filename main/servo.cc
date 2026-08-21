#include "servo.h"
#include "config.h"

#include <algorithm>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "dino_servo";

static constexpr gpio_num_t kServoPins[] = {
    SERVO_NECK_TILT_GPIO, SERVO_NECK_LEAN_GPIO, SERVO_HEAD_TURN_GPIO,
    SERVO_TAIL_UD_GPIO, SERVO_TAIL_LR_GPIO};
static constexpr ledc_channel_t kServoCh[] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2,
    LEDC_CHANNEL_3, LEDC_CHANNEL_4};

const int kServoCount = sizeof(kServoPins) / sizeof(kServoPins[0]);

static int servo_angles_[5] = {
    SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
    SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};

static uint32_t AngleToDuty(int angle)
{
  angle = std::clamp(angle, 0, SERVO_MAX_ANGLE);
  int32_t pulse_us = 500 + angle * 2000 / SERVO_MAX_ANGLE;
  if (pulse_us < 500) pulse_us = 500;
  if (pulse_us > 2500) pulse_us = 2500;
  return pulse_us * (SERVO_MAX_DUTY + 1) / SERVO_PERIOD_US;
}

void SetServoAngle(int idx, int angle)
{
  if (idx < 0 || idx >= kServoCount) return;
  angle = std::clamp(angle, 0, SERVO_MAX_ANGLE);
  if (idx == SERVO_TAIL_UD)
    angle = std::clamp(angle, SERVO_TAIL_UD_MIN, SERVO_TAIL_UD_MAX);
  servo_angles_[idx] = angle;
  uint32_t duty = AngleToDuty(angle);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kServoCh[idx], duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kServoCh[idx]);
}

void InitServos()
{
  // --- IO4 pull HIGH to power servos (same as power latch IO7) ---
  gpio_config_t pwr_cfg = {
      .pin_bit_mask = 1ULL << SERVO_POWER_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_cfg);
  gpio_set_level(SERVO_POWER_GPIO, 1);  // IO4 HIGH → servos powered

  // Pull all servo GPIOs LOW first to prevent floating glitches
  for (int i = 0; i < kServoCount; i++) {
      gpio_config_t io_cfg = {
          .pin_bit_mask = 1ULL << kServoPins[i],
          .mode = GPIO_MODE_OUTPUT,
          .pull_up_en = GPIO_PULLUP_DISABLE,
          .pull_down_en = GPIO_PULLDOWN_DISABLE,
          .intr_type = GPIO_INTR_DISABLE,
      };
      gpio_config(&io_cfg);
      gpio_set_level(kServoPins[i], 0);
  }

  // Configure LEDC PWM — servos already powered, PWM takes effect immediately
  ledc_timer_config_t tcfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = SERVO_DUTY_RES,
      .timer_num = SERVO_TIMER,
      .freq_hz = SERVO_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&tcfg);

  for (int i = 0; i < kServoCount; i++)
  {
    ledc_channel_config_t ch = {
        .gpio_num = static_cast<int>(kServoPins[i]),
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = kServoCh[i],
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_TIMER,
        .duty = AngleToDuty(servo_angles_[i]),
        .hpoint = 0,
        .flags = {.output_invert = 0},
    };
    ledc_channel_config(&ch);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kServoCh[i], AngleToDuty(servo_angles_[i]));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kServoCh[i]);
  }

  // Explicitly set all to default after PWM config
  for (int i = 0; i < kServoCount; i++)
    SetServoAngle(i, servo_angles_[i]);
  ESP_LOGI(TAG, "Servos initialized: neck_tilt=%d neck_lean=%d head_turn=%d tail_ud=%d tail_lr=%d",
           servo_angles_[0], servo_angles_[1], servo_angles_[2],
           servo_angles_[3], servo_angles_[4]);
  ESP_LOGI(TAG, "Servo map: head=IO15 neck_lr=IO16 neck_ud=IO17 tail_lr=IO18 tail_ud=IO8");
}
