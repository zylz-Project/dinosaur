/*
 * external_flash.h — 外置 SPI Flash 编译期抽象层（NOR / NAND 二选一）
 *
 * 职责：根据 config.h 的 EXTERNAL_FLASH_TYPE 在编译期把 external_flash_*()
 * 转发到 w25q256（NOR, 32MB）或 w25n01gv（NAND, 128MB）驱动。
 * flash_audio.cc 只看本头文件，不关心底层芯片型号。
 */
#pragma once

#include "config.h"
#include "esp_err.h"
#include "w25q256.h"
#include "w25n01gv.h"

#define EXTERNAL_FLASH_W25Q256       1
#define EXTERNAL_FLASH_W25N01GVZEIG  2
#define EXTERNAL_FLASH_TOC_SIZE      4096U

#if EXTERNAL_FLASH_TYPE == EXTERNAL_FLASH_W25Q256
#define EXTERNAL_FLASH_ERASE_SIZE W25Q256_SECTOR_SIZE
#define EXTERNAL_FLASH_PROGRAM_SIZE W25Q256_PAGE_SIZE
#define EXTERNAL_FLASH_NAME "W25Q256"
static inline esp_err_t external_flash_init(void) { return w25q256_init(); }
static inline esp_err_t external_flash_read(uint32_t a, uint8_t *b, size_t n) { return w25q256_read(a, b, n); }
static inline esp_err_t external_flash_write(uint32_t a, const uint8_t *b, size_t n) { return w25q256_write(a, b, n); }
static inline esp_err_t external_flash_erase_unit(uint32_t a) { return w25q256_erase_sector(a); }
static inline uint64_t external_flash_get_capacity(void) { return w25q256_get_capacity(); }
#elif EXTERNAL_FLASH_TYPE == EXTERNAL_FLASH_W25N01GVZEIG
#define EXTERNAL_FLASH_ERASE_SIZE W25N01GV_BLOCK_SIZE
#define EXTERNAL_FLASH_PROGRAM_SIZE W25N01GV_PAGE_SIZE
#define EXTERNAL_FLASH_NAME "W25N01GVZEIG"
static inline esp_err_t external_flash_init(void) { return w25n01gv_init(); }
static inline esp_err_t external_flash_read(uint32_t a, uint8_t *b, size_t n) { return w25n01gv_read(a, b, n); }
static inline esp_err_t external_flash_write(uint32_t a, const uint8_t *b, size_t n) { return w25n01gv_write(a, b, n); }
static inline esp_err_t external_flash_erase_unit(uint32_t a) { return w25n01gv_erase_block(a); }
static inline uint64_t external_flash_get_capacity(void) { return w25n01gv_get_capacity(); }
#else
#error "Unsupported EXTERNAL_FLASH_TYPE"
#endif
