# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 firmware for a dinosaur pet toy: 5-servo motion engine, Opus audio streamed from external SPI flash, an LLM realtime voice chat client (WSS), and a web control panel. ESP-IDF **v5.5** project (target `esp32s3`, 16MB flash, PSRAM N16R8). Comments, log messages, and docs are largely in Chinese — match that style.

## Build & Flash

```bash
source /home/wzh/.espressif/v5.5.4/esp-idf/export.sh   # ESP-IDF v5.5.4
idf.py build                                            # compile
idf.py -p /dev/ttyUSB0 flash monitor                    # flash + serial logs
idf.py fullclean                                        # also clears managed_components
```

There is no host-side test suite. Verification = clean build + on-device behavior over `idf.py monitor` (watch the `dino_*` log tags). `managed_components/` is gitignored; it is resolved from `main/idf_component.yml` + `dependencies.lock` by the component manager on build.

## Architecture

Everything lives in one `main/` component (all sources listed in `main/CMakeLists.txt`; mixed C and C++ — headers bridge with `extern "C"`). Central knobs are in `main/config.h` (feature toggles, all pin definitions, flash type).

### Boot flow (`main.cc`)

`InitPower` (latch power) → `InitAudio` → `InitServos` → `PlayBootTone` → WiFi: no saved credentials → provisioning portal immediately; credentials that fail → portal after timeout → `flash_audio_init` → HTTP server → (optional, `AUDIO_SYNC_ENABLE`) device activation + audio sync → `InitAutoRun` → `ChatInit`. `OFFLINE_DEMO=1` in config.h skips everything network-related for instant boot.

### FreeRTOS task map

| Task | Prio | Role |
|------|------|------|
| `dino_play` | 3 (core 1) | Flash Opus playback pipeline |
| chat tasks (chat.cc) | high | Mic capture → WSS send / TTS receive → speaker |
| `dino_auto` | 2 | Autonomous motion engine, 50 FPS tick |
| `dino_power` | 1 | Button debounce, long-press shutdown, battery ADC |

### Audio — two independent paths behind one facade (`audio.h`)

1. **Flash playback**: SPI Flash 4KB reads → `ogg_demuxer` (zero-alloc OGG state machine) → Opus decode → I2S/ES8311. Non-blocking `PlayDinoSound(type)` with a queue.
2. **LLM realtime chat** (`chat.cc` + `realtime_ws.c`): mic 48kHz → 3:1 decimate to 16kHz → binary WSS frames → server ASR/LLM/TTS → base64 TTS chunks queued → `AudioWritePcm48k` playback. Pre-buffering (`TTS_PREBUFFER_MS` etc. in chat.cc) is tuned against real server jitter — treat those constants carefully. State machine: LISTENING (send mic, pause during TTS to avoid echo) ↔ PLAYING. Toggle by **double-clicking the power button** (`power.cc` → `ChatToggle`).

`AudioReadMic48k`/`AudioWritePcm48k` in audio.cc are the shared duplex interface over the single ES8311 codec; flash playback and chat both go through audio.cc's mutex.

### Motion engine (`auto_run.cc`, ~2900 lines — the core)

High-level `DinoAction` enum (discover/affection/happy/eat/startled/…) choreographs all 5 servos with "organic" waveforms (phase-warped + harmonic sine, center drift) and 3s smoothstep crossfades between actions. Sound+motion pair up three ways: by TOC index (`TriggerDinoSoundAction`), by filename semantics (`TriggerDinoActionWithAutoSound`), or live by decoded-audio envelope (`AudioMotionData` in audio.h). Servo index order: 0=IO17 neck tilt, 1=IO16 neck lean, 2=IO15 head turn, 3=IO8 tail up/down, 4=IO18 tail LR. IO8 range is physically limited to 55°–180°.

### External flash + audio filesystem

`external_flash.h` is a compile-time abstraction: `EXTERNAL_FLASH_TYPE` in config.h picks W25Q256 NOR (1) or W25N01GV NAND (2); all code calls `external_flash_*` inline wrappers, never a chip driver directly. `flash_audio.cc` implements the TOC filesystem on top (sector 0, magic `"PNDA"`, 80-byte entries: name/offset/size/rate/duration); `flash_upload_server.cc` exposes upload/erase/play HTTP APIs; `sync_audio.cc` mirrors a server file list into flash (streamed, no RAM buffer).

### Connectivity & auth

- `wifi.cc` — STA connect/reconnect. Credentials now live in NVS (provisioning), not config.h (README section on this is outdated).
- `wifi_config.cc` — SoftAP provisioning portal ("Panda-XXXX", 192.168.4.1, DNS hijack, AP scan), saved to NVS.
- `http_server.cc` — control panel UI + `/api/servo|battery|autoplay|action|chat|wifi*` endpoints.
- `device_registry.cc` — Audio Hub device registration/activation/heartbeat; token cached in NVS. Gating in front of `sync_audio_files`.
- Chat auth (`ws_auth.c` + `chat_cert.h`): the realtime WSS uses **cookie + Origin** auth (login POST → `mem_dialog_session` cookie), not Bearer tokens — a Bearer-only handshake gets closed with 1008.

### sdkconfig.defaults is load-bearing

The realtime chat breaks if these regress: `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` (RX/TX must not share one WS lock), enlarged TCP window/snd_buf (TTS stalls otherwise), 240MHz CPU, PSRAM-with-malloc thresholds, reduced mbedTLS buffers, 1000Hz FreeRTOS tick, 500ms interrupt watchdog (SPI flash + I2S contention). Read the comments in that file before touching sdkconfig.

## Reference docs

- `docs/elegant_motion_v2.md` — motion system V3.3: servo mapping, organic-wave design, crossfade, how Web/voice code calls actions.
- `docs/dinosaur_action_catalog_v2_8.md` — every action variant, ambient scenes, and call signatures.
- `docs/audio_and_action.md` — deep dive into the audio pipeline and motion/sound coupling.
- Sibling projects: `../tailRedPanda/` (panda firmware this was ported from; its `scripts/build_opus_bin.py` / `flash_opus.py` package & upload .opus audio), `../audio/`, `../crawlPanda/`, `../shared/`.
