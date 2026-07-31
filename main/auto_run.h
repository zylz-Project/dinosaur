#pragma once

#include "config.h"

#if ENABLE_AUTO_RUN

#include <esp_http_server.h>

void InitAutoRun();
bool IsAutoRunRunning();
void SetAutoRunRunning(bool v);
bool IsAutoRunHardSwing();
void SetAutoRunHardSwing(bool v);
esp_err_t HandleAutoPlay(httpd_req_t *req);

#endif  // ENABLE_AUTO_RUN
