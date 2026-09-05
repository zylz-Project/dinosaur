/*
 * flash_upload_server.cc — Flash 音频管理页/上传/擦除 API 实现（见 flash_upload_server.h）
 */
#include "flash_upload_server.h"
#include "flash_audio.h"
#include "audio.h"
#include "http_server.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <esp_heap_caps.h>

static const char *TAG = "flash_upload";

// === Flash management web page ===
// 管理页/播放页 HTML 在 web_assets/flash.html、audio.html，EMBED_FILES 链入。
extern const unsigned char flash_html_start[] asm("_binary_flash_html_start");
extern const unsigned char flash_html_end[]   asm("_binary_flash_html_end");
#define kFlashHtml ((const char *)flash_html_start)
#define kFlashHtmlLen ((size_t)(flash_html_end - flash_html_start))

/* ==========================================================================
   HTTP Handlers
   ========================================================================== */

static esp_err_t HandleFlashPage(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kFlashHtml, kFlashHtmlLen);
    return ESP_OK;
}

static void EscapeJsonString(const char *src, char *dst, size_t dst_size) {
    if (!dst_size) return;
    size_t out = 0;
    while (src && *src && out + 1 < dst_size) {
        const unsigned char ch = (unsigned char)*src++;
        if ((ch == '"' || ch == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = (char)ch;
        } else if (ch >= 0x20) {
            dst[out++] = (char)ch;
        }
    }
    dst[out] = '\0';
}

static esp_err_t HandleFlashStatus(httpd_req_t *req) {
    // CORS header — allow browser to query directly
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    const int count = flash_audio_get_file_count();
    if (count < 0 || count > FLASH_AUDIO_MAX_FILES) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Invalid flash file count");
        return ESP_FAIL;
    }
    uint32_t total_size = 0;

    httpd_resp_set_type(req, "application/json");
    char chunk[256];
    int len = snprintf(chunk, sizeof(chunk), "{\"count\":%d,\"files\":[", count);
    if (len < 0 || (size_t)len >= sizeof(chunk) ||
        httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
        return ESP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        flash_audio_info_t info = {};
        if (flash_audio_get_file_info(i, &info) != ESP_OK) return ESP_FAIL;
        total_size += info.size;
        char escaped_name[FLASH_AUDIO_FILENAME_MAX * 2 + 1];
        EscapeJsonString(info.name, escaped_name, sizeof(escaped_name));
        len = snprintf(chunk, sizeof(chunk),
                       R"(%s{"name":"%s","size":%lu,"sample_rate":%lu})",
                       i ? "," : "", escaped_name,
                       (unsigned long)info.size, (unsigned long)info.sample_rate);
        if (len < 0 || (size_t)len >= sizeof(chunk) ||
            httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    len = snprintf(chunk, sizeof(chunk), "],\"total_size\":%lu}",
                   (unsigned long)total_size);
    if (len < 0 || (size_t)len >= sizeof(chunk) ||
        httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t HandleFlashUpload(httpd_req_t *req) {
    // 写 flash 会擦除/改写 TOC —— 播放中操作会让读音端读到半新半旧的表，先拒绝
    if (IsAudioPlaying()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "正在播放音频，请先停止后再上传");
        return ESP_FAIL;
    }

    char content_type[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                      sizeof(content_type)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type");
        return ESP_FAIL;
    }

    // Read the full body (simple approach: read in chunks)
    static uint8_t upload_buf[65536];  // 64KB max per upload
    int total = 0;
    int ret;

    while (total < (int)sizeof(upload_buf) - 1) {
        ret = httpd_req_recv(req, (char *)(upload_buf + total),
                              sizeof(upload_buf) - total - 1);
        if (ret <= 0) break;
        total += ret;
    }

    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    // Simple multipart parser: find filename and data boundary
    // Find boundary
    const char *boundary = strstr(content_type, "boundary=");
    if (!boundary) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary");
        return ESP_FAIL;
    }
    boundary += 9;

    // Find filename in multipart headers
    const char *fname_start = strstr((char *)upload_buf, "filename=\"");
    if (!fname_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }
    fname_start += 10;
    const char *fname_end = strchr(fname_start, '"');
    if (!fname_end) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad filename");
        return ESP_FAIL;
    }
    char filename[64] = {};
    strncpy(filename, fname_start,
            std::min((size_t)(fname_end - fname_start), sizeof(filename) - 1));

    // Find start of file data (after \r\n\r\n)
    const char *data_start = strstr(fname_end, "\r\n\r\n");
    if (!data_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    data_start += 4;

    // Find end boundary
    char boundary_marker[72];
    snprintf(boundary_marker, sizeof(boundary_marker), "\r\n--%s", boundary);
    const char *data_end = strstr(data_start, boundary_marker);
    if (!data_end) {
        // Try without leading \r\n
        snprintf(boundary_marker, sizeof(boundary_marker), "--%s", boundary);
        data_end = strstr(data_start, boundary_marker);
    }
    if (!data_end) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No end boundary");
        return ESP_FAIL;
    }

    size_t data_len = data_end - data_start;
    // Trim trailing \r\n
    while (data_len > 0 && (data_start[data_len - 1] == '\r' || data_start[data_len - 1] == '\n'))
        data_len--;

    if (data_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zero-length data");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload: %s (%zu bytes)", filename, data_len);

    // Write to SPI Flash
    esp_err_t err = flash_audio_write_file(filename, (const uint8_t *)data_start,
                                            data_len, 48000, "animal");

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Flash write failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t HandleFlashErase(httpd_req_t *req) {
    // 擦除会清掉所有音频数据 —— 播放中擦除会让解码端读空，先拒绝
    if (IsAudioPlaying()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "正在播放音频，请先停止后再擦除");
        return ESP_FAIL;
    }

    esp_err_t ret = flash_audio_erase_all();
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ==========================================================================
   Audio player — serve HTML page + stream raw .opus files from SPI Flash
   GET /play         →  HTML player page (lists all files)
   GET /play?idx=0   →  raw .opus file #0 (streamed to browser audio element)
   ========================================================================== */

extern const unsigned char audio_html_start[] asm("_binary_audio_html_start");
extern const unsigned char audio_html_end[]   asm("_binary_audio_html_end");
#define kAudioHtml ((const char *)audio_html_start)
#define kAudioHtmlLen ((size_t)(audio_html_end - audio_html_start))

/* Handle /play → HTML, /play?idx=N → raw opus stream */
static esp_err_t HandlePlay(httpd_req_t *req) {
    // If query param "idx=N" present → stream file #N
    const char *q = strchr(req->uri, '?');
    if (q && strstr(q, "idx=")) {
        int idx = atoi(strstr(q, "idx=") + 4);

        flash_audio_info_t info;
        if (flash_audio_get_file_info(idx, &info) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "audio/ogg");

        // Set Content-Length so browser can determine OGG duration (stored in last page)
        char cl_hdr[32];
        snprintf(cl_hdr, sizeof(cl_hdr), "%lu", (unsigned long)info.size);
        httpd_resp_set_hdr(req, "Content-Length", cl_hdr);

        // Stream file in chunks
        uint8_t buf[2048];
        uint32_t offset = 0;
        while (offset < info.size) {
            size_t n = info.size - offset;
            if (n > sizeof(buf)) n = sizeof(buf);
            if (flash_audio_read_file(idx, offset, buf, n) != ESP_OK) break;
            if (httpd_resp_send_chunk(req, (char *)buf, n) != ESP_OK) break;
            offset += n;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    // /play → HTML page
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kAudioHtml, kAudioHtmlLen);
    return ESP_OK;
}

void flash_upload_server_register(void) {
    httpd_handle_t server = HttpServerHandle();
    if (!server) {
        ESP_LOGW(TAG, "HTTP server not started, flash upload endpoints skipped");
        return;
    }

    httpd_uri_t uris[] = {
        {"/flash",   HTTP_GET, HandleFlashPage,     nullptr},
        {"/play",    HTTP_GET, HandlePlay,          nullptr},
        {"/api/flash/status", HTTP_GET,  HandleFlashStatus, nullptr},
        {"/api/flash/upload", HTTP_POST, HandleFlashUpload, nullptr},
        {"/api/flash/erase",  HTTP_POST, HandleFlashErase,  nullptr},
    };
    for (auto &u : uris) {
        httpd_register_uri_handler(server, &u);
    }

    ESP_LOGI(TAG, "Flash endpoints: /, /flash, /play, /api/flash/*");
}
