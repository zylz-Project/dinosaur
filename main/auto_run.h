#pragma once

#include "config.h"

#if ENABLE_AUTO_RUN

// High-level character actions. Callers do not need to know servo indices or
// angles; the auto-run task sequences all five axes and optionally starts a
// matching sound on the same frame.
enum DinoAction {
    DINO_ACTION_DISCOVER = 0,   // head notices first, neck follows, curious tilt
    DINO_ACTION_AFFECTION,      // lean in and nuzzle
    DINO_ACTION_HAPPY,           // excited chirp and lively tail wag
    DINO_ACTION_PROUD_CALL,      // young dinosaur's confident display
    DINO_ACTION_EAT,             // rhythmic pecking/chewing
    DINO_ACTION_LISTEN,          // freeze and locate distant footsteps
    DINO_ACTION_STARTLED,        // recoil, check, then recover
    DINO_ACTION_SLEEPY,          // settle down with breathing motion
    DINO_ACTION_COUNT,
};

void InitAutoRun();
bool IsAutoRunRunning();
void SetAutoRunRunning(bool v);
bool IsAutoRunHardSwing();
void SetAutoRunHardSwing(bool v);

// Queue one complete action. sound_index=-1 plays motion only. When a valid
// Flash audio index is supplied, sound and motion are started together.
bool TriggerDinoAction(DinoAction action, int sound_index = -1);

// Choose the action from the Flash filename/duration and queue both together.
// Useful for startup, Web controls, and future touch/voice interaction code.
bool TriggerDinoSoundAction(int sound_index);

// Choose a semantically matching Flash sound for this action. If no suitable
// file exists, the motion still runs silently instead of playing a wrong sound.
bool TriggerDinoActionWithAutoSound(DinoAction action);

// Pick a short vocal sound (never chewing/footsteps) for the boot greeting.
bool TriggerDinoGreeting();

#endif  // ENABLE_AUTO_RUN
