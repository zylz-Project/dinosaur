#pragma once

#include <esp_http_server.h>

extern httpd_handle_t g_http_server;

void StartHttpServer();
