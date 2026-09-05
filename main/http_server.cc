/*
 * http_server.cc — Web 面板与本地 API 实现
 *
 * 路由：/ 控制面板页（内嵌 HTML）；/api/servo 舵机滑条、/api/action 动作、
 * /api/autoplay 开关、/api/battery 电量、/api/wifi* 配网扫描/连接、
 * /api/chat* 对话开关。404 全部 302 回首页（配合 DNS 劫持 captive portal）。
 * 对话进行中拒绝动作/滑条请求（舵机归 chat_motion 管）。
 * Flash 管理页与上传 API 在 flash_upload_server.cc。
 */
#include "http_server.h"
#include "audio.h"
#include "auto_run.h"
#include "chat.h"
#include "config.h"
#include "flash_audio.h"
#include "flash_upload_server.h"
#include "power.h"
#include "servo.h"
#include "wifi.h"
#include "wifi_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
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

<!-- WIFI -->
<div class="card" id="wifiCard">
<h2>WiFi</h2>
<!-- 当前状态 (badge + 详情) -->
<div class="row" style="justify-content:space-between;align-items:center;margin-bottom:6px">
<span id="wifiBadge" style="font-size:12px;font-weight:bold;padding:4px 12px;border-radius:4px;color:#000;background:#555">--</span>
<button type="button" class="btn btn-set" id="btnReconfig" onclick="enterPortal()" style="display:none">重新配网</button>
</div>
<div id="wifiDetail" style="font-size:12px;color:#aaa;margin-bottom:6px">--</div>
<!-- 未连接/配网: 显示完整表单 -->
<div id="wifiForm" style="display:none">
<button type="button" class="btn btn-set" style="width:100%;margin-top:6px" onclick="scanWifi()">扫描附近 WiFi</button>
<div id="wifiList" style="margin-top:6px;min-height:24px"></div>
<div class="row" style="margin-top:6px">
<div class="col" style="flex:1"><input id="wSsid" placeholder="WiFi SSID" style="width:100%"></div>
</div>
<div class="row">
<div class="col" style="flex:1"><input id="wPass" placeholder="WiFi 密码" type="text" style="width:100%"></div>
</div>
<div class="row">
<button type="button" class="btn btn-set" style="width:100%" onclick="saveWifi()">连接此 WiFi</button>
</div>
</div>
</div>

<!-- CHAT -->
<div class="card">
<h2>AI 对话 <span style="font-size:10px;color:#888">双击电源键切换</span></h2>
<div class="row" style="justify-content:space-between">
<span style="font-size:12px;color:#aaa" id="chatStatus">--</span>
<button type="button" class="btn btn-set" onclick="toggleChat()">开/关对话</button>
</div>
</div>

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

// --- WiFi ---
async function apiTxt(url, body){
  try{
    let o = body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {method:'GET'};
    let r = await fetch(url, o);
    return await r.text();
  }catch(e){ setStatus('连接失败','#e94560'); return null; }
}
function wifiShowForm(show){
  $('wifiForm').style.display = show ? 'block' : 'none';
  $('btnReconfig').style.display = show ? 'none' : 'inline-block';
}
async function updateWifiInfo(){
  let r = await api('/api/wifi');
  if(!r) return;
  let badge = $('wifiBadge'), det = $('wifiDetail');
  if(r.mode === 'connected'){
    badge.textContent = '已连接';
    badge.style.background = '#2ecc71';
    det.textContent = 'SSID: ' + r.ssid + '   |   IP: ' + r.ip +
                      (r.rssi !== undefined && r.rssi !== null ? '   |   信号: ' + r.rssi + 'dBm' : '');
    wifiShowForm(false);
  } else if(r.mode === 'portal'){
    badge.textContent = '配网模式';
    badge.style.background = '#f39c12';
    det.textContent = '热点 Dino-XXXX 已开启: 手机连接后访问 192.168.4.1';
    wifiShowForm(true);
  } else {
    badge.textContent = '未连接';
    badge.style.background = '#e94560';
    det.textContent = '已配置: ' + (r.saved_ssid || '无') + '   |   请选择 WiFi 重新连接';
    wifiShowForm(true);
  }
}
async function enterPortal(){
  setStatus('正在进入配网模式...','#3498db');
  await apiTxt('/api/wifi/portal', {});
  setStatus('配网模式已开启：手机连接 Dino-XXXX 热点，访问 192.168.4.1', '#3498db');
  await updateWifiInfo();
}
async function scanWifi(){
  setStatus('正在扫描附近 WiFi...','#4ecca3');
  $('wifiList').innerHTML = '<div style="color:#888;font-size:12px;padding:4px">扫描中，请稍候...</div>';
  let r = await api('/api/wifi/scan');
  setStatus('扫描完成','#4ecca3');
  if(!r){ $('wifiList').innerHTML='<div style="color:#e94560;font-size:12px">扫描失败，请重试</div>'; return; }
  if(!r.length){ $('wifiList').innerHTML='<div style="color:#888;font-size:12px">未发现 WiFi</div>'; return; }
  let html='';
  for(let a of r){
    if(!a.ssid) continue;
    let pad = a.rssi>=-65?'#2ecc71':(a.rssi>=-80?'#f39c12':'#e94560');
    html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 4px;border-bottom:1px solid #1a2a4a;font-size:13px" onclick="pickWifi(\''+a.ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">'+
            '<span>'+a.ssid+'</span><span style="color:'+pad+'">'+a.rssi+'dBm</span></div>';
  }
  $('wifiList').innerHTML = html;
  setStatus('点击列表选择 WiFi','#4ecca3');
}
function pickWifi(ssid){ $('wSsid').value = ssid; $('wPass').focus(); }
async function saveWifi(){
  let ssid = $('wSsid').value.trim();
  let pass = $('wPass').value.trim();
  if(!ssid){ setStatus('请输入 SSID','#e94560'); $('wSsid').focus(); return; }
  setStatus('正在保存并连接 '+ssid+'...','#4ecca3');
  await apiTxt('/api/wifi/configure', {ssid:ssid, password:pass});
  setStatus('已保存！正在连接新网络，热点稍后自动关闭...', '#4ecca3');
  setTimeout(updateWifiInfo, 2500);
}

// --- Chat ---
async function updateChat(){
  let r = await api('/api/chat');
  if(r){
    $('chatStatus').textContent = r.active ? (r.ready ? '对话中 (服务端就绪)' : '对话中 (连接中...)') : '已关闭';
    $('chatStatus').style.color = r.ready ? '#2ecc71' : (r.active ? '#f39c12' : '#888');
  }
}
async function toggleChat(){
  await apiTxt('/api/chat/toggle', {});
  await updateChat();
}
setInterval(updateChat, 3000);
updateChat();

(async function(){ await updateWifiInfo(); })();
// 每 3 秒刷新 WiFi 当前状态
setInterval(updateWifiInfo, 3000);
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
  // 聊天进行中时 chat_motion 任务持有舵机（情绪驱动）——这里复活写舵机会打架
  if (ChatIsActive()) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\"}");
    return ESP_OK;
  }

  char buf[512] = {};
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  buf[ret] = 0;

  int angles[5] = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                   SERVO_HEAD_TURN_DEFAULT, SERVO_TAIL_UD_DEFAULT,
                   SERVO_TAIL_LR_DEFAULT};
  cJSON *root = cJSON_Parse(buf);
  if (root) {
    const cJSON *arr = cJSON_GetObjectItem(root, "angles");
    if (cJSON_IsArray(arr)) {
      for (int i = 0; i < 5 && i < cJSON_GetArraySize(arr); i++)
        angles[i] = cJSON_GetArrayItem(arr, i)->valueint;
    }
    cJSON_Delete(root);
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
    // 同 HandleServo：聊天中不允许手动触发动作，避免与 chat_motion 抢舵机
    if (ChatIsActive()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\"}");
        return ESP_OK;
    }

    char buf[96] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[received] = 0;
    int action = -1;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
      action = cJSON_GetObjectItem(root, "action")->valueint;
      cJSON_Delete(root);
    }
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
    // POST 会复活 auto_run 任务（SetAutoRunRunning(true)）——聊天中拒绝；
    // GET 只读状态，放行
    if (req->method == HTTP_POST && ChatIsActive()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"chat_active\",\"autoplay\":false}");
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        char buf[64] = {};
        httpd_req_recv(req, buf, sizeof(buf) - 1);
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *enable = cJSON_GetObjectItem(root, "enable");
            const cJSON *hard = cJSON_GetObjectItem(root, "hard_swing");
            if (enable) SetAutoRunRunning(cJSON_IsTrue(enable));
            else if (!hard) SetAutoRunRunning(!IsAutoRunRunning());
            if (hard) SetAutoRunHardSwing(cJSON_IsTrue(hard));
            cJSON_Delete(root);
        }
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

static esp_err_t HandleWifiStatus(httpd_req_t *req)
{
    char buf[320];
    bool online = (strcmp(WiFiIP(), "0.0.0.0") != 0);
    bool portal = WifiConfigPortalRunning();

    char cur_ssid[33] = {};
    int rssi = -1;
    if (online) WifiConnectedApInfo(cur_ssid, sizeof(cur_ssid), &rssi);

    char saved_ssid[33] = {};
    {
        char p[65] = {};
        if (WifiConfigGetCredentials(saved_ssid, sizeof(saved_ssid), p, sizeof(p)) && saved_ssid[0]) {
            /* NVS 已保存的实际凭据 */
        }
#ifdef WIFI_STA_SSID
        else strlcpy(saved_ssid, WIFI_STA_SSID, sizeof(saved_ssid));
#else
        else strlcpy(saved_ssid, "none", sizeof(saved_ssid));
#endif
    }

    const char *mode = online ? "connected" : (portal ? "portal" : "offline");
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"mode\":\"%s\",\"ssid\":\"%s\",\"saved_ssid\":\"%s\","
             "\"ip\":\"%s\",\"rssi\":%d,\"portal\":%s}",
             online ? "true" : "false", mode, cur_ssid, saved_ssid,
             WiFiIP(), rssi, portal ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiPortal(httpd_req_t *req)
{
    httpd_req_recv(req, nullptr, 0); /* discard body */
    WifiConfigEnterFromWeb();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "portal started");
    return ESP_OK;
}

static esp_err_t HandleWifiScan(httpd_req_t *req)
{
    char buf[1200];
    WifiConfigScanAps(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiConfigure(httpd_req_t *req)
{
    char body[512] = {};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = 0;

    char ssid[33] = {}, pass[65] = {};
    cJSON *root = cJSON_Parse(body);
    if (root) {
        const cJSON *s = cJSON_GetObjectItem(root, "ssid");
        const cJSON *p = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(s)) {
            strncpy(ssid, s->valuestring, sizeof(ssid) - 1);
        }
        if (cJSON_IsString(p)) {
            strncpy(pass, p->valuestring, sizeof(pass) - 1);
        }
        cJSON_Delete(root);
    }
    if (!ssid[0]) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "no ssid"); return ESP_OK; }

    WifiConfigSaveCredentials(ssid, pass);

    /* 先回响应, 让手机收到"已保存"再关热点, 避免页面加载中断乱跳 */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");

    WifiConfigStopPortal();  /* 保存后切回 STA 连接 */
    return ESP_OK;
}

static esp_err_t HandleChatStatus(httpd_req_t *req)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"active\":%s,\"ready\":%s}",
             ChatIsActive() ? "true" : "false",
             ChatIsReady() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleChatToggle(httpd_req_t *req)
{
    httpd_req_recv(req, nullptr, 0); /* discard body */
    ChatToggle();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

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

  httpd_uri_t wifi_status = {.uri = "/api/wifi", .method = HTTP_GET, .handler = HandleWifiStatus, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_status);
  httpd_uri_t wifi_portal = {.uri = "/api/wifi/portal", .method = HTTP_POST, .handler = HandleWifiPortal, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_portal);
  httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = HandleWifiScan, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_scan);
  httpd_uri_t wifi_cfg = {.uri = "/api/wifi/configure", .method = HTTP_POST, .handler = HandleWifiConfigure, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &wifi_cfg);

  httpd_uri_t chat_status = {.uri = "/api/chat", .method = HTTP_GET, .handler = HandleChatStatus, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &chat_status);
  httpd_uri_t chat_toggle = {.uri = "/api/chat/toggle", .method = HTTP_POST, .handler = HandleChatToggle, .user_ctx = nullptr};
  httpd_register_uri_handler(g_http_server, &chat_toggle);

  /* 全局 404 → 重定向到首页: 配合 DNS 劫持实现 captive portal,
   * 手机连上 Dino-XXXX 后系统探测任何 URL 都会落到首页, 触发"登录网络"弹窗 */
  httpd_register_err_handler(g_http_server, HTTPD_404_NOT_FOUND, [](httpd_req_t *req, httpd_err_code_t err) -> esp_err_t {
      httpd_resp_set_status(req, "302 Found");
      httpd_resp_set_hdr(req, "Location", "/");
      httpd_resp_sendstr(req, "<html><body>redirecting...</body></html>");
      return ESP_OK;
  });

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
