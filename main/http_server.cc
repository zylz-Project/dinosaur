#include "http_server.h"
#include "audio.h"
#include "auto_run.h"
#include "config.h"
#include "dino_samples.h"
#include "flash_audio.h"
#include "flash_upload_server.h"
#include "power.h"
#include "servo.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "dino_http";

httpd_handle_t g_http_server = nullptr;

// ======================== Embedded Web UI ========================
static const char kHtml[] = R"raw(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Dino Controller</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#eee;padding:8px;max-width:600px;margin:auto}
h1{text-align:center;font-size:18px;color:#4ecca3;margin:6px 0}
.card{background:#16213e;border-radius:8px;padding:10px;margin-bottom:8px}
h2{font-size:14px;margin-bottom:5px;color:#ff6b35}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
.col{flex:1;min-width:80px}
.btn{padding:8px 14px;border:none;border-radius:4px;font-size:13px;cursor:pointer;color:#fff;margin:3px}
.btn-set{background:#4ecca3;color:#000}
.btn-reset{background:#333}
input[type=range]{-webkit-appearance:none;width:100%;height:24px;background:linear-gradient(90deg,#0f3460,#e94560);border-radius:4px;margin:2px 0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;background:#e94560;border-radius:50%}
.lbl{display:flex;justify-content:space-between;font-size:11px;color:#aaa}
.val{font-size:16px;font-weight:bold;color:#4ecca3;text-align:center;min-width:36px}
.status{padding:4px;text-align:center;font-size:12px;color:#888;min-height:20px}
.grp-neck{border-left:3px solid #4ecca3}
.grp-head{border-left:3px solid #a34ecc}
.grp-tail{border-left:3px solid #4ea3cc}
</style>
</head>
<body>
<h1>Dino Pet Controller</h1>
<div class="status" id="status">Ready</div>

<div class="card grp-neck">
<h2>Neck 脖子 (IO17: up/down 上下  IO16: left/right 左右)</h2>
<div class="row">
<div class="col">
<div class="lbl"><span>UD (IO17) 上下</span><span class="val" id="v0">70°</span></div>
<input type="range" id="s0" min="0" max="180" value="70" oninput="onSlider()">
<div style="font-size:10px;color:#888;text-align:center">0°=最上 | 90°=中位 | 180°=最下</div>
</div>
</div>
<div class="row">
<div class="col">
<div class="lbl"><span>Lean (IO16) 左右</span><span class="val" id="v1">90°</span></div>
<input type="range" id="s1" min="0" max="180" value="90" oninput="onSlider()">
<div style="font-size:10px;color:#888;text-align:center">0°=右倾 | 90°=中位 | 180°=左倾</div>
</div>
</div>
</div>

<div class="card grp-head">
<h2>Head 头部 (IO15: turn 转头)</h2>
<div class="row">
<div class="col">
<div class="lbl"><span>Turn (IO15) 转头</span><span class="val" id="v2">90°</span></div>
<input type="range" id="s2" min="0" max="180" value="90" oninput="onSlider()">
<div style="font-size:10px;color:#888;text-align:center">0°=右转 | 90°=中位 | 180°=左转</div>
</div>
</div>
</div>

<div class="card grp-tail">
<h2>Tail 尾巴 (IO18: left/right 左右  IO8: up/down 上下)</h2>
<div class="row">
<div class="col">
<div class="lbl"><span>UD (IO8) 上下</span><span class="val" id="v3">90°</span></div>
<input type="range" id="s3" min="55" max="180" value="90" oninput="onSlider()">
<div style="font-size:10px;color:#888;text-align:center">55°=最下 | 90°=中位 | 180°=最上</div>
</div>
</div>
<div class="row">
<div class="col">
<div class="lbl"><span>LR (IO18) 左右</span><span class="val" id="v4">90°</span></div>
<input type="range" id="s4" min="0" max="180" value="90" oninput="onSlider()">
<div style="font-size:10px;color:#888;text-align:center">0°=最左 | 90°=中位 | 180°=最右</div>
</div>
</div>
</div>

<div class="card">
<h2>Quick Presets</h2>
<div class="row">
<button class="btn btn-reset" onclick="preset(70,90,90,90,90)">默认抬颈</button>
<button class="btn btn-reset" onclick="preset(90,90,90,90,90)">全部中位</button>
<button class="btn btn-reset" onclick="preset(0,90,90,180,90)">仰天长啸</button>
<button class="btn btn-reset" onclick="preset(180,90,90,90,90)">低头吃东西</button>
<button class="btn btn-reset" onclick="preset(90,45,90,90,90)">脖子右倾</button>
<button class="btn btn-reset" onclick="preset(90,135,90,90,90)">脖子左倾</button>
</div>
<div class="row" style="margin-top:3px">
<button class="btn btn-reset" onclick="preset(90,90,45,90,90)">头右转</button>
<button class="btn btn-reset" onclick="preset(90,90,135,90,90)">头左转</button>
<button class="btn btn-reset" onclick="preset(90,90,90,180,90)">尾巴上翘</button>
<button class="btn btn-reset" onclick="preset(90,90,90,55,90)">尾巴下垂</button>
<button class="btn btn-reset" onclick="preset(90,90,90,90,0)">尾巴最左</button>
<button class="btn btn-reset" onclick="preset(90,90,90,90,180)">尾巴最右</button>
</div>
</div>

<div class="card">
<h2>萌宠动作库（点击即可试演）</h2>
<div class="row">
<button class="btn btn-set" onclick="action(0)">发现你了</button>
<button class="btn btn-set" onclick="action(1)">贴贴撒娇</button>
<button class="btn btn-set" onclick="action(2)">雀跃开心</button>
<button class="btn btn-set" onclick="action(3)">威风鸣叫</button>
</div>
<div class="row">
<button class="btn btn-reset" onclick="action(4)">低头进食</button>
<button class="btn btn-reset" onclick="action(5)">听声定位</button>
<button class="btn btn-reset" onclick="action(6)">受惊恢复</button>
<button class="btn btn-reset" onclick="action(7)">困倦入睡</button>
</div>
</div>

<div class="card">
<h2>Battery & Info</h2>
<div id="batt" style="font-size:14px;color:#4ecca3;text-align:center">Loading...</div>
</div>

<script>
let lastSend = 0;
function $(id){return document.getElementById(id)}
function setStatus(s,c){$('status').textContent=s;if(c)$('status').style.color=c}

async function api(url, body){
  try{
    let o = body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {method:'GET'};
    let r = await fetch(url, o);
    return await r.json();
  }catch(e){ setStatus('Connection lost','#e94560'); return null; }
}

function onSlider(){
  for(let i=0;i<5;i++) $('v'+i).textContent=$('s'+i).value+'°';
  let now=Date.now();
  if(now-lastSend>40){ lastSend=now; sendAngles(); }
}

function preset(a0,a1,a2,a3,a4){
  let vals=[a0,a1,a2,a3,a4];
  for(let i=0;i<5;i++){ $('s'+i).value=vals[i]; $('v'+i).textContent=vals[i]+'°'; }
  sendAngles();
}

async function sendAngles(){
  let a=[];
  for(let i=0;i<5;i++) a.push(parseInt($('s'+i).value));
  await api('/api/servo', {angles:a});
}

async function action(id){
  let r=await api('/api/action',{action:id});
  if(r&&r.ok)setStatus('Action queued: '+id,'#4ecca3');
}

// Poll battery every 5s
setInterval(async ()=>{
  let r = await api('/api/battery');
  if(r) $('batt').textContent = 'Battery: ' + r.voltage_mv + 'mV | Level: ' + r.level + '%';
}, 5000);

// Initial battery fetch
(async ()=>{let r=await api('/api/battery');if(r)$('batt').textContent='Battery: '+r.voltage_mv+'mV | Level: '+r.level+'%';})();
</script>
</body>
</html>
)raw";

// ======================== HTTP Handlers ========================
static esp_err_t HandleRoot(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, kHtml, strlen(kHtml));
  return ESP_OK;
}

static esp_err_t HandleServo(httpd_req_t *req)
{
  char buf[512] = {};
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  buf[ret] = 0;

  int angles[5] = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                   SERVO_HEAD_TURN_DEFAULT, SERVO_TAIL_UD_DEFAULT,
                   SERVO_TAIL_LR_DEFAULT};
  const char *p = strstr(buf, "\"angles\":");
  if (p) {
    p += 9;
    for (int i = 0; i < 5; i++) {
      while (*p == ' ' || *p == '[' || *p == ',') p++;
      angles[i] = atoi(p);
      while (*p && *p != ',' && *p != ']') p++;
    }
  }
  for (int i = 0; i < 5; i++)
    SetServoAngle(i, angles[i]);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

static esp_err_t HandleBattery(httpd_req_t *req)
{
  char resp[128];
  snprintf(resp, sizeof(resp), "{\"voltage_mv\":%d,\"level\":%d}",
           GetBatteryVoltageMv(), GetBatteryLevel());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

#if ENABLE_AUTO_RUN
static esp_err_t HandleAction(httpd_req_t *req)
{
    char buf[96] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[received] = 0;
    const char *p = strstr(buf, "\"action\":");
    int action = p ? atoi(p + 9) : -1;
    bool ok = action >= 0 && action < DINO_ACTION_COUNT;
    if (ok) {
        SetAutoRunRunning(true);
        ok = TriggerDinoActionWithAutoSound(static_cast<DinoAction>(action));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t HandleAutoPlay(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char buf[64] = {};
        httpd_req_recv(req, buf, sizeof(buf) - 1);
        const char *p = strstr(buf, "\"enable\":");
        if (p) { p += 9; SetAutoRunRunning(atoi(p) != 0); }
        else if (!strstr(buf, "hard_swing"))
            SetAutoRunRunning(!IsAutoRunRunning());
        p = strstr(buf, "\"hard_swing\":");
        if (p) { p += 13; SetAutoRunHardSwing(strncmp(p, "true", 4) == 0 || atoi(p) == 1); }
    }
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"autoplay\":%s,\"hard_swing\":%s}",
             IsAutoRunRunning() ? "true" : "false",
             IsAutoRunHardSwing() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
#endif

void StartHttpServer()
{
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 20;
  httpd_start(&g_http_server, &cfg);

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &root);

  httpd_uri_t servo = {.uri = "/api/servo", .method = HTTP_POST, .handler = HandleServo, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &servo);

  httpd_uri_t batt = {.uri = "/api/battery", .method = HTTP_GET, .handler = HandleBattery, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &batt);

#if ENABLE_AUTO_RUN
  httpd_uri_t action = {.uri = "/api/action", .method = HTTP_POST, .handler = HandleAction, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &action);

  httpd_uri_t auto_play = {.uri = "/api/autoplay", .method = HTTP_GET, .handler = HandleAutoPlay, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &auto_play);
  {
    httpd_uri_t post_auto = {.uri = "/api/autoplay", .method = HTTP_POST, .handler = HandleAutoPlay, .user_ctx = nullptr};
    httpd_register_uri_handler(g_http_server, &post_auto);
  }
#endif

  // Register flash upload endpoints
  flash_upload_server_register();

  ESP_LOGI(TAG, "HTTP server started");
}
