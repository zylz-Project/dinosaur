#include "flash_upload_server.h"
#include "flash_audio.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <esp_heap_caps.h>

static const char *TAG = "flash_upload";

// External reference to the HTTP server handle (from http_server.cc)
extern httpd_handle_t g_http_server;

// === Flash management web page ===
static const char kFlashHtml[] = R"raw(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flash Audio Manager</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#eee;padding:12px;max-width:600px;margin:auto}
h1{text-align:center;font-size:18px;color:#e94560;margin:8px 0}
.card{background:#16213e;border-radius:8px;padding:12px;margin-bottom:10px}
h2{font-size:14px;margin-bottom:6px;color:#4ecca3}
.btn{padding:8px 16px;border:none;border-radius:4px;font-size:13px;cursor:pointer;color:#fff;margin:4px}
.btn-upload{background:#4ecca3;color:#000}
.btn-erase{background:#e94560}
.btn-refresh{background:#333}
table{width:100%;border-collapse:collapse;font-size:11px}
th,td{padding:4px 6px;text-align:left;border-bottom:1px solid #333}
th{color:#4ecca3}
.status{padding:6px;text-align:center;font-size:12px;color:#888}
input[type=file]{margin:6px 0;color:#eee}
.progress{width:100%;height:6px;background:#333;border-radius:3px;margin:4px 0;display:none}
.progress div{height:100%;background:#4ecca3;border-radius:3px;width:0%}
</style>
</head>
<body>
<h1>Flash Audio Manager</h1>
<div class="status" id="status">Ready</div>

<div class="card">
<h2>Upload .opus file</h2>
<input type="file" id="fileInput" accept=".opus">
<button class="btn btn-upload" onclick="uploadFile()">Upload to Flash</button>
<div class="progress" id="progress"><div id="progressBar"></div></div>
</div>

<div class="card">
<h2>Files on Flash <button class="btn btn-refresh" onclick="refreshStatus()">Refresh</button></h2>
<div id="fileList">Loading...</div>
</div>

<div class="card">
<h2>Danger Zone</h2>
<button class="btn btn-erase" onclick="eraseAll()">Erase All Audio</button>
</div>

<script>
async function api(url, body) {
    try{
        let o = body ? {method:'POST', body:body} : {method:'GET'};
        let r = await fetch(url, o);
        return await r.text();
    }catch(e){ return null; }
}

async function refreshStatus() {
    let txt = await api('/api/flash/status');
    if(!txt){ document.getElementById('fileList').innerHTML='<p style="color:#e94560">Connection failed</p>'; return; }
    try{
        let j = JSON.parse(txt);
        if(j.count==0){
            document.getElementById('fileList').innerHTML='<p style="color:#888">No files on flash</p>';
        } else {
            let html='<table><tr><th>#</th><th>Filename</th><th>Size</th><th>Rate</th></tr>';
            j.files.forEach((f,i)=>{
                let kb = (f.size/1024).toFixed(1);
                html+='<tr><td>'+i+'</td><td>'+f.name+'</td><td>'+kb+' KB</td><td>'+f.sample_rate+' Hz</td></tr>';
            });
            html+='</table>';
            html+='<p style="margin-top:6px;color:#888">Total: '+j.count+' files, '+(j.total_size/1024).toFixed(1)+' KB / 32 MB</p>';
            document.getElementById('fileList').innerHTML=html;
        }
    }catch(e){
        document.getElementById('fileList').innerHTML='<p style="color:#e94560">Parse error</p>';
    }
}

async function uploadFile() {
    let f = document.getElementById('fileInput').files[0];
    if(!f){ setStatus('Select a file first','#e94560'); return; }
    if(!f.name.endsWith('.opus')){ setStatus('Only .opus files allowed','#e94560'); return; }

    setStatus('Uploading: '+f.name+'...','#4ecca3');
    document.getElementById('progress').style.display='block';

    let form = new FormData();
    form.append('file', f);

    let xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/flash/upload');
    xhr.upload.onprogress = function(e) {
        if(e.lengthComputable){
            let pct = (e.loaded/e.total*100).toFixed(0);
            document.getElementById('progressBar').style.width = pct+'%';
        }
    };
    xhr.onload = function() {
        document.getElementById('progress').style.display='none';
        document.getElementById('progressBar').style.width='0%';
        if(xhr.status==200){
            setStatus('Uploaded: '+f.name+' ✓','#4ecca3');
            refreshStatus();
        } else {
            setStatus('Upload failed: '+xhr.responseText,'#e94560');
        }
    };
    xhr.onerror = function() {
        document.getElementById('progress').style.display='none';
        setStatus('Network error','#e94560');
    };
    xhr.send(form);
}

async function eraseAll() {
    if(!confirm('Erase ALL audio files from flash? This cannot be undone.')) return;
    setStatus('Erasing...','#e94560');
    let txt = await api('/api/flash/erase');
    if(txt && txt.startsWith('OK')){
        setStatus('All files erased ✓','#4ecca3');
        refreshStatus();
    } else {
        setStatus('Erase failed','#e94560');
    }
}

function setStatus(msg, color) {
    let s = document.getElementById('status');
    s.textContent = msg;
    s.style.color = color;
}

refreshStatus();
</script>
</body>
</html>
)raw";

/* ==========================================================================
   HTTP Handlers
   ========================================================================== */

static esp_err_t HandleFlashPage(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kFlashHtml, strlen(kFlashHtml));
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
                                            data_len, 48000);

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

static const char kAudioHtml[] = R"raw(
<!DOCTYPE html>
<html lang="zh">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>音频验证</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#eee;padding:12px;max-width:600px;margin:auto}
h1{text-align:center;font-size:18px;color:#e94560;margin:8px 0}
.card{background:#16213e;border-radius:8px;padding:12px;margin-bottom:10px}
h2{font-size:14px;margin-bottom:6px;color:#4ecca3}
.file{display:flex;align-items:center;gap:10px;padding:8px;border-bottom:1px solid #333;flex-wrap:wrap}
.fname{flex:1;font-size:12px;min-width:120px}
.fsize{font-size:10px;color:#888;min-width:60px}
audio{width:200px;height:32px}
.status{padding:6px;text-align:center;font-size:12px;color:#888}
</style>
</head>
<body>
<h1>音频验证</h1>
<div class="card">
<h2>SPI Flash 中的文件</h2>
<div id="files">加载中...</div>
</div>
<div class="status" id="info"></div>
<script>
async function loadFiles(){
 let r=await fetch('/api/flash/status'),d=await r.json(),h='';
 d.files.forEach((f,i)=>{h+='<div class="file"><span class="fname">'+f.name+'</span><span class="fsize">'+(f.size/1024).toFixed(1)+' KB</span><audio controls preload="none"><source src="/play?idx='+i+'" type="audio/ogg"></audio></div>'});
 document.getElementById('files').innerHTML=h||'<p style="color:#888;text-align:center">没有文件</p>';
}
loadFiles();
</script>
</body>
</html>
)raw";

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
    httpd_resp_send(req, kAudioHtml, strlen(kAudioHtml));
    return ESP_OK;
}

void flash_upload_server_register(void) {
    if (!g_http_server) {
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
        httpd_register_uri_handler(g_http_server, &u);
    }

    ESP_LOGI(TAG, "Flash endpoints: /, /flash, /play, /api/flash/*");
}
