#pragma once

/**
 * @file w25n01gv.h
 * @brief Winbond W25N01GVZEIG 1Gbit SPI NAND driver.
 *
 * The public read/write API uses linear byte addresses. Internally the driver
 * performs the NAND page-to-cache and cache-to-page command sequences.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define W25N01GV_PAGE_SIZE          2048U
#define W25N01GV_SPARE_SIZE           64U
#define W25N01GV_PAGES_PER_BLOCK      64U
#define W25N01GV_BLOCK_SIZE       131072U
#define W25N01GV_PHYSICAL_BLOCK_COUNT 1024U
/* Winbond guarantees a continuous logical address space for blocks 0..999.
 * Keep physical blocks 1000..1023 out of the public address space so they can
 * remain available to the chip's factory/user BBM LUT as replacement blocks. */
#define W25N01GV_USER_BLOCK_COUNT     1000U
#define W25N01GV_RESERVED_BLOCK_COUNT \
    (W25N01GV_PHYSICAL_BLOCK_COUNT - W25N01GV_USER_BLOCK_COUNT)
#define W25N01GV_PHYSICAL_SIZE \
    ((uint64_t)W25N01GV_PHYSICAL_BLOCK_COUNT * W25N01GV_BLOCK_SIZE)
#define W25N01GV_TOTAL_SIZE \
    ((uint64_t)W25N01GV_USER_BLOCK_COUNT * W25N01GV_BLOCK_SIZE)

#define W25N01GV_CMD_RESET             0xFF
#define W25N01GV_CMD_JEDEC_ID          0x9F
#define W25N01GV_CMD_READ_STATUS       0x0F
#define W25N01GV_CMD_WRITE_STATUS      0x1F
#define W25N01GV_CMD_WRITE_ENABLE      0x06
#define W25N01GV_CMD_WRITE_DISABLE     0x04
#define W25N01GV_CMD_BLOCK_ERASE       0xD8
#define W25N01GV_CMD_PROGRAM_LOAD      0x02
#define W25N01GV_CMD_PROGRAM_EXECUTE   0x10
#define W25N01GV_CMD_PAGE_DATA_READ    0x13
#define W25N01GV_CMD_READ_DATA         0x03
#define W25N01GV_CMD_READ_BBM_LUT      0xA5

#define W25N01GV_REG_PROTECTION        0xA0
#define W25N01GV_REG_CONFIGURATION     0xB0
#define W25N01GV_REG_STATUS            0xC0

#define W25N01GV_CFG_OTP_E              (1U << 6)
#define W25N01GV_CFG_ECC_E              (1U << 4)
#define W25N01GV_CFG_BUF                (1U << 3)

#define W25N01GV_STATUS_LUTF             (1U << 6)
#define W25N01GV_STATUS_ECC_MASK         (3U << 4)
#define W25N01GV_STATUS_ECC_CORRECTED    (1U << 4)
#define W25N01GV_STATUS_ECC_UNCORRECTABLE (2U << 4)
#define W25N01GV_STATUS_PFAIL            (1U << 3)
#define W25N01GV_STATUS_EFAIL            (1U << 2)
#define W25N01GV_STATUS_WEL              (1U << 1)
#define W25N01GV_STATUS_BUSY             (1U << 0)

esp_err_t w25n01gv_init(void);
void w25n01gv_deinit(void);

esp_err_t w25n01gv_read_jedec_id(uint8_t *manufacturer_id,
                                  uint8_t *device_id_high,
                                  uint8_t *device_id_low);
esp_err_t w25n01gv_read_unique_id(uint8_t uid[8]);

esp_err_t w25n01gv_read(uint32_t addr, uint8_t *buf, size_t len);
esp_err_t w25n01gv_write(uint32_t addr, const uint8_t *buf, size_t len);
esp_err_t w25n01gv_erase_block(uint32_t addr);
esp_err_t w25n01gv_erase_chip(void);

bool w25n01gv_wait_busy(uint32_t timeout_ms);
uint64_t w25n01gv_get_capacity(void);
esp_err_t w25n01gv_read_status(uint8_t *protection,
                                uint8_t *configuration,
                                uint8_t *status);
esp_err_t w25n01gv_is_bad_block(uint32_t block_index, bool *is_bad);

esp_err_t w25n01gv_diagnose(void);
esp_err_t w25n01gv_quick_test(void);
int w25n01gv_full_test(int test_blocks);

#ifdef __cplusplus
}
#endif
