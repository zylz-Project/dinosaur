#pragma once

// Sound type enum — order matches alphabetical sort of .opus files in opus_audio/.
// TOC index = enum value. All file metadata is read from SPI Flash TOC at runtime.
typedef enum {
    DINO_SOUND_COUNT  // placeholder — actual count determined by TOC at runtime
} dino_sound_type_t;

// Read from SPI Flash TOC at runtime.
// Returns "???" if TOC not loaded or index out of range.
const char *dino_sound_name(int type);

// Read from SPI Flash TOC at runtime (estimated from file size).
// Returns 0 if TOC not loaded or index out of range.
int dino_sound_duration_ms(int type);
