#pragma once

#include <cstdint>

// Lightweight motion features extracted from decoded PCM. Values are updated
// while the speaker is playing, so animation code can follow the actual sound
// instead of relying only on a filename or an estimated duration.
struct AudioMotionData {
  bool playing;
  float level;        // smoothed 0..1 loudness envelope
  float attack;       // 0..1 onset strength (new syllable/bite/footstep)
  uint32_t elapsed_ms;
  uint32_t duration_ms;
  int sound_index;
};

void InitAudio();
bool PlayDinoSound(int type);  // non-blocking: returns immediately, plays in background
bool IsAudioPlaying();
AudioMotionData GetAudioMotionData();
void FlushAudioQueue();         // clear all pending sounds from queue

// Power on/off chimes. These are generated in-code (a short sine melody) and
// written straight to the codec, so they work independently of the Flash audio
// TOC — important for the shutdown chime, which must play reliably after the
// audio files may have been unmounted. Both block until the tone finishes.
void PlayBootTone();
void PlayShutdownTone();
