idf_component_register(SRCS "wifi_config.cc" "chat.cc" "ws_auth.c" "realtime_ws.c" "audio.cc" "audio_tone.cc" "http_server.cc" "auto_run.cc" "auto_run_data.cc" "wifi.cc" "servo.cc" "power.cc" "main.cc" "flash_audio.cc" "ogg_demuxer.cc" "w25q256.cc" "w25n01gv.c" "flash_upload_server.cc" "sync_audio.cc" "device_registry.cc"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_adc esp_app_format esp_common esp_event esp_hw_support esp_http_client esp_http_server esp_netif esp_wifi esp_websocket_client espressif__cjson freertos heap log lwip mbedtls nvs_flash esp-tls)
