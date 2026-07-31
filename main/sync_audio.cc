/**
 * @file    sync_audio.cc
 * @brief   从 HTTP 服务端同步音频文件到 SPI Flash (流式写入, 无 RAM 缓冲)
 */

#include "sync_audio.h"
#include "flash_audio.h"
#include "config.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char *TAG = "sync";
static const char *SYNC_NVS_NAMESPACE = "audio_sync";
static const char *SYNC_NVS_REVISION_KEY = "revision";
static constexpr size_t SYNC_REVISION_MAX = 64;

/* --------------------------------------------------------------------------
   Server endpoint helpers
   -------------------------------------------------------------------------- */

static char g_base_url[64] = {};

static void build_base_url()
{
    snprintf(g_base_url, sizeof(g_base_url),
             "http://%s:%d", SYNC_SERVER_IP, SYNC_SERVER_PORT);
}

/* --------------------------------------------------------------------------
   HTTP GET helper — accumulate response into a heap buffer (for manifest)
   -------------------------------------------------------------------------- */

struct http_buf_t {
    uint8_t *data;
    size_t   cap;
    size_t   len;
};

static esp_err_t http_buf_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        size_t need = buf->len + evt->data_len + 1;
        if (need > buf->cap) {
            size_t new_cap = buf->cap * 2;
            if (new_cap < need) new_cap = need + 1024;
            uint8_t *p = (uint8_t *)realloc(buf->data, new_cap);
            if (!p) return ESP_ERR_NO_MEM;
            buf->data = p;
            buf->cap  = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, evt->data_len);
        buf->len = need;
        buf->len--;
        buf->data[buf->len] = '\0';
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t http_get_buf(const char *url, http_buf_t *out, int timeout_ms)
{
    out->data = (uint8_t *)malloc(4096);
    if (!out->data) return ESP_ERR_NO_MEM;
    out->cap = 4096;
    out->len = 0;
    out->data[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 2048;
    cfg.event_handler = http_buf_handler;
    cfg.user_data = out;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(out->data); out->data = nullptr; return ESP_FAIL; }

    esp_err_t ret = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (ret != ESP_OK || status != 200) {
        free(out->data);
        out->data = nullptr;
        out->len = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
   Streaming download — writes directly to flash, no RAM buffering
   -------------------------------------------------------------------------- */

struct stream_ctx_t {
    flash_audio_stream_t *flash;
    const char *fname;      // for log
    int   idx, total;        // for progress "[idx/total]"
    int   last_pct;          // last logged percentage
    bool  ok;
};

static esp_err_t stream_handler(esp_http_client_event_t *evt)
{
    stream_ctx_t *ctx = (stream_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (flash_audio_stream_write(ctx->flash,
                                      (const uint8_t *)evt->data,
                                      evt->data_len) != ESP_OK) {
            ctx->ok = false;
            return ESP_FAIL;
        }
        // Log progress every 20%
        if (ctx->flash->total_size > 100 * 1024) {  // only for files > 100KB
            int pct = (int)(ctx->flash->written * 100ULL / ctx->flash->total_size);
            if (pct - ctx->last_pct >= 20) {
                ctx->last_pct = pct;
                ESP_LOGI(TAG, "[%d/%d] %s  %d%% (%lu/%lu KB)",
                         ctx->idx, ctx->total, ctx->fname, pct,
                         (unsigned long)(ctx->flash->written / 1024),
                         (unsigned long)(ctx->flash->total_size / 1024));
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ctx->ok = true;
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t download_stream(int index, int total, const char *fname,
                                  uint32_t fsize, flash_audio_stream_t *s)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/api/download-idx/%d?product=%s", g_base_url, index, SYNC_PRODUCT_ID);

    stream_ctx_t ctx = { .flash = s, .fname = fname,
                         .idx = index + 1, .total = total,
                         .last_pct = 0, .ok = false };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 180000;   // 3 min for large files
    cfg.buffer_size = 8192;    // 8KB buffer for faster download
    cfg.event_handler = stream_handler;
    cfg.user_data = &ctx;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t ret = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (ret != ESP_OK || status != 200 || !ctx.ok) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
   Manifest parsing
   -------------------------------------------------------------------------- */

struct server_file_t {
    char     name[FLASH_AUDIO_FILENAME_MAX];
    uint32_t size;
    char     category[16];  // "animal" or "ambient"
};

static bool parse_manifest(const char *json, size_t len,
                           std::vector<server_file_t> *files,
                           char *revision, size_t revision_size)
{
    if (!json || !files || !revision || revision_size == 0) return false;
    files->clear();
    revision[0] = '\0';

    cJSON *root = cJSON_ParseWithLength(json, len);
    cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "files") : nullptr;
    cJSON *revision_item = root
        ? cJSON_GetObjectItemCaseSensitive(root, "revision")
        : nullptr;
    if (!cJSON_IsObject(root) || !cJSON_IsArray(items) ||
        !cJSON_IsString(revision_item) || !revision_item->valuestring ||
        strlen(revision_item->valuestring) == 0 ||
        strlen(revision_item->valuestring) >= revision_size) {
        ESP_LOGW(TAG, "Invalid manifest JSON");
        cJSON_Delete(root);
        return false;
    }
    strlcpy(revision, revision_item->valuestring, revision_size);

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
        cJSON *category = cJSON_GetObjectItemCaseSensitive(item, "category");
        if (!cJSON_IsObject(item) || !cJSON_IsString(name) ||
            !name->valuestring || !cJSON_IsNumber(size) ||
            size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
            !cJSON_IsString(category) || !category->valuestring ||
            (strcmp(category->valuestring, "animal") != 0 &&
             strcmp(category->valuestring, "ambient") != 0)) {
            ESP_LOGW(TAG, "Manifest contains an invalid file entry");
            cJSON_Delete(root);
            files->clear();
            revision[0] = '\0';
            return false;
        }
        size_t name_len = strlen(name->valuestring);
        if (name_len == 0 || name_len >= sizeof(server_file_t::name)) {
            ESP_LOGW(TAG, "Manifest contains an overlong filename (%u bytes)",
                     (unsigned)name_len);
            cJSON_Delete(root);
            files->clear();
            revision[0] = '\0';
            return false;
        }
        if (name_len <= 5 ||
            strcmp(name->valuestring + name_len - 5, ".opus") != 0) {
            ESP_LOGW(TAG, "Manifest contains a non-Opus filename");
            cJSON_Delete(root);
            files->clear();
            revision[0] = '\0';
            return false;
        }
        server_file_t sf = {};
        strlcpy(sf.name, name->valuestring, sizeof(sf.name));
        sf.size = (uint32_t)size->valuedouble;
        strlcpy(sf.category, category->valuestring, sizeof(sf.category));
        for (const auto &existing : *files) {
            if (strcmp(existing.name, sf.name) == 0) {
                ESP_LOGW(TAG, "Manifest contains duplicate filename: %s", sf.name);
                cJSON_Delete(root);
                files->clear();
                revision[0] = '\0';
                return false;
            }
        }
        files->push_back(sf);
    }
    cJSON_Delete(root);
    return true;
}

static bool load_synced_revision(char *revision, size_t revision_size)
{
    if (!revision || revision_size == 0) return false;
    revision[0] = '\0';

    nvs_handle_t handle;
    if (nvs_open(SYNC_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t required = revision_size;
    esp_err_t ret = nvs_get_str(handle, SYNC_NVS_REVISION_KEY, revision, &required);
    nvs_close(handle);
    if (ret != ESP_OK) {
        revision[0] = '\0';
        return false;
    }
    return true;
}

static bool save_synced_revision(const char *revision)
{
    if (!revision || revision[0] == '\0') return false;

    nvs_handle_t handle;
    if (nvs_open(SYNC_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS to save manifest revision");
        return false;
    }
    esp_err_t ret = nvs_set_str(handle, SYNC_NVS_REVISION_KEY, revision);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot save manifest revision: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

/* ==========================================================================
   Main sync logic
   ========================================================================== */

/* strip .opus extension for display */
static void strip_opus(char *name)
{
    size_t n = strlen(name);
    if (n > 5 && strcmp(name + n - 5, ".opus") == 0)
        name[n - 5] = '\0';
}

/* format file size for display */
static const char *fmt_size(uint32_t bytes)
{
    static char b[16];
    if (bytes >= 1024 * 1024)
        snprintf(b, sizeof(b), "%.1f MB", bytes / (1024.0f * 1024.0f));
    else if (bytes >= 1024)
        snprintf(b, sizeof(b), "%lu KB", (unsigned long)(bytes / 1024));
    else
        snprintf(b, sizeof(b), "%lu B", (unsigned long)bytes);
    return b;
}

esp_err_t sync_audio_files(void)
{
    build_base_url();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Sync Server: %s", g_base_url);
    ESP_LOGI(TAG, "  Product ID:  %s", SYNC_PRODUCT_ID);
    ESP_LOGI(TAG, "========================================");

    // 1. Fetch manifest
    char url[128];
    snprintf(url, sizeof(url), "%s/api/files?product=%s", g_base_url, SYNC_PRODUCT_ID);
    ESP_LOGI(TAG, "Fetch manifest: %s", url);

    http_buf_t buf = {};
    esp_err_t ret = http_get_buf(url, &buf, 10000);
    if (ret != ESP_OK || buf.data == nullptr) {
        ESP_LOGW(TAG, "Server unreachable, skip sync");
        return ESP_FAIL;
    }

    std::vector<server_file_t> server_files;
    char server_revision[SYNC_REVISION_MAX] = {};
    bool manifest_ok = parse_manifest((const char *)buf.data, buf.len,
                                      &server_files, server_revision,
                                      sizeof(server_revision));
    free(buf.data);
    if (!manifest_ok) {
        ESP_LOGW(TAG, "Reject invalid manifest; flash contents left unchanged");
        return ESP_FAIL;
    }

    char synced_revision[SYNC_REVISION_MAX] = {};
    load_synced_revision(synced_revision, sizeof(synced_revision));
    bool force_refresh = strcmp(server_revision, synced_revision) != 0;
    if (force_refresh) {
        ESP_LOGI(TAG, "Manifest revision changed: %s -> %s; verify all files",
                 synced_revision[0] ? synced_revision : "(none)", server_revision);
    }

    int local_count = flash_audio_get_file_count();
    ESP_LOGI(TAG, "Server[%zu] <-> Flash[%d]", server_files.size(), local_count);

    // 2. Compare: find files to delete
    int deleted = 0;
    for (int i = local_count - 1; i >= 0; i--) {
        flash_audio_info_t info;
        flash_audio_get_file_info(i, &info);
        bool found = false;
        for (auto &sf : server_files) {
            char sn[FLASH_AUDIO_FILENAME_MAX];
            strncpy(sn, sf.name, sizeof(sn) - 1); sn[sizeof(sn) - 1] = '\0';
            strip_opus(sn);
            if (strcmp(info.name, sn) == 0) { found = true; break; }
        }
        if (!found) { flash_audio_delete_file(info.name); deleted++; }
    }

    // 3. Print comparison result for each file
    int to_dl = 0, to_skip = 0;
    ESP_LOGI(TAG, "--- Compare ---");
    for (int idx = 0; idx < (int)server_files.size(); idx++) {
        char dn[FLASH_AUDIO_FILENAME_MAX];
        strncpy(dn, server_files[idx].name, sizeof(dn) - 1);
        dn[sizeof(dn) - 1] = '\0';
        strip_opus(dn);
        int local_idx = flash_audio_find_file(dn);
        if (local_idx >= 0) {
            flash_audio_info_t info;
            flash_audio_get_file_info(local_idx, &info);
            if (!force_refresh && info.size == server_files[idx].size) {
                to_skip++;
                continue;
            }
            if (info.size == server_files[idx].size) {
                ESP_LOGI(TAG, "  REFRESH %-30s  %s", dn, fmt_size(info.size));
            } else {
                ESP_LOGI(TAG, "  UPDATE  %-30s  %s -> %s",
                         dn, fmt_size(info.size), fmt_size(server_files[idx].size));
            }
        } else {
            ESP_LOGI(TAG, "  NEW     %-30s  %s",
                     dn, fmt_size(server_files[idx].size));
        }
        to_dl++;
    }
    // Show deleted files
    for (int i = 0; i < local_count; i++) {
        flash_audio_info_t info;
        flash_audio_get_file_info(i, &info);
        bool on_server = false;
        for (auto &sf : server_files) {
            char sn[FLASH_AUDIO_FILENAME_MAX];
            strncpy(sn, sf.name, sizeof(sn) - 1); sn[sizeof(sn) - 1] = '\0';
            strip_opus(sn);
            if (strcmp(info.name, sn) == 0) { on_server = true; break; }
        }
        if (!on_server) {
            ESP_LOGI(TAG, "  DELETE  %-30s  %s",
                     info.name, fmt_size(info.size));
        }
    }
    ESP_LOGI(TAG, "--- Result: +%d -%d =%d ---", to_dl, deleted, to_skip);
    if (to_dl == 0 && deleted == 0) {
        ESP_LOGI(TAG, "All up to date, nothing to do");
        return (!force_refresh || save_synced_revision(server_revision))
            ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Download base URL: %s/api/download-idx/<n>?product=%s", g_base_url, SYNC_PRODUCT_ID);

    // 4. Download new/changed files
    int downloaded = 0, skipped = 0, failed = 0;
    int total = (int)server_files.size();

    for (int idx = 0; idx < total; idx++) {
        auto &sf = server_files[idx];
        char dn[FLASH_AUDIO_FILENAME_MAX];
        strncpy(dn, sf.name, sizeof(dn) - 1); dn[sizeof(dn) - 1] = '\0';
        strip_opus(dn);

        int local_idx = flash_audio_find_file(dn);
        if (local_idx >= 0) {
            flash_audio_info_t info;
            flash_audio_get_file_info(local_idx, &info);
            if (!force_refresh && info.size == sf.size) {
                skipped++;
                continue;
            }
            // Equal-size refreshes can safely overwrite in place. A size change
            // must append elsewhere so it cannot overlap the following file.
            if (info.size != sf.size) {
                flash_audio_delete_file(dn);
            }
        }

        // Stream download → flash
        flash_audio_stream_t stream;
        ret = flash_audio_stream_begin(&stream, sf.name, sf.size, 48000, sf.category);
        if (ret != ESP_OK) { failed++; continue; }

        // Download with retry (up to 3 attempts)
        uint32_t expect_size = sf.size;
        ret = download_stream(idx, total, dn, sf.size, &stream);
        int attempt = 1;
        while ((ret != ESP_OK || stream.written < expect_size) && attempt < 3) {
            uint32_t got = stream.written;
            flash_audio_stream_end(&stream);
            int delay_ms = 2000 * attempt;  // 2s, 4s, 6s
            ESP_LOGW(TAG, "[%d/%d] retry %d/3 %s (got %lu/%lu, delay %ds)",
                     idx + 1, total, attempt, dn,
                     (unsigned long)got, (unsigned long)expect_size, delay_ms/1000);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            flash_audio_stream_begin(&stream, sf.name, sf.size, 48000, sf.category);
            ret = download_stream(idx, total, dn, sf.size, &stream);
            attempt++;
        }
        if (ret != ESP_OK || stream.written < expect_size) {
            ESP_LOGW(TAG, "[%d/%d] FAIL  %s (%lu/%lu bytes after %d tries)",
                     idx + 1, total, dn,
                     (unsigned long)stream.written, (unsigned long)expect_size, attempt);
            flash_audio_stream_end(&stream);
            failed++;
            continue;
        }

        ret = flash_audio_stream_end(&stream);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[%d/%d] FAIL  %s (cannot finalize flash entry)",
                     idx + 1, total, dn);
            failed++;
            continue;
        }
        downloaded++;
        ESP_LOGI(TAG, "[%d/%d] OK    %-30s %s",
                 idx + 1, total, dn, fmt_size(sf.size));
    }

    ESP_LOGI(TAG, "--- Sync end: download=%d delete=%d skip=%d fail=%d ---",
             downloaded, deleted, skipped, failed);
    if (failed == 0 && force_refresh &&
        !save_synced_revision(server_revision)) {
        return ESP_FAIL;
    }
    return (failed == 0) ? ESP_OK : ESP_FAIL;
}
