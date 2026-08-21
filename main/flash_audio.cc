#include "flash_audio.h"
#include "external_flash.h"

#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "flash_audio";

static flash_audio_info_t g_files[FLASH_AUDIO_MAX_FILES];
static int g_file_count = 0;
static bool g_initialized = false;

/* ==========================================================================
   Internal helpers
   ========================================================================== */

/* Serialize TOC to buffer */
static size_t toc_serialize(uint8_t *buf, size_t buf_sz)
{
    if (buf_sz < 12 + FLASH_AUDIO_MAX_FILES * FLASH_AUDIO_ENTRY_SIZE)
        return 0;

    uint32_t magic = FLASH_AUDIO_TOC_MAGIC;
    uint32_t version = FLASH_AUDIO_TOC_VERSION;
    uint32_t count = g_file_count;

    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &version, 4);
    memcpy(buf + 8, &count, 4);

    for (int i = 0; i < g_file_count; i++) {
        uint8_t *entry = buf + 12 + i * FLASH_AUDIO_ENTRY_SIZE;
        memset(entry, 0, FLASH_AUDIO_ENTRY_SIZE);
        strncpy((char *)entry, g_files[i].name, FLASH_AUDIO_FILENAME_MAX - 1);
        memcpy(entry + 64, &g_files[i].offset, 4);
        memcpy(entry + 68, &g_files[i].size, 4);
        memcpy(entry + 72, &g_files[i].sample_rate, 4);
        memcpy(entry + 76, &g_files[i].duration_ms, 4);
        strncpy((char *)(entry + 80), g_files[i].category, FLASH_AUDIO_CATEGORY_MAX - 1);
    }

    return 12 + count * FLASH_AUDIO_ENTRY_SIZE;
}

/* Deserialize TOC from buffer */
static bool toc_deserialize(const uint8_t *buf, size_t buf_sz)
{
    if (buf_sz < 12)
        return false;

    uint32_t magic, version, count;
    memcpy(&magic, buf, 4);
    memcpy(&version, buf + 4, 4);
    memcpy(&count, buf + 8, 4);

    if (magic != FLASH_AUDIO_TOC_MAGIC) {
        ESP_LOGW(TAG, "TOC magic mismatch: 0x%08lX (expected 0x%08X)",
                 (unsigned long)magic, FLASH_AUDIO_TOC_MAGIC);
        return false;
    }
    if (count > FLASH_AUDIO_MAX_FILES) {
        ESP_LOGW(TAG, "TOC file count too large: %lu (max %d)",
                 (unsigned long)count, FLASH_AUDIO_MAX_FILES);
        return false;
    }

    g_file_count = count;
    for (int i = 0; i < g_file_count; i++) {
        const uint8_t *entry = buf + 12 + i * FLASH_AUDIO_ENTRY_SIZE;
        strncpy(g_files[i].name, (const char *)entry, FLASH_AUDIO_FILENAME_MAX - 1);
        g_files[i].name[FLASH_AUDIO_FILENAME_MAX - 1] = '\0';
        memcpy(&g_files[i].offset, entry + 64, 4);
        memcpy(&g_files[i].size, entry + 68, 4);
        memcpy(&g_files[i].sample_rate, entry + 72, 4);
        memcpy(&g_files[i].duration_ms, entry + 76, 4);
        // v2: read category field (default to "animal" for v1 compat)
        if (version >= 2) {
            strncpy(g_files[i].category, (const char *)(entry + 80), FLASH_AUDIO_CATEGORY_MAX - 1);
            g_files[i].category[FLASH_AUDIO_CATEGORY_MAX - 1] = '\0';
        } else {
            strncpy(g_files[i].category, "animal", FLASH_AUDIO_CATEGORY_MAX - 1);
        }
    }

    ESP_LOGI(TAG, "TOC loaded: %d files", g_file_count);
    return true;
}

/* Flush TOC to SPI Flash */
static esp_err_t toc_flush(void)
{
    uint8_t buf[EXTERNAL_FLASH_TOC_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    size_t toc_len = toc_serialize(buf, sizeof(buf));
    if (toc_len == 0)
        return ESP_FAIL;

    // Erase TOC sector and write
    uint32_t toc_addr = FLASH_AUDIO_TOC_SECTOR * EXTERNAL_FLASH_ERASE_SIZE;
    esp_err_t ret = external_flash_erase_unit(toc_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase TOC sector");
        return ret;
    }
    ret = external_flash_write(toc_addr, buf, toc_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write TOC");
        return ret;
    }
    ESP_LOGI(TAG, "TOC flushed (%zu bytes, %d files)", toc_len, g_file_count);
    return ESP_OK;
}

/* ==========================================================================
   Public API
   ========================================================================== */

esp_err_t flash_audio_init(void)
{
    if (g_initialized)
        return ESP_OK;

    // Initialize SPI Flash hardware
    esp_err_t ret = external_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Flash init failed — audio from flash disabled");
        g_file_count = 0;
        g_initialized = true;  // Mark as initialized but empty
        return ret;
    }
    ESP_LOGI(TAG, "External flash selected: %s", EXTERNAL_FLASH_NAME);

    // Read TOC from flash
    uint8_t toc_buf[EXTERNAL_FLASH_TOC_SIZE];
    uint32_t toc_addr = FLASH_AUDIO_TOC_SECTOR * EXTERNAL_FLASH_ERASE_SIZE;
    ret = external_flash_read(toc_addr, toc_buf, sizeof(toc_buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read TOC sector");
        g_file_count = 0;
        g_initialized = true;
        return ret;
    }

    if (!toc_deserialize(toc_buf, sizeof(toc_buf))) {
        ESP_LOGI(TAG, "No valid TOC found — audio flash is empty");
        g_file_count = 0;
    }

    g_initialized = true;
    uint64_t cap = external_flash_get_capacity();
    uint32_t used = 0;
    int animal_cnt = 0, ambient_cnt = 0;
    for (int i = 0; i < g_file_count; i++) {
        used += g_files[i].size;
        if (strcmp(g_files[i].category, "ambient") == 0) ambient_cnt++;
        else animal_cnt++;
    }
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Flash capacity: %llu MB", (unsigned long long)(cap / (1024*1024)));
    ESP_LOGI(TAG, "  Audio files:    %d total (🐾%d animal  🌿%d ambient)",
             g_file_count, animal_cnt, ambient_cnt);
    ESP_LOGI(TAG, "  Used:           %lu KB (%.1f%%)",
             (unsigned long)(used/1024), used*100.0f/(cap ? cap : 1));
    ESP_LOGI(TAG, "  TOC sector:     %d", FLASH_AUDIO_TOC_SECTOR);
    ESP_LOGI(TAG, "  Data start:     0x%06lX", (unsigned long)FLASH_AUDIO_DATA_START);
    for (int i = 0; i < g_file_count && i < 10; i++) {
        ESP_LOGI(TAG, "  [%d] %-30s %6lu KB  %s",
                 i, g_files[i].name, (unsigned long)(g_files[i].size/1024),
                 g_files[i].category);
    }
    if (g_file_count > 10) ESP_LOGI(TAG, "  ... +%d more files", g_file_count - 10);
    ESP_LOGI(TAG, "========================================");
    return ESP_OK;
}

int flash_audio_get_file_count(void)
{
    return g_file_count;
}

esp_err_t flash_audio_get_file_info(int index, flash_audio_info_t *info)
{
    if (!info || index < 0 || index >= g_file_count)
        return ESP_ERR_NOT_FOUND;
    memcpy(info, &g_files[index], sizeof(flash_audio_info_t));
    return ESP_OK;
}

const char *flash_audio_get_name(int index)
{
    if (index < 0 || index >= g_file_count)
        return "???";
    return g_files[index].name;
}

int flash_audio_find_file(const char *filename)
{
    for (int i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].name, filename) == 0)
            return i;
    }
    return -1;
}

int flash_audio_get_duration_ms(int index)
{
    if (index < 0 || index >= g_file_count)
        return 0;
    return (int)g_files[index].duration_ms;
}

esp_err_t flash_audio_read_file(int index, uint32_t offset, uint8_t *buf, size_t len)
{
    if (index < 0 || index >= g_file_count)
        return ESP_ERR_NOT_FOUND;
    if (offset + len > g_files[index].size)
        return ESP_ERR_INVALID_ARG;

    uint32_t flash_addr = FLASH_AUDIO_DATA_START + g_files[index].offset + offset;
    return external_flash_read(flash_addr, buf, len);
}

esp_err_t flash_audio_write_file(const char *filename, const uint8_t *data,
                                  size_t len, uint32_t sample_rate)
{
    if (!filename || !data || len == 0)
        return ESP_ERR_INVALID_ARG;
    if (g_file_count >= FLASH_AUDIO_MAX_FILES)
        return ESP_ERR_NO_MEM;

    // Check if file already exists
    int existing = flash_audio_find_file(filename);
    int index = existing >= 0 ? existing : g_file_count;

    // Calculate offset: after last file, aligned to sector
    uint32_t offset = 0;
    if (existing >= 0) {
        // Replace existing file — use same offset
        offset = g_files[existing].offset;
    } else {
        // New file — append after last file
        if (g_file_count > 0) {
            auto &last = g_files[g_file_count - 1];
            offset = last.offset + last.size;
            // Align to sector boundary
            if (offset % EXTERNAL_FLASH_ERASE_SIZE != 0) {
                offset = ((offset / EXTERNAL_FLASH_ERASE_SIZE) + 1) * EXTERNAL_FLASH_ERASE_SIZE;
            }
        }
        // else first file, offset = 0
    }

    // Erase sectors needed (with yield to keep WiFi alive)
    uint32_t start_sector = (FLASH_AUDIO_DATA_START + offset) / EXTERNAL_FLASH_ERASE_SIZE;
    uint32_t end_sector = (FLASH_AUDIO_DATA_START + offset + len + EXTERNAL_FLASH_ERASE_SIZE - 1) / EXTERNAL_FLASH_ERASE_SIZE;
    for (uint32_t sec = start_sector; sec < end_sector; sec++) {
        esp_err_t ret = external_flash_erase_unit(sec * EXTERNAL_FLASH_ERASE_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase sector %lu", (unsigned long)sec);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // Yield to WiFi/other tasks
    }

    // Write data
    uint32_t flash_addr = FLASH_AUDIO_DATA_START + offset;
    esp_err_t ret = external_flash_write(flash_addr, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write file data");
        return ret;
    }

    // Update TOC entry — strip .opus extension for display name consistency
    {
        char clean_name[FLASH_AUDIO_FILENAME_MAX];
        strncpy(clean_name, filename, FLASH_AUDIO_FILENAME_MAX - 1);
        clean_name[FLASH_AUDIO_FILENAME_MAX - 1] = '\0';
        // Strip trailing .opus if present
        size_t nlen = strlen(clean_name);
        if (nlen > 5 && strcmp(clean_name + nlen - 5, ".opus") == 0)
            clean_name[nlen - 5] = '\0';
        strncpy(g_files[index].name, clean_name, FLASH_AUDIO_FILENAME_MAX - 1);
    }
    g_files[index].name[FLASH_AUDIO_FILENAME_MAX - 1] = '\0';
    g_files[index].offset = offset;
    g_files[index].size = len;
    g_files[index].sample_rate = sample_rate;
    g_files[index].duration_ms = (uint32_t)(len * 1000ULL / 6000);  // est. ~48kbps

    if (existing < 0)
        g_file_count++;

    ESP_LOGI(TAG, "File written: %s (%zu bytes @ offset 0x%06lX)", filename, len, (unsigned long)offset);

    return toc_flush();
}

/* ==========================================================================
   Streaming write (for network downloads — no full-file RAM buffer needed)
   ========================================================================== */

esp_err_t flash_audio_stream_begin(flash_audio_stream_t *s, const char *filename,
                                    uint32_t total_size, uint32_t sample_rate,
                                    const char *category)
{
    if (!s || !filename || total_size == 0) return ESP_ERR_INVALID_ARG;

    memset(s, 0, sizeof(*s));
    strncpy(s->name, filename, sizeof(s->name) - 1);
    if (category) strncpy(s->category, category, sizeof(s->category) - 1);
    else strncpy(s->category, "animal", sizeof(s->category) - 1);
    s->total_size = total_size;

    // Strip .opus for TOC name
    size_t nlen = strlen(s->name);
    if (nlen > 5 && strcmp(s->name + nlen - 5, ".opus") == 0)
        s->name[nlen - 5] = '\0';

    // Check if replacing existing file
    s->existing_idx = flash_audio_find_file(s->name);
    if (s->existing_idx >= 0) {
        s->data_offset = g_files[s->existing_idx].offset;
    } else {
        // Append after last file, aligned to sector
        if (g_file_count > 0) {
            auto &last = g_files[g_file_count - 1];
            s->data_offset = last.offset + last.size;
            if (s->data_offset % EXTERNAL_FLASH_ERASE_SIZE != 0)
                s->data_offset = ((s->data_offset / EXTERNAL_FLASH_ERASE_SIZE) + 1) * EXTERNAL_FLASH_ERASE_SIZE;
        }
    }

    s->flash_addr = FLASH_AUDIO_DATA_START + s->data_offset;

    uint32_t start_sec = s->flash_addr / EXTERNAL_FLASH_ERASE_SIZE;
    uint32_t end_sec = (s->flash_addr + total_size + EXTERNAL_FLASH_ERASE_SIZE - 1) / EXTERNAL_FLASH_ERASE_SIZE;
    uint32_t sec_count = end_sec - start_sec;
    ESP_LOGI(TAG, "Stream begin: %s (%lu KB, %lu sectors @ 0x%06lX)",
             s->name, (unsigned long)(total_size/1024),
             (unsigned long)sec_count, (unsigned long)s->flash_addr);

    // Erase all needed sectors upfront
    bool show_progress = (sec_count > 50);  // only for large files
    int erase_log_every = sec_count / 5;
    if (erase_log_every < 1) erase_log_every = 1;

    for (uint32_t sec = start_sec; sec < end_sec; sec++) {
        esp_err_t ret = external_flash_erase_unit(sec * EXTERNAL_FLASH_ERASE_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Erase failed @ sector %lu", (unsigned long)sec);
            return ret;
        }
        if (show_progress) {
            uint32_t done = sec - start_sec + 1;
            if (done % erase_log_every == 0 || done == sec_count) {
                ESP_LOGI(TAG, "Erase: %d%% (%lu/%lu)", (int)(done * 100 / sec_count),
                         (unsigned long)done, (unsigned long)sec_count);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    s->pending_buf = (uint8_t *)malloc(EXTERNAL_FLASH_PROGRAM_SIZE);
    if (!s->pending_buf) return ESP_ERR_NO_MEM;
    memset(s->pending_buf, 0xFF, EXTERNAL_FLASH_PROGRAM_SIZE);
    s->active = true;
    return ESP_OK;
}

esp_err_t flash_audio_stream_write(flash_audio_stream_t *s, const uint8_t *data, size_t len)
{
    if (!s || !s->active || s->failed) return ESP_ERR_INVALID_STATE;
    if ((!data && len) || s->written + len > s->total_size) {
        s->failed = true;
        return ESP_ERR_INVALID_ARG;
    }

    size_t consumed = 0;
    while (consumed < len) {
        size_t space = EXTERNAL_FLASH_PROGRAM_SIZE - s->pending_len;
        size_t take = len - consumed;
        if (take > space) take = space;
        memcpy(s->pending_buf + s->pending_len, data + consumed, take);
        s->pending_len += take;
        s->written += take;
        consumed += take;

        if (s->pending_len == EXTERNAL_FLASH_PROGRAM_SIZE) {
            esp_err_t ret = external_flash_write(s->flash_addr + s->programmed,
                                                 s->pending_buf,
                                                 EXTERNAL_FLASH_PROGRAM_SIZE);
            if (ret != ESP_OK) {
                s->failed = true;
                return ret;
            }
            s->programmed += EXTERNAL_FLASH_PROGRAM_SIZE;
            s->pending_len = 0;
            memset(s->pending_buf, 0xFF, EXTERNAL_FLASH_PROGRAM_SIZE);
        }
    }
    return ESP_OK;
}

esp_err_t flash_audio_stream_end(flash_audio_stream_t *s)
{
    if (!s || !s->active) return ESP_ERR_INVALID_STATE;
    s->active = false;

    if (s->failed || s->written != s->total_size) {
        ESP_LOGW(TAG, "Stream incomplete: %s (%lu/%lu bytes)",
                 s->name, (unsigned long)s->written, (unsigned long)s->total_size);
        free(s->pending_buf);
        s->pending_buf = nullptr;
        return ESP_FAIL;
    }

    if (s->pending_len > 0) {
        esp_err_t ret = external_flash_write(s->flash_addr + s->programmed,
                                             s->pending_buf,
                                             EXTERNAL_FLASH_PROGRAM_SIZE);
        if (ret != ESP_OK) {
            free(s->pending_buf);
            s->pending_buf = nullptr;
            return ret;
        }
        s->programmed += EXTERNAL_FLASH_PROGRAM_SIZE;
        s->pending_len = 0;
    }
    free(s->pending_buf);
    s->pending_buf = nullptr;

    // Update TOC entry
    int idx = s->existing_idx;
    if (idx < 0) {
        if (g_file_count >= FLASH_AUDIO_MAX_FILES) return ESP_ERR_NO_MEM;
        idx = g_file_count;
        g_file_count++;
    }

    strncpy(g_files[idx].name, s->name, FLASH_AUDIO_FILENAME_MAX - 1);
    g_files[idx].name[FLASH_AUDIO_FILENAME_MAX - 1] = '\0';
    g_files[idx].offset     = s->data_offset;
    g_files[idx].size       = s->total_size;
    g_files[idx].sample_rate = 48000;
    g_files[idx].duration_ms = (uint32_t)(s->total_size * 1000ULL / 6000);
    strncpy(g_files[idx].category, s->category, FLASH_AUDIO_CATEGORY_MAX - 1);

    ESP_LOGI(TAG, "Stream end OK: %s [%s] (%lu KB, %d files total)",
             s->name, g_files[idx].category, (unsigned long)(s->total_size/1024), g_file_count);
    return toc_flush();
}

esp_err_t flash_audio_delete_file(const char *filename)
{
    if (!filename) return ESP_ERR_INVALID_ARG;

    int idx = flash_audio_find_file(filename);
    if (idx < 0) {
        ESP_LOGW(TAG, "Delete failed: '%s' not found", filename);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t data_addr = FLASH_AUDIO_DATA_START + g_files[idx].offset;
    uint32_t data_size = g_files[idx].size;

    ESP_LOGI(TAG, "Delete: %s (%lu KB)", g_files[idx].name,
             (unsigned long)data_size / 1024);

    // Erase data sectors
    uint32_t start_sec = data_addr / EXTERNAL_FLASH_ERASE_SIZE;
    uint32_t end_sec = (data_addr + data_size + EXTERNAL_FLASH_ERASE_SIZE - 1) / EXTERNAL_FLASH_ERASE_SIZE;
    for (uint32_t sec = start_sec; sec < end_sec; sec++) {
        external_flash_erase_unit(sec * EXTERNAL_FLASH_ERASE_SIZE);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Shift remaining entries forward
    for (int i = idx; i < g_file_count - 1; i++) {
        memcpy(&g_files[i], &g_files[i + 1], sizeof(flash_audio_info_t));
    }
    g_file_count--;

    return toc_flush();
}

esp_err_t flash_audio_erase_all(void)
{
    ESP_LOGI(TAG, "Erasing all audio data...");

    // Count total sectors
    uint32_t total_sec = 1;  // TOC
    for (int i = 0; i < g_file_count; i++) {
        uint32_t start = FLASH_AUDIO_DATA_START + g_files[i].offset;
        uint32_t end = start + g_files[i].size;
        total_sec += (end + EXTERNAL_FLASH_ERASE_SIZE - 1) / EXTERNAL_FLASH_ERASE_SIZE
                     - start / EXTERNAL_FLASH_ERASE_SIZE;
    }

    // Erase TOC sector
    external_flash_erase_unit(FLASH_AUDIO_TOC_SECTOR * EXTERNAL_FLASH_ERASE_SIZE);
    uint32_t done = 1;
    int last_pct = 0;

    // Erase data sectors
    for (int i = 0; i < g_file_count; i++) {
        uint32_t start = FLASH_AUDIO_DATA_START + g_files[i].offset;
        uint32_t end = start + g_files[i].size;
        uint32_t sec_start = start / EXTERNAL_FLASH_ERASE_SIZE;
        uint32_t sec_end = (end + EXTERNAL_FLASH_ERASE_SIZE - 1) / EXTERNAL_FLASH_ERASE_SIZE;
        for (uint32_t sec = sec_start; sec < sec_end; sec++) {
            external_flash_erase_unit(sec * EXTERNAL_FLASH_ERASE_SIZE);
            done++;
            int pct = (int)(done * 100 / total_sec);
            if (pct - last_pct >= 10) {
                last_pct = pct;
                ESP_LOGI(TAG, "Erase all: %d%% (%lu/%lu sectors)",
                         pct, (unsigned long)done, (unsigned long)total_sec);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    ESP_LOGI(TAG, "Erase all: 100%% (%lu sectors)", (unsigned long)total_sec);
    g_file_count = 0;
    memset(g_files, 0, sizeof(g_files));
    return toc_flush();
}

esp_err_t flash_audio_write_toc(const uint8_t *toc_data, size_t toc_len)
{
    if (!toc_data || toc_len == 0 || toc_len > EXTERNAL_FLASH_TOC_SIZE)
        return ESP_ERR_INVALID_ARG;

    uint8_t buf[EXTERNAL_FLASH_TOC_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, toc_data, toc_len);

    uint32_t toc_addr = FLASH_AUDIO_TOC_SECTOR * EXTERNAL_FLASH_ERASE_SIZE;
    esp_err_t ret = external_flash_erase_unit(toc_addr);
    if (ret != ESP_OK)
        return ret;
    ret = external_flash_write(toc_addr, buf, toc_len);
    if (ret != ESP_OK)
        return ret;

    // Reload TOC
    toc_deserialize(buf, sizeof(buf));
    ESP_LOGI(TAG, "TOC written from external source: %d files", g_file_count);
    return ESP_OK;
}

esp_err_t flash_audio_read_toc(uint8_t *buf, size_t buf_sz)
{
    if (!buf || buf_sz < EXTERNAL_FLASH_TOC_SIZE)
        return ESP_ERR_INVALID_ARG;

    uint32_t toc_addr = FLASH_AUDIO_TOC_SECTOR * EXTERNAL_FLASH_ERASE_SIZE;
    return external_flash_read(toc_addr, buf, EXTERNAL_FLASH_TOC_SIZE);
}

int flash_audio_get_count_by_category(const char *category)
{
    if (!category) return 0;
    int count = 0;
    for (int i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].category, category) == 0) count++;
    }
    return count;
}

int flash_audio_get_random_in_category(const char *category)
{
    if (!category) return -1;
    // Collect matching indices
    int matches[FLASH_AUDIO_MAX_FILES];
    int n = 0;
    for (int i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].category, category) == 0)
            matches[n++] = i;
    }
    if (n == 0) return -1;
    return matches[rand() % n];
}

const char *flash_audio_get_category(int index)
{
    if (index < 0 || index >= g_file_count) return "???";
    return g_files[index].category;
}
