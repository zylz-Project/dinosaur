#pragma once

void InitAudio();
bool PlayDinoSound(int type);  // non-blocking: returns immediately, plays in background
bool IsAudioPlaying();
void FlushAudioQueue();         // clear all pending sounds from queue
