#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Flash Layout
   ==========================================================================
   Sector 0 (0x000000, 4KB): TOC (Table of Contents)
     [0..3]   Magic: "PNDA"
     [4..7]   Version: uint32_t (2)
     [8..11]  File count: uint32_t (N)
     [12..]   File entries array (each 96 bytes):
       [0..63]   Filename (UTF-8, null-padded, 64 bytes) — display name, no extension
       [64..67]  Offset in flash (uint32_t, from start of data area)
       [68..71]  Size in bytes (uint32_t)
       [72..75]  Sample rate (uint32_t, e.g. 48000)
       [76..79]  Duration in milliseconds (uint32_t, estimated)
       [80..95]  Category (UTF-8, null-padded, 16 bytes) — "animal" or "ambient"

   Sector 1+ (0x001000+): Opus file data
     Each file stored contiguously, aligned to 4KB sector boundary

   Firmware reads all file metadata from TOC — no hardcoded filenames needed.
   To change audio files, rebuild opus_data.bin and reflash; firmware unchanged.
   ========================================================================== */

#define FLASH_AUDIO_TOC_SECTOR    0
#define FLASH_AUDIO_DATA_START    0x001000  // Sector 1
#define FLASH_AUDIO_MAX_FILES     32
#define FLASH_AUDIO_FILENAME_MAX  64
#define FLASH_AUDIO_ENTRY_SIZE    96       // 64 + 4*4 + 16 (category)
#define FLASH_AUDIO_CATEGORY_MAX  16
#define FLASH_AUDIO_TOC_MAGIC     0x41444E50  // "PNDA" little-endian
#define FLASH_AUDIO_TOC_VERSION   2         // v2: added category field

/* File info from TOC */
typedef struct {
    char     name[FLASH_AUDIO_FILENAME_MAX];
    uint32_t offset;      // offset from FLASH_AUDIO_DATA_START
    uint32_t size;        // file size in bytes
    uint32_t sample_rate;
    uint32_t duration_ms; // estimated duration in milliseconds
    char     category[FLASH_AUDIO_CATEGORY_MAX];  // "animal" or "ambient"
} flash_audio_info_t;

/**
 * @brief  Initialize flash audio storage.
 *         Reads TOC from SPI Flash if present, or creates an empty one.
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_init(void);

/**
 * @brief  Get number of files in the TOC.
 */
int flash_audio_get_file_count(void);

/**
 * @brief  Get file info by index.
 * @param  index  File index (0-based)
 * @param  info   [out] File metadata
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if index out of range.
 */
esp_err_t flash_audio_get_file_info(int index, flash_audio_info_t *info);

/**
 * @brief  Find file index by filename.
 * @param  filename  Filename to search for (exact match)
 * @return Index (0..N-1) on success, -1 if not found.
 */
int flash_audio_find_file(const char *filename);

/**
 * @brief  Read a chunk of a file from SPI Flash.
 * @param  index  File index (0-based)
 * @param  offset Byte offset within the file
 * @param  buf    Destination buffer
 * @param  len    Number of bytes to read
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_read_file(int index, uint32_t offset, uint8_t *buf, size_t len);

/**
 * @brief  Write a file to SPI Flash (used by upload/flashing tools).
 *         Erases the necessary sectors and writes the TOC.
 * @param  filename   File name (max 63 chars)
 * @param  data       File data
 * @param  len        Data length
 * @param  sample_rate Audio sample rate (e.g. 48000)
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_write_file(const char *filename, const uint8_t *data,
                                  size_t len, uint32_t sample_rate);

/**
 * @brief  Delete a specific file from flash by name.
 *         Removes the TOC entry and rewrites TOC. Data area is NOT erased
 *         (wastes a little space but is safer and faster).
 * @param  filename  Exact filename to delete
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file doesn't exist.
 */
esp_err_t flash_audio_delete_file(const char *filename);

/**
 * @brief  Erase all audio data sectors and reset TOC.
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_erase_all(void);

/**
 * @brief  Write the TOC to flash (sector 0).
 *         Used by external flashing tools.
 * @param  toc_data  Raw TOC binary
 * @param  toc_len   Length (must be <= 4096)
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_write_toc(const uint8_t *toc_data, size_t toc_len);

/**
 * @brief  Read the raw TOC from flash.
 * @param  buf    Buffer (should be at least 4096 bytes)
 * @param  buf_sz Buffer size
 * @return ESP_OK on success.
 */
esp_err_t flash_audio_read_toc(uint8_t *buf, size_t buf_sz);

/* ==========================================================================
   Streaming write — for network downloads without buffering in RAM
   ========================================================================== */

typedef struct {
    char     name[FLASH_AUDIO_FILENAME_MAX];
    char     category[FLASH_AUDIO_CATEGORY_MAX];
    uint32_t total_size;
    uint32_t written;
    uint32_t flash_addr;    // absolute flash address being written to
    uint32_t data_offset;   // offset within data area (for TOC)
    int      existing_idx;  // -1 if new, else index to replace
    bool     active;
} flash_audio_stream_t;

/** Begin streaming write. Erases all needed sectors upfront. */
esp_err_t flash_audio_stream_begin(flash_audio_stream_t *s, const char *filename,
                                    uint32_t total_size, uint32_t sample_rate,
                                    const char *category);

/** Write next chunk. Call repeatedly as data arrives from network. */
esp_err_t flash_audio_stream_write(flash_audio_stream_t *s, const uint8_t *data, size_t len);

/** Finish streaming write. Updates TOC if all data received. */
esp_err_t flash_audio_stream_end(flash_audio_stream_t *s);

/**
 * @brief  Get display name pointer for a file by TOC index.
 *         Pointer is valid as long as TOC is loaded (lifetime of the process).
 * @param  index  File index (0-based)
 * @return Name string, "???" if index out of range or TOC not loaded.
 */
const char *flash_audio_get_name(int index);

/**
 * @brief  Get duration in milliseconds for a file by TOC index.
 * @param  index  File index (0-based)
 * @return Duration in ms, 0 if index out of range or TOC not loaded.
 */
int flash_audio_get_duration_ms(int index);

/**
 * @brief  Get file count for a specific category.
 * @param  category  "animal" or "ambient"
 * @return Number of files in that category.
 */
int flash_audio_get_count_by_category(const char *category);

/**
 * @brief  Get a random file index within a specific category.
 * @param  category  "animal" or "ambient"
 * @return Random valid index, or -1 if category has no files.
 */
int flash_audio_get_random_in_category(const char *category);

/**
 * @brief  Get the category string for a file by index.
 * @param  index  File index (0-based)
 * @return Category string ("animal", "ambient"), or "???" if invalid.
 */
const char *flash_audio_get_category(int index);

#ifdef __cplusplus
}
#endif
