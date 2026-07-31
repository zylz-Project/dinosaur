/**
 * @file    w25q256.c
 * @brief   W25Q256JVEIQ SPI NOR Flash 驱动实现 (32MB, 256Mbit)
 *
 * 使用 ESP-IDF SPI Master 驱动。
 *
 * 引脚:
 *   IO9  → CLK
 *   IO47 → MOSI (DI)
 *   IO21 ← MISO (DO)
 *   IO10 → CS
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "w25q256.h"

static const char *TAG = "w25q256";

/* --------------------------------------------------------------------------
   SPI 配置
   -------------------------------------------------------------------------- */

#define W25Q_SPI_HOST      SPI2_HOST   // ESP32-S3 FSPI
#define W25Q_SPI_CLK_HZ    40000000    // 40 MHz (W25Q256 最大 133 MHz)
#define W25Q_CS_PIN        GPIO_NUM_10
#define W25Q_CLK_PIN       GPIO_NUM_9
#define W25Q_MOSI_PIN      GPIO_NUM_47
#define W25Q_MISO_PIN      GPIO_NUM_21

/* --------------------------------------------------------------------------
   全局句柄
   -------------------------------------------------------------------------- */

static spi_device_handle_t g_spi_dev = NULL;
static bool g_initialized = false;
static uint32_t g_capacity_mbit = 0;   // 从 JEDEC ID 查表得到

/* --------------------------------------------------------------------------
   Winbond 容量查表 (JEDEC 第3字节 → Mbit)
   -------------------------------------------------------------------------- */
typedef struct { uint8_t id; uint32_t mbit; } cap_entry_t;

static const cap_entry_t cap_table[] = {
    {0x14,   8},   // W25Q80
    {0x15,  16},   // W25Q16
    {0x16,  32},   // W25Q32
    {0x17,  64},   // W25Q64
    {0x18, 128},   // W25Q128
    {0x19, 256},   // W25Q256
    {0x20, 512},   // W25Q256JV
    {0x21, 1024},  // W25Q01 (1Gbit)
};

static uint32_t w25q_lookup_capacity(uint8_t cap_id)
{
    for (size_t i = 0; i < sizeof(cap_table) / sizeof(cap_table[0]); i++) {
        if (cap_table[i].id == cap_id) return cap_table[i].mbit;
    }
    return 0;  // 未知
}

/* --------------------------------------------------------------------------
   内部辅助函数
   -------------------------------------------------------------------------- */

/**
 * @brief 拉低 CS 选中 Flash。
 */
static inline void w25q_cs_low(void)
{
    gpio_set_level(W25Q_CS_PIN, 0);
}

/**
 * @brief 拉高 CS 释放 Flash。
 */
static inline void w25q_cs_high(void)
{
    gpio_set_level(W25Q_CS_PIN, 1);
}

/**
 * @brief 发送单字节命令 (仅发送，不接收)。
 */
static esp_err_t w25q_send_cmd(uint8_t cmd)
{
    uint8_t tx[1] = {cmd};
    spi_transaction_t t = {
        .flags     = 0,
        .length    = 8,
        .rxlength  = 0,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(g_spi_dev, &t);
}

/**
 * @brief 发送命令 + 24-bit 地址（READ / PROGRAM / ERASE），仅发送不接收。
 *        tx_buffer[0]=cmd, [1]=addr[23:16], [2]=addr[15:8], [3]=addr[7:0]
 */
static esp_err_t w25q_send_cmd_addr(uint8_t cmd, uint32_t addr)
{
    uint8_t tx[4] = {
        cmd,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)(addr & 0xFF),
    };
    spi_transaction_t t = {
        .flags     = 0,
        .length    = 32,
        .rxlength  = 0,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(g_spi_dev, &t);
}

/**
 * @brief 写使能。
 */
static esp_err_t w25q_write_enable(void)
{
    return w25q_send_cmd(W25Q_CMD_WRITE_ENABLE);
}

/**
 * @brief 读取状态寄存器 1。
 *        发 2 字节: {cmd, 0x00}, 收 2 字节: {dummy, SR1}
 *        SR1 在 rx[1]。
 */
static esp_err_t w25q_read_sr1(uint8_t *sr1)
{
    uint8_t tx[2] = {W25Q_CMD_READ_SR1, 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .flags     = 0,
        .length    = 16,
        .rxlength  = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t ret = spi_device_polling_transmit(g_spi_dev, &t);
    if (ret == ESP_OK) {
        *sr1 = rx[1];   // 第二个收到的字节
    }
    return ret;
}

/* ==========================================================================
   Public API
   ========================================================================== */

esp_err_t w25q256_init(void)
{
    if (g_initialized) return ESP_OK;

    ESP_LOGI(TAG, "初始化 SPI Flash (Winbond W25Q 系列)...");
    ESP_LOGI(TAG, "  CLK  → IO%d", W25Q_CLK_PIN);
    ESP_LOGI(TAG, "  MOSI → IO%d (DI)", W25Q_MOSI_PIN);
    ESP_LOGI(TAG, "  MISO ← IO%d (DO)", W25Q_MISO_PIN);
    ESP_LOGI(TAG, "  CS   ← IO%d", W25Q_CS_PIN);

    // --- CS 引脚配置 (手动控制) ---
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << W25Q_CS_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_cfg));
    w25q_cs_high();  // 初始释放 CS

    // --- SPI 总线初始化 ---
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = W25Q_MOSI_PIN,
        .miso_io_num     = W25Q_MISO_PIN,
        .sclk_io_num     = W25Q_CLK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = W25Q256_PAGE_SIZE + 8,  // page + cmd/addr overhead
        .flags           = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id      = ESP_INTR_CPU_AFFINITY_AUTO,
    };
    esp_err_t ret = spi_bus_initialize(W25Q_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // --- 添加 SPI 设备 ---
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.mode           = 0;                         // SPI 模式 0 (CPOL=0, CPHA=0)
    dev_cfg.clock_speed_hz = W25Q_SPI_CLK_HZ;
    dev_cfg.spics_io_num   = -1;                        // 手动 CS
    dev_cfg.flags          = 0;                         // 全双工, 4 线 SPI
    dev_cfg.queue_size     = 7;
    dev_cfg.pre_cb         = NULL;
    dev_cfg.post_cb        = NULL;
    ret = spi_bus_add_device(W25Q_SPI_HOST, &dev_cfg, &g_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 设备添加失败: %s", esp_err_to_name(ret));
        spi_bus_free(W25Q_SPI_HOST);
        return ret;
    }

    // --- 读取 JEDEC ID 验证 ---
    uint8_t mid, mtype, cap;
    ret = w25q256_read_jedec_id(&mid, &mtype, &cap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JEDEC ID 读取失败, 可能 Flash 未连接");
        spi_bus_remove_device(g_spi_dev);
        spi_bus_free(W25Q_SPI_HOST);
        g_spi_dev = NULL;
        return ESP_FAIL;
    }

    g_capacity_mbit = w25q_lookup_capacity(cap);
    ESP_LOGI(TAG, "JEDEC ID: MF=0x%02X  Type=0x%02X  Cap=0x%02X → %lu Mbit (%lu MB)",
             mid, mtype, cap,
             (unsigned long)g_capacity_mbit,
             (unsigned long)(g_capacity_mbit / 8));

    if (mid != 0xEF) {
        ESP_LOGW(TAG, "非 Winbond 厂商 (0xEF), 但继续尝试使用");
    }

    // 等待 Flash 空闲
    if (!w25q256_wait_busy(1000)) {
        ESP_LOGW(TAG, "Flash 上电后未就绪, 继续尝试");
    }

    g_initialized = true;
    ESP_LOGI(TAG, "✅ W25Q256 初始化成功");
    return ESP_OK;
}

void w25q256_deinit(void)
{
    if (!g_initialized) return;
    w25q_cs_high();
    if (g_spi_dev) {
        spi_bus_remove_device(g_spi_dev);
        g_spi_dev = NULL;
    }
    spi_bus_free(W25Q_SPI_HOST);
    g_initialized = false;
    ESP_LOGI(TAG, "W25Q256 已释放");
}

esp_err_t w25q256_read_jedec_id(uint8_t *manufacturer_id,
                                uint8_t *memory_type,
                                uint8_t *capacity)
{
    if (!g_spi_dev) return ESP_FAIL;

    // 发 4 字节: 0x9F + 3 dummy, 收 4 字节: dummy + MF + Type + Cap
    uint8_t tx[4] = {W25Q_CMD_READ_JEDEC_ID, 0x00, 0x00, 0x00};
    uint8_t rx[4] = {0};

    spi_transaction_t t = {
        .flags     = 0,
        .length    = 32,
        .rxlength  = 32,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    w25q_cs_low();
    esp_err_t ret = spi_device_polling_transmit(g_spi_dev, &t);
    w25q_cs_high();

    if (ret == ESP_OK) {
        // rx[0]=dummy(命令发出时收到,丢弃), rx[1]=MF, rx[2]=Type, rx[3]=Cap
        *manufacturer_id = rx[1];
        *memory_type     = rx[2];
        *capacity        = rx[3];
    }
    return ret;
}

esp_err_t w25q256_read_unique_id(uint8_t uid[8])
{
    if (!g_spi_dev) return ESP_FAIL;

    // Send: CMD + 4 dummy bytes, then read 8 bytes UID
    // Total: 8 + 32 + 64 = 104 bits
    // tx_data can hold 4 bytes. We'll use rx_buffer for the rest.

    uint8_t tx[5] = {
        W25Q_CMD_READ_UNIQUE_ID,
        0xFF, 0xFF, 0xFF, 0xFF   // 4 dummy bytes
    };
    uint8_t rx[8] = {0};

    spi_transaction_t t = {
        .flags     = 0,
        .length    = 8 + 32 + 64,  // cmd (8) + 4 dummy (32) + 8-byte UID (64)
        .rxlength  = 64,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    w25q_cs_low();
    esp_err_t ret = spi_device_polling_transmit(g_spi_dev, &t);
    w25q_cs_high();

    if (ret == ESP_OK) {
        memcpy(uid, rx, 8);
    }
    return ret;
}

bool w25q256_wait_busy(uint32_t timeout_ms)
{
    if (!g_spi_dev) return false;

    uint32_t elapsed = 0;
    uint8_t sr1;
    while (elapsed < timeout_ms) {
        w25q_cs_low();
        esp_err_t ret = w25q_read_sr1(&sr1);
        w25q_cs_high();

        if (ret == ESP_OK && !(sr1 & W25Q_SR1_BUSY)) {
            return true;  // 空闲
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }
    ESP_LOGW(TAG, "等待 Flash BUSY 超时 (%lums)", (unsigned long)timeout_ms);
    return false;
}

esp_err_t w25q256_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (!g_spi_dev) return ESP_FAIL;
    if (addr + len > W25Q256_TOTAL_SIZE) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;

    // The address limits are already checked; ESP_ERR_INVALID_ARG is returned for overflow
    // This macro satisfies the unsigned comparison warning
    if (len == 0) return ESP_OK;

    esp_err_t ret = ESP_OK;

    w25q_cs_low();

    // Send READ command + 24-bit address
    ret = w25q_send_cmd_addr(W25Q_CMD_READ_DATA, addr);
    if (ret != ESP_OK) {
        w25q_cs_high();
        return ret;
    }

    // Receive data in chunks matching max_transfer_sz
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > W25Q256_PAGE_SIZE) chunk = W25Q256_PAGE_SIZE;

        spi_transaction_t t = {
            .flags    = 0,
            .length   = chunk * 8,
            .rxlength = chunk * 8,
            .rx_buffer = buf + offset,
        };
        ret = spi_device_polling_transmit(g_spi_dev, &t);
        if (ret != ESP_OK) break;
        offset += chunk;
    }

    w25q_cs_high();
    return ret;
}

esp_err_t w25q256_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (!g_spi_dev) return ESP_FAIL;
    if (addr + len > W25Q256_TOTAL_SIZE) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;

    esp_err_t ret = ESP_OK;
    size_t offset = 0;

    while (offset < len) {
        uint32_t page_start = (addr + offset) & ~(W25Q256_PAGE_SIZE - 1);
        uint32_t page_end   = page_start + W25Q256_PAGE_SIZE;
        size_t chunk = page_end - (addr + offset);
        if (chunk > len - offset) chunk = len - offset;

        // 写使能
        w25q_cs_low();
        ret = w25q_write_enable();
        w25q_cs_high();
        if (ret != ESP_OK) return ret;

        // 发送 PAGE PROGRAM 命令 + 地址 + 数据
        w25q_cs_low();
        {
            // Build command + address
            uint8_t header[4] = {
                W25Q_CMD_PAGE_PROGRAM,
                (uint8_t)(((addr + offset) >> 16) & 0xFF),
                (uint8_t)(((addr + offset) >> 8) & 0xFF),
                (uint8_t)((addr + offset) & 0xFF),
            };

            spi_transaction_t t_hdr = {
                .flags     = 0,
                .length    = 32,               // cmd + 3 addr bytes
                .rxlength  = 0,
                .tx_buffer = header,
            };
            ret = spi_device_polling_transmit(g_spi_dev, &t_hdr);
            if (ret != ESP_OK) {
                w25q_cs_high();
                return ret;
            }

            // Send data chunk
            spi_transaction_t t_data = {
                .flags     = 0,
                .length    = chunk * 8,
                .rxlength  = 0,
                .tx_buffer = buf + offset,
            };
            ret = spi_device_polling_transmit(g_spi_dev, &t_data);
        }
        w25q_cs_high();

        if (ret != ESP_OK) return ret;

        // 等待写完成
        if (!w25q256_wait_busy(100)) {
            ESP_LOGE(TAG, "页写入超时 @ 0x%08lX", (unsigned long)(addr + offset));
            return ESP_ERR_TIMEOUT;
        }

        offset += chunk;
    }

    return ESP_OK;
}

/* ----- 擦除操作 ----- */

static esp_err_t w25q_erase_op(uint8_t cmd, uint32_t addr, const char *op_name)
{
    if (!g_spi_dev) return ESP_FAIL;

    w25q_cs_low();
    esp_err_t ret = w25q_write_enable();
    w25q_cs_high();
    if (ret != ESP_OK) return ret;

    w25q_cs_low();
    ret = w25q_send_cmd_addr(cmd, addr);
    w25q_cs_high();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s 命令发送失败", op_name);
        return ret;
    }

    // 等待完成 — 扇区擦除通常 <400ms, 块擦除 <2s, 全片擦除最多 400s
    uint32_t timeout = (cmd == W25Q_CMD_CHIP_ERASE_1) ? 400000 : 5000;
    if (!w25q256_wait_busy(timeout)) {
        ESP_LOGE(TAG, "%s 超时", op_name);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t w25q256_erase_sector(uint32_t addr)
{
    return w25q_erase_op(W25Q_CMD_SECTOR_ERASE, addr & ~(W25Q256_SECTOR_SIZE - 1),
                         "扇区擦除 4KB");
}

esp_err_t w25q256_erase_block_32k(uint32_t addr)
{
    return w25q_erase_op(W25Q_CMD_BLOCK_ERASE_32K,
                         addr & ~(W25Q256_BLOCK_32K_SIZE - 1), "块擦除 32KB");
}

esp_err_t w25q256_erase_block_64k(uint32_t addr)
{
    return w25q_erase_op(W25Q_CMD_BLOCK_ERASE_64K,
                         addr & ~(W25Q256_BLOCK_64K_SIZE - 1), "块擦除 64KB");
}

esp_err_t w25q256_erase_chip(void)
{
    ESP_LOGW(TAG, "⚠️  全片擦除开始, 预计耗时 200~400 秒...");
    esp_err_t ret = w25q_erase_op(W25Q_CMD_CHIP_ERASE_1, 0, "全片擦除");
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ 全片擦除完成");
    }
    return ret;
}

uint64_t w25q256_get_capacity(void)
{
    if (g_capacity_mbit > 0)
        return (uint64_t)g_capacity_mbit * 1024ULL * 1024ULL / 8ULL;
    return W25Q256_TOTAL_SIZE;  // fallback
}

/* ==========================================================================
   状态寄存器读取 (供调试)
   ========================================================================== */

esp_err_t w25q256_read_status(uint8_t *sr1, uint8_t *sr2, uint8_t *sr3)
{
    if (!g_spi_dev) return ESP_FAIL;

    if (sr1) {
        w25q_cs_low();
        w25q_read_sr1(sr1);
        w25q_cs_high();
    }

    if (sr2) {
        uint8_t tx[2] = {W25Q_CMD_READ_SR2, 0x00};
        uint8_t rx[2] = {0};
        w25q_cs_low();
        spi_transaction_t t2 = {
            .flags = 0, .length = 16, .rxlength = 16,
            .tx_buffer = tx, .rx_buffer = rx,
        };
        esp_err_t ret = spi_device_polling_transmit(g_spi_dev, &t2);
        w25q_cs_high();
        if (ret != ESP_OK) return ret;
        *sr2 = rx[1];
    }

    if (sr3) {
        uint8_t tx[2] = {W25Q_CMD_READ_SR3, 0x00};
        uint8_t rx[2] = {0};
        w25q_cs_low();
        spi_transaction_t t3 = {
            .flags = 0, .length = 16, .rxlength = 16,
            .tx_buffer = tx, .rx_buffer = rx,
        };
        esp_err_t ret = spi_device_polling_transmit(g_spi_dev, &t3);
        w25q_cs_high();
        if (ret != ESP_OK) return ret;
        *sr3 = rx[1];
    }

    return ESP_OK;
}

/* ==========================================================================
   硬件诊断 — 逐项检测
   ========================================================================== */

esp_err_t w25q256_diagnose(void)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "❌ Flash 未初始化, 无法诊断");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   W25Q256 硬件诊断                  ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    int passed = 0, failed = 0;

    /* ==================================================================
       检测①: CS 引脚电平确认
       ================================================================== */
    ESP_LOGI(TAG, "── 检测①: CS 引脚 (IO%d) ──", W25Q_CS_PIN);
    {
        int cs_level = gpio_get_level(W25Q_CS_PIN);
        if (cs_level == 1) {
            ESP_LOGI(TAG, "  ✅ CS 默认高电平 (1), 空闲状态正常");
            passed++;
        } else {
            ESP_LOGE(TAG, "  ❌ CS 电平 = %d (期望 1), 检查上拉/引脚连接", cs_level);
            failed++;
        }
    }

    /* ==================================================================
       检测②: JEDEC ID — 芯片是否存在、身份是否正确
       ================================================================== */
    ESP_LOGI(TAG, "── 检测②: JEDEC ID (0x9F 命令) ──");
    {
        uint8_t mid = 0, mtype = 0, cap = 0;
        esp_err_t ret = w25q256_read_jedec_id(&mid, &mtype, &cap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ JEDEC ID 读取失败: SPI 通信异常");
            ESP_LOGE(TAG, "     可能原因:");
            ESP_LOGE(TAG, "     - MOSI(IO%d)/MISO(IO%d)/CLK(IO%d) 引脚连接错误", W25Q_MOSI_PIN, W25Q_MISO_PIN, W25Q_CLK_PIN);
            ESP_LOGE(TAG, "     - 供电不足 (Flash 需 2.7~3.6V)");
            ESP_LOGE(TAG, "     - 焊接虚焊/短路");
            ESP_LOGE(TAG, "     - CS 未正确拉低");
            failed++;
        } else {
            uint32_t cap_mbit = w25q_lookup_capacity(cap);
            ESP_LOGI(TAG, "  ✅ JEDEC ID: MF=0x%02X, Type=0x%02X, Cap=0x%02X → %lu Mbit (%lu MB)",
                     mid, mtype, cap,
                     (unsigned long)cap_mbit, (unsigned long)(cap_mbit / 8));

            // 检查制造商和型号
            if (mid == 0xEF) {
                ESP_LOGI(TAG, "     ✓ 制造商: Winbond ✓");
            } else if (mid == 0x00 || mid == 0xFF) {
                ESP_LOGW(TAG, "     ⚠️ 制造商ID异常 (0x%02X): 可能全0/全1 → MISO 短路或断开", mid);
                failed++;
            } else {
                ESP_LOGW(TAG, "     ⚠️ 非 Winbond 制造商, 但器件有响应");
            }

            if (mtype == 0x60 && cap == 0x20) {
                ESP_LOGI(TAG, "     ✓ 型号确认: W25Q256JV ✓");
                passed++;
            } else {
                ESP_LOGW(TAG, "     ⚠️ 型号不匹配 (期望 0x60/0x20), 但仍可继续");
                passed++;
            }
        }
    }

    /* ==================================================================
       检测③: 状态寄存器 — 空闲? 有写保护?
       ================================================================== */
    ESP_LOGI(TAG, "── 检测③: 状态寄存器 ──");
    {
        uint8_t sr1 = 0, sr2 = 0, sr3 = 0;
        esp_err_t ret = w25q256_read_status(&sr1, &sr2, &sr3);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ 状态寄存器读失败");
            failed++;
        } else {
            ESP_LOGI(TAG, "  ✅ SR1=0x%02X  SR2=0x%02X  SR3=0x%02X", sr1, sr2, sr3);

            // 逐位解码 SR1
            if (sr1 & W25Q_SR1_BUSY) {
                ESP_LOGW(TAG, "     ⚠️ BUSY=1: Flash 正在忙于内部操作");
            } else {
                ESP_LOGI(TAG, "     ✓ BUSY=0: 空闲就绪");
            }
            ESP_LOGI(TAG, "     WEL=%d (写使能锁存)", (sr1 >> 1) & 1);

            uint8_t bp = (sr1 >> 2) & 0x07;  // BP0, BP1, BP2
            if (bp != 0) {
                ESP_LOGW(TAG, "     ⚠️ BP[2:0]=%d: 有区块写保护! 部分区域无法写入", bp);
            } else {
                ESP_LOGI(TAG, "     ✓ BP[2:0]=0: 无区块保护");
            }
            ESP_LOGI(TAG, "     TB=%d (保护区域方向), SRP0=%d", (sr1 >> 5) & 1, (sr1 >> 7) & 1);

            passed++;
        }
    }

    /* ==================================================================
       检测④: 写使能锁存 (WEL) — 能否置位/清零
       ================================================================== */
    ESP_LOGI(TAG, "── 检测④: 写使能锁存 WEL 功能 ──");
    {
        uint8_t sr1 = 0;

        // 4a: 发送 Write Enable，检查 WEL=1
        w25q_cs_low();
        esp_err_t ret = w25q_write_enable();
        w25q_cs_high();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ Write Enable 命令发送失败");
            failed++;
        } else {
            w25q256_read_status(&sr1, NULL, NULL);
            if (sr1 & W25Q_SR1_WEL) {
                ESP_LOGI(TAG, "  ✅ Write Enable → WEL=1 (锁存置位正常)");
                passed++;
            } else {
                ESP_LOGE(TAG, "  ❌ Write Enable 后 WEL=0: 写使能锁存失效, 芯片可能损坏或 WP# 被拉低");
                failed++;
            }
        }

        // 4b: 发送 Write Disable, 检查 WEL=0
        w25q_cs_low();
        ret = w25q_send_cmd(W25Q_CMD_WRITE_DISABLE);
        w25q_cs_high();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ Write Disable 命令发送失败");
            failed++;
        } else {
            w25q256_read_status(&sr1, NULL, NULL);
            if (!(sr1 & W25Q_SR1_WEL)) {
                ESP_LOGI(TAG, "  ✅ Write Disable → WEL=0 (锁存清除正常)");
                passed++;
            } else {
                ESP_LOGE(TAG, "  ❌ Write Disable 后 WEL 仍=1");
                failed++;
            }
        }
    }

    /* ==================================================================
       检测⑤: 单页擦除 → 写入 → 回读 (数据完整性)
       ================================================================== */
    ESP_LOGI(TAG, "── 检测⑤: 数据完整性 (擦除→写→读回) @ 地址 0x000000 ──");
    {
        // 先用不同模式检查 0x000000 是否全为 0xFF (出厂擦除态)
        // 如果不是，可能是之前测试写入的，先擦除

        // 5a: 擦除扇区 0
        esp_err_t ret = w25q256_erase_sector(0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ 扇区 0 擦除失败: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "     可能原因: 区块写保护(检查BP位) / WP#引脚拉低 / HOLD#引脚拉低");
            failed++;
        } else {
            ESP_LOGI(TAG, "  ✅ 扇区 0 擦除成功 (4KB)");

            // 5b: 验证擦除 (全 FF)
            uint8_t erase_check[16];
            ret = w25q256_read(0, erase_check, sizeof(erase_check));
            bool all_ff = true;
            for (int i = 0; i < (int)sizeof(erase_check); i++) {
                if (erase_check[i] != 0xFF) { all_ff = false; break; }
            }
            if (ret == ESP_OK && all_ff) {
                ESP_LOGI(TAG, "  ✅ 擦除验证通过 (全 0xFF)");
            } else {
                ESP_LOGW(TAG, "  ⚠️ 擦除后仍有非 0xFF, 扇区可能已磨损");
            }

            // 5c: 写入一页测试数据 (递增 + 地址混合模式)
            uint8_t wr_buf[W25Q256_PAGE_SIZE];
            uint8_t rd_buf[W25Q256_PAGE_SIZE];
            for (int i = 0; i < W25Q256_PAGE_SIZE; i++) {
                wr_buf[i] = (uint8_t)((i & 0xFF) ^ ((i >> 2) & 0x55) ^ 0xA5);
            }

            ret = w25q256_write(0, wr_buf, W25Q256_PAGE_SIZE);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "  ❌ 页写入失败: %s", esp_err_to_name(ret));
                failed++;
            } else {
                // 5d: 读回比对
                memset(rd_buf, 0x00, W25Q256_PAGE_SIZE);
                ret = w25q256_read(0, rd_buf, W25Q256_PAGE_SIZE);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "  ❌ 页读取失败: %s", esp_err_to_name(ret));
                    failed++;
                } else {
                    int mismatches = 0;
                    for (int i = 0; i < W25Q256_PAGE_SIZE; i++) {
                        if (wr_buf[i] != rd_buf[i]) {
                            if (mismatches < 5) {
                                ESP_LOGE(TAG, "  ❌ 字节 %d: 写 0x%02X ≠ 读 0x%02X", i, wr_buf[i], rd_buf[i]);
                            }
                            mismatches++;
                        }
                    }
                    if (mismatches == 0) {
                        ESP_LOGI(TAG, "  ✅ 单页 (256B) 读写完全一致!");
                        passed++;
                    } else {
                        ESP_LOGE(TAG, "  ❌ 总共 %d 字节不一致", mismatches);
                        failed++;
                    }
                }
            }
        }
    }

    /* ==================================================================
       检测⑥: 跨页边界写入 — 验证驱动自动跨页逻辑
       ================================================================== */
    ESP_LOGI(TAG, "── 检测⑥: 跨页边界写入 (地址 0x00F0, 32 字节, 跨越 0x00FF→0x0100) ──");
    {
        // 先擦除扇区 0 的跨页区域
        // 扇区0已在步骤⑤擦除, 但被写入了数据, 这里重新擦除
        esp_err_t ret = w25q256_erase_sector(0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  ❌ 擦除失败, 跳过跨页测试");
            failed++;
        } else {
            uint8_t cross_buf[32];
            // 生成可识别模式: 前半页数据 = 0xAB开头, 后半页 = 0xCD
            for (int i = 0; i < 32; i++) {
                cross_buf[i] = (uint8_t)(0x80 + i);  // 0x80, 0x81, ...
            }

            // 从页内偏移 0xF0 开始写入 32 字节 → 前 16 字节在页 0, 后 16 字节在页 1
            ret = w25q256_write(0xF0, cross_buf, 32);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "  ❌ 跨页写入失败: %s", esp_err_to_name(ret));
                failed++;
            } else {
                uint8_t readback[32];
                ret = w25q256_read(0xF0, readback, 32);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "  ❌ 跨页读取失败");
                    failed++;
                } else {
                    int mismatches = 0;
                    for (int i = 0; i < 32; i++) {
                        if (cross_buf[i] != readback[i]) {
                            if (mismatches < 5) {
                                ESP_LOGE(TAG, "  ❌ 跨页偏移 %d: 写 0x%02X ≠ 读 0x%02X", i, cross_buf[i], readback[i]);
                            }
                            mismatches++;
                        }
                    }
                    if (mismatches == 0) {
                        ESP_LOGI(TAG, "  ✅ 跨页边界写入完全正确!");
                        passed++;
                    } else {
                        ESP_LOGE(TAG, "  ❌ %d 字节不一致 (跨页逻辑可能有 bug)", mismatches);
                        failed++;
                    }
                }
            }
        }
    }

    /* ==================================================================
       总结
       ================================================================== */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    if (failed == 0) {
        ESP_LOGI(TAG, "║  ✅ 诊断全部通过 (%d/%d 项)        ║", passed, passed);
    } else {
        ESP_LOGI(TAG, "║  ⚠️ 诊断: %d 通过, %d 失败         ║", passed, failed);
    }
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    return (failed == 0) ? ESP_OK : ESP_FAIL;
}

/* ==========================================================================
   快速功能测试 (1 个扇区)
   ========================================================================== */

esp_err_t w25q256_quick_test(void)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Flash 未初始化, 无法测试");
        return ESP_FAIL;
    }

    // 写两个扇区: 扇区0 = 递增计数, 扇区1 = 0xAA 填充, 对比明显
    uint32_t s0_addr = 0x000000;
    uint32_t s1_addr = 0x001000;
    int page_count   = W25Q256_SECTOR_SIZE / W25Q256_PAGE_SIZE;
    esp_err_t ret;

    ESP_LOGI(TAG, "========== SPI Flash 快速测试 ==========");

    // --- 1. 擦除两个扇区 ---
    ESP_LOGI(TAG, "[1/4] 擦除扇区 0 (0x%06lX) 和扇区 1 (0x%06lX)...",
             (unsigned long)s0_addr, (unsigned long)s1_addr);
    ret = w25q256_erase_sector(s0_addr);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区0擦除失败!"); return ESP_FAIL; }
    ret = w25q256_erase_sector(s1_addr);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区1擦除失败!"); return ESP_FAIL; }
    ESP_LOGI(TAG, "  ✅ 擦除成功");

    // --- 2. 写入扇区0: 递增计数 (地址的低字节) ---
    ESP_LOGI(TAG, "[2/4] 扇区0: 写入递增模式...");
    uint8_t page_buf[W25Q256_PAGE_SIZE];
    for (int p = 0; p < page_count; p++) {
        for (int i = 0; i < W25Q256_PAGE_SIZE; i++) {
            page_buf[i] = (uint8_t)((s0_addr + p * W25Q256_PAGE_SIZE + i) & 0xFF);
        }
        ret = w25q256_write(s0_addr + p * W25Q256_PAGE_SIZE, page_buf, W25Q256_PAGE_SIZE);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区0 页%d 写入失败!", p); return ESP_FAIL; }
    }
    ESP_LOGI(TAG, "  ✅ 扇区0 写入完成 (0x00,0x01,0x02...)");

    // --- 3. 写入扇区1: 全 0xAA ---
    ESP_LOGI(TAG, "[3/4] 扇区1: 写入 0xAA...");
    memset(page_buf, 0xAA, W25Q256_PAGE_SIZE);
    for (int p = 0; p < page_count; p++) {
        ret = w25q256_write(s1_addr + p * W25Q256_PAGE_SIZE, page_buf, W25Q256_PAGE_SIZE);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区1 页%d 写入失败!", p); return ESP_FAIL; }
    }
    ESP_LOGI(TAG, "  ✅ 扇区1 写入完成 (全 0xAA)");

    // --- 4. 读回比对 ---
    ESP_LOGI(TAG, "[4/4] 读回比对...");
    uint8_t rd_buf[W25Q256_PAGE_SIZE];
    int mismatches = 0;

    // 检查扇区0
    for (int p = 0; p < page_count; p++) {
        memset(rd_buf, 0x00, W25Q256_PAGE_SIZE);
        ret = w25q256_read(s0_addr + p * W25Q256_PAGE_SIZE, rd_buf, W25Q256_PAGE_SIZE);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区0 页%d 读取失败!", p); return ESP_FAIL; }
        for (int i = 0; i < W25Q256_PAGE_SIZE; i++) {
            uint8_t exp = (uint8_t)((s0_addr + p * W25Q256_PAGE_SIZE + i) & 0xFF);
            if (rd_buf[i] != exp && mismatches < 5) {
                ESP_LOGE(TAG, "  扇区0 偏移0x%04X: 期望0x%02X 读到0x%02X",
                         p * W25Q256_PAGE_SIZE + i, exp, rd_buf[i]);
            }
            if (rd_buf[i] != exp) mismatches++;
        }
    }
    // 检查扇区1
    for (int p = 0; p < page_count; p++) {
        memset(rd_buf, 0x00, W25Q256_PAGE_SIZE);
        ret = w25q256_read(s1_addr + p * W25Q256_PAGE_SIZE, rd_buf, W25Q256_PAGE_SIZE);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "❌ 扇区1 页%d 读取失败!", p); return ESP_FAIL; }
        for (int i = 0; i < W25Q256_PAGE_SIZE; i++) {
            if (rd_buf[i] != 0xAA && mismatches < 5) {
                ESP_LOGE(TAG, "  扇区1 偏移0x%04X: 期望0xAA 读到0x%02X",
                         p * W25Q256_PAGE_SIZE + i, rd_buf[i]);
            }
            if (rd_buf[i] != 0xAA) mismatches++;
        }
    }

    if (mismatches == 0) {
        ESP_LOGI(TAG, "  ✅ 全部 %d 字节一致!", W25Q256_SECTOR_SIZE * 2);
        ESP_LOGI(TAG, "========== ✅ 测试通过 ==========");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "  ❌ %d 字节不一致", mismatches);
        ESP_LOGE(TAG, "========== ❌ 测试失败 ==========");
        return ESP_FAIL;
    }
}

/* ==========================================================================
   全片自检
   ========================================================================== */

/**
 * @brief 生成伪随机测试模式（基于地址）。
 */
static uint8_t test_pattern(uint32_t addr)
{
    return (uint8_t)((addr * 1103515245 + 12345) & 0xFF);
}

int w25q256_full_test(int test_sectors)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Flash 未初始化, 无法测试");
        return -1;
    }

    int max_sectors = (int)W25Q256_NUM_SECTORS;
    if (test_sectors <= 0 || test_sectors > max_sectors) {
        test_sectors = max_sectors;
    }

    ESP_LOGI(TAG, "========== W25Q256 全片自检 ==========");
    ESP_LOGI(TAG, "测试扇区数: %d / %d", test_sectors, max_sectors);
    ESP_LOGI(TAG, "每扇区 4KB, 总测试 %d KB", test_sectors * 4);

    uint8_t *write_buf = (uint8_t *)heap_caps_malloc(W25Q256_SECTOR_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *read_buf  = (uint8_t *)heap_caps_malloc(W25Q256_SECTOR_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!write_buf || !read_buf) {
        ESP_LOGE(TAG, "无法分配测试缓冲");
        free(write_buf);
        free(read_buf);
        return -1;
    }

    int failures = 0;
    int tested   = 0;

    for (int sec = 0; sec < test_sectors; sec++) {
        uint32_t addr = (uint32_t)sec * W25Q256_SECTOR_SIZE;

        // 擦除扇区
        esp_err_t ret = w25q256_erase_sector(addr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "扇区 %d 擦除失败", sec);
            failures++;
            continue;
        }

        // 填写测试模式
        for (int i = 0; i < W25Q256_SECTOR_SIZE; i++) {
            write_buf[i] = test_pattern(addr + i);
        }

        // 写入
        ret = w25q256_write(addr, write_buf, W25Q256_SECTOR_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "扇区 %d 写入失败", sec);
            failures++;
            continue;
        }

        // 读取回
        memset(read_buf, 0xAA, W25Q256_SECTOR_SIZE);
        ret = w25q256_read(addr, read_buf, W25Q256_SECTOR_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "扇区 %d 读取失败", sec);
            failures++;
            continue;
        }

        // 比对
        int errors = 0;
        for (int i = 0; i < W25Q256_SECTOR_SIZE; i++) {
            if (write_buf[i] != read_buf[i]) {
                if (errors < 5) {
                    ESP_LOGE(TAG, "扇区 %d 偏移 %d: 写 0x%02X 读 0x%02X",
                             sec, i, write_buf[i], read_buf[i]);
                }
                errors++;
            }
        }

        if (errors > 0) {
            ESP_LOGE(TAG, "扇区 %d 有 %d 个字节错误!", sec, errors);
            failures++;
        }

        tested++;

        // 每 256 扇区 (1MB) 打印进度
        if (tested % 256 == 0) {
            ESP_LOGI(TAG, "测试进度: %d/%d 扇区 (%.1f%%), 已发现 %d 个错误扇区",
                     tested, test_sectors, (float)tested * 100 / test_sectors, failures);
        }
    }

    ESP_LOGI(TAG, "========== 测试完成 ==========");
    ESP_LOGI(TAG, "测试扇区: %d, 失败数: %d (%.1f%%)",
             tested, failures, tested > 0 ? (float)failures * 100 / tested : 0);
    if (failures == 0) {
        ESP_LOGI(TAG, "✅ 全片通过!");
    }

    free(write_buf);
    free(read_buf);
    return failures;
}
