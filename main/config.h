#pragma once

// === Feature toggles ===
#define OFFLINE_DEMO 1           // 1 = skip WiFi, boot instantly for offline demo. 0 = normal
#define ENABLE_AUTO_RUN 1        // 1 = compile auto-run feature, 0 = disable entirely
#define AUTO_RUN_DEFAULT_ON 1    // 1 = active on power-up, 0 = start paused
#define AUTO_RUN_DEFAULT_HARD 0  // 1 = hard swing (instant to extrema + hold), 0 = sin² smooth
#define HARD_SWING_SPEED_X 4.0f  // Hard-swing period multiplier (>1 = faster, 4x = continuous)

// === Power management ===
#define POWER_CTRL_GPIO GPIO_NUM_7   // Latch HIGH = power on, LOW = power off
#define POWER_OUT_GPIO GPIO_NUM_6    // ADC read, <1V = button pressed
#define POWER_ADC_THRESHOLD 1241     // 1.0V threshold for pressed/released
#define POWER_LONG_PRESS_MS 1500
#define POWER_DEBOUNCE_MS 50
#define POWER_POLL_MS 20

// Battery ADC: IO3 = ADC1_CH2
// Voltage divider: R_upper=2k, R_lower=4.7k
// Vpin = Vbat * 4.7 / (2 + 4.7) → Vbat = Vpin * 1.426
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_DIVIDER_RATIO 2
#define BATTERY_EMPTY_VOLTAGE_MV 3200
#define BATTERY_FULL_VOLTAGE_MV 4200
#define BATTERY_READ_TICKS 250  // Every 5s (250 * 20ms)

// === Servo pins (5x 180° servos) ===
// IO15: neck_tilt  — 0°=tilt back, 90°=center, 180°=tilt forward
// IO16: neck_lean  — 0°=lean right, 90°=center, 180°=lean left
// IO17: head_turn  — 0°=turn right, 90°=center, 180°=turn left
// IO18: tail_ud    — 0°=tail up, 180°=tail down
// IO8:  tail_lr    — 180°=tail leftmost
#define SERVO_POWER_GPIO GPIO_NUM_4
#define SERVO_NECK_TILT_GPIO GPIO_NUM_15   // IO15: neck front/back
#define SERVO_NECK_LEAN_GPIO GPIO_NUM_16   // IO16: neck left/right
#define SERVO_HEAD_TURN_GPIO GPIO_NUM_17   // IO17: head left/right
#define SERVO_TAIL_UD_GPIO   GPIO_NUM_18   // IO18: tail up/down
#define SERVO_TAIL_LR_GPIO   GPIO_NUM_8    // IO8:  tail left/right

// Servo default angles (neutral position)
#define SERVO_NECK_TILT_DEFAULT  90
#define SERVO_NECK_LEAN_DEFAULT  90
#define SERVO_HEAD_TURN_DEFAULT  90
#define SERVO_TAIL_UD_DEFAULT    90
#define SERVO_TAIL_LR_DEFAULT    90

#define SERVO_MAX_ANGLE 180

// === LEDC PWM 50Hz ===
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_FREQ_HZ 50
#define SERVO_DUTY_RES LEDC_TIMER_13_BIT
#define SERVO_MAX_DUTY ((1 << 13) - 1)
#define SERVO_PERIOD_US 20000

// === Audio (ES8311 codec over I2C + I2S) ===
#define AUDIO_I2C_SDA_PIN GPIO_NUM_2
#define AUDIO_I2C_SCL_PIN GPIO_NUM_38
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_NC
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_48
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_46
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_14
#define AUDIO_SAMPLE_RATE 48000  // Opus source is 48kHz
#define AUDIO_OUTPUT_VOLUME 80   // 0-100
#define AUDIO_SILENT_INTERVAL_MIN_S 4
#define AUDIO_SILENT_INTERVAL_MAX_S 6

// === SPI Flash (W25Q256) ===
#define SPI_FLASH_CS_PIN   GPIO_NUM_10
#define SPI_FLASH_CLK_PIN  GPIO_NUM_9
#define SPI_FLASH_MOSI_PIN GPIO_NUM_47
#define SPI_FLASH_MISO_PIN GPIO_NUM_21

// === WiFi Station ===
#define WIFI_STA_SSID     "YOUR_WIFI_SSID"
#define WIFI_STA_PASSWORD "YOUR_WIFI_PASSWORD"
#define WIFI_STA_TIMEOUT_S 15   // Connection timeout (seconds), continue offline after

// === Audio Sync Server ===
#define SYNC_SERVER_IP    "192.168.1.100"  // User computer IP
#define SYNC_SERVER_PORT  5000
#define SYNC_PRODUCT_ID   "dinosaur"       // Product ID for server routing
#define SYNC_DOWNLOAD_BUF_SIZE 4096        // Download buffer size
