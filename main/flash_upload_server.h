#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register HTTP endpoints for SPI Flash audio management.
 *
 * Adds routes to the existing HTTP server:
 *   GET  /flash            — Web UI for managing flash audio files
 *   GET  /api/flash/status — JSON list of files on flash
 *   POST /api/flash/upload — Upload .opus file (multipart form)
 *   POST /api/flash/erase  — Erase all audio files
 */
void flash_upload_server_register(void);

#ifdef __cplusplus
}
#endif
