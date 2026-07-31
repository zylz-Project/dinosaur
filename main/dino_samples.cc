#include "dino_samples.h"
#include "flash_audio.h"

const char *dino_sound_name(int type) {
    return flash_audio_get_name(type);
}

int dino_sound_duration_ms(int type) {
    return flash_audio_get_duration_ms(type);
}
