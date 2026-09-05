/*
 * http_server.h — Web 控制面板与设备本地 API（httpd 封装）
 *
 * 职责：StartHttpServer 启动 esp_http_server，注册首页和
 * /api/ 系列路由（舵机滑条、动作按钮、电池、WiFi 配网、对话开关）。
 * 网页 HTML 内嵌在 http_server.cc；Flash 音频管理的独立页面/路由在
 * flash_upload_server.cc（启动时向本模块拿 server 句柄注册）。
 * g_http_server 仅本模块与注册方使用，外部请走 HttpServerHandle()。
 */
#pragma once

#include <esp_http_server.h>

extern httpd_handle_t g_http_server;

void StartHttpServer();

/** 供 flash_upload_server 等模块拿句柄注册自己的路由（NULL=还没启动）。 */
httpd_handle_t HttpServerHandle(void);
