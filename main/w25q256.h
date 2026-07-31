#pragma once

/**
 * @file    w25q256.h
 * @brief   W25Q256JVEIQ SPI NOR Flash 驱动 (32MB / 256Mbit)
 *
 * 引脚映射 (ESP32-S3 → W25Q256JVEIQ):
 *   IO9  →  CLK
 *   IO47 →  DI  (MOSI)
 *   IO21 ←  DO  (MISO)
 *   IO10 →  /CS
 *
 * 使用标准 4 线 SPI (模式 0/3)。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Flash 物理参数
   ========================================================================== */

#define W25Q256_PAGE_SIZE       256     // 每页 256 字节
#define W25Q256_SECTOR_SIZE     4096    // 每个扇区 4KB
#define W25Q256_BLOCK_32K_SIZE  32768   // 32KB 块
#define W25Q256_BLOCK_64K_SIZE  65536   // 64KB 块
#define W25Q256_TOTAL_SIZE      (32ULL * 1024 * 1024)  // 32MB

#define W25Q256_NUM_SECTORS     (W25Q256_TOTAL_SIZE / W25Q256_SECTOR_SIZE) // 8192
#define W25Q256_NUM_BLOCKS_64K  (W25Q256_TOTAL_SIZE / W25Q256_BLOCK_64K_SIZE) // 512

/* ==========================================================================
   命令表 (Standard SPI)
   ========================================================================== */

#define W25Q_CMD_WRITE_ENABLE       0x06
#define W25Q_CMD_WRITE_DISABLE      0x04
#define W25Q_CMD_READ_SR1           0x05
#define W25Q_CMD_READ_SR2           0x35
#define W25Q_CMD_READ_SR3           0x15
#define W25Q_CMD_WRITE_SR           0x01
#define W25Q_CMD_READ_DATA          0x03
#define W25Q_CMD_FAST_READ          0x0B
#define W25Q_CMD_PAGE_PROGRAM       0x02
#define W25Q_CMD_SECTOR_ERASE       0x20    // 4KB
#define W25Q_CMD_BLOCK_ERASE_32K    0x52
#define W25Q_CMD_BLOCK_ERASE_64K    0xD8
#define W25Q_CMD_CHIP_ERASE_1       0xC7
#define W25Q_CMD_CHIP_ERASE_2       0x60
#define W25Q_CMD_READ_JEDEC_ID      0x9F
#define W25Q_CMD_READ_UNIQUE_ID     0x4B
#define W25Q_CMD_POWER_DOWN         0xB9
#define W25Q_CMD_RELEASE_PD         0xAB
#define W25Q_CMD_READ_SFDP          0x5A

/* ==========================================================================
   状态寄存器位
   ========================================================================== */

#define W25Q_SR1_BUSY    (1 << 0)    // BUSY/WIP
#define W25Q_SR1_WEL     (1 << 1)    // Write Enable Latch
#define W25Q_SR1_BP0     (1 << 2)    // Block Protect 0
#define W25Q_SR1_BP1     (1 << 3)    // Block Protect 1
#define W25Q_SR1_BP2     (1 << 4)    // Block Protect 2
#define W25Q_SR1_TB      (1 << 5)    // Top/Bottom
#define W25Q_SR1_SEC     (1 << 6)    // Sector Protect
#define W25Q_SR1_SRP0    (1 << 7)    // Status Register Protect 0

/* ==========================================================================
   API
   ========================================================================== */

/**
 * @brief 初始化 SPI 总线和 W25Q256 Flash。
 * @return ESP_OK 成功, 否则失败。
 */
esp_err_t w25q256_init(void);

/**
 * @brief 反初始化 SPI 总线。
 */
void w25q256_deinit(void);

/**
 * @brief 读取 JEDEC 制造商/设备 ID。
 * @param manufacturer_id [out] 制造商 ID（Winbond = 0xEF）
 * @param memory_type     [out] 存储类型（W25Q256 = 0x60）
 * @param capacity        [out] 容量码（W25Q256 = 0x19）
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_read_jedec_id(uint8_t *manufacturer_id,
                                uint8_t *memory_type,
                                uint8_t *capacity);

/**
 * @brief 读取 64-bit 唯一 ID。
 * @param uid [out] 8 字节缓冲。
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_read_unique_id(uint8_t uid[8]);

/**
 * @brief 从 Flash 读取数据。
 * @param addr   24-bit 起始地址
 * @param buf    目标缓冲
 * @param len    读取长度
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_read(uint32_t addr, uint8_t *buf, size_t len);

/**
 * @brief 向 Flash 写入数据（自动跨页）。
 *        注意：写入前请先擦除对应区域。
 * @param addr   24-bit 起始地址
 * @param buf    源数据缓冲
 * @param len    写入长度
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_write(uint32_t addr, const uint8_t *buf, size_t len);

/**
 * @brief 擦除 4KB 扇区。
 * @param addr 扇区内任意地址（自动对齐到 4KB 边界）
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_erase_sector(uint32_t addr);

/**
 * @brief 擦除 32KB 块。
 * @param addr 块内任意地址
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_erase_block_32k(uint32_t addr);

/**
 * @brief 擦除 64KB 块。
 * @param addr 块内任意地址
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_erase_block_64k(uint32_t addr);

/**
 * @brief 整片擦除（非常耗时，约 200-400 秒）。
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t w25q256_erase_chip(void);

/**
 * @brief 等待 Flash 空闲。
 * @param timeout_ms 超时(ms)
 * @return true=空闲, false=超时
 */
bool w25q256_wait_busy(uint32_t timeout_ms);

/**
 * @brief 获取总容量（字节）。
 */
uint64_t w25q256_get_capacity(void);

/**
 * @brief 读取状态寄存器 (SR1, SR2, SR3) 用于调试。
 * @param sr1/2/3 [out] 状态寄存器值, 不需要的填 NULL。
 */
esp_err_t w25q256_read_status(uint8_t *sr1, uint8_t *sr2, uint8_t *sr3);

/**
 * @brief 硬件诊断：逐项检测 SPI Flash 各环节是否正常。
 *
 * 检测流程（从底层到上层）：
 *   ① CS 引脚电平确认
 *   ② JEDEC ID 读取（芯片存在性 + 身份验证）
 *   ③ 状态寄存器读取（空闲/写保护/区块保护位）
 *   ④ 写使能锁存测试（WELL 能否置位/清零）
 *   ⑤ 单页擦除 → 写入 → 读回比对（数据完整性，地址 0x000000）
 *   ⑥ 跨页边界写入测试（验证自动跨页逻辑）
 *
 * @return ESP_OK 全部通过, ESP_FAIL 某项失败（查看串口日志定位）。
 */
esp_err_t w25q256_diagnose(void);

/**
 * @brief 全片检测（读回与写入一致即为 ok）。
 * @param test_sectors 测试扇区数（0=全片）
 * @return 0 表示全部通过，>0 为失败数。
 */
int w25q256_full_test(int test_sectors);

/**
 * @brief 快速功能测试 (1 个扇区: 擦除→写入→读回比对)。
 * @return ESP_OK 通过, ESP_FAIL 失败。
 */
esp_err_t w25q256_quick_test(void);

#ifdef __cplusplus
}
#endif
