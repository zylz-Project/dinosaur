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
void AudioStopCurrent();        // interrupt the currently playing sound (if any)
void AudioPlayChime(bool ascending);  // 合成叮咚提示音: true=上扬, false=下扬

// Power on/off chimes. These are generated in-code (a short sine melody) and
// written straight to the codec, so they work independently of the Flash audio
// TOC — important for the shutdown chime, which must play reliably after the
// audio files may have been unmounted. Both block until the tone finishes.
void PlayBootTone();
void PlayShutdownTone();

// === LLM chat audio (48kHz mono duplex over the ES8311 codec) ===
// Reads `samples` mono int16 samples captured from the codec mic @ 48kHz.
// Returns bytes actually read (0 on failure).
int  AudioReadMic48k(int16_t *buf, int samples);
// Writes `samples` mono int16 samples @ 48kHz to the speaker (mutex-protected).
void AudioWritePcm48k(const int16_t *pcm, int samples);
// Plays the short two-tone prompt used when realtime chat is actually ready.
void AudioPlayChatReadyTone();
