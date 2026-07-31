#include "auto_run.h"
#include "audio.h"
#include "config.h"
#include "dino_samples.h"
#include "flash_audio.h"
#include "servo.h"

#if ENABLE_AUTO_RUN

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "dino_auto";

// =========================================================================
// Global state
// =========================================================================
static volatile bool g_running    = AUTO_RUN_DEFAULT_ON;
static volatile bool g_hard_swing = AUTO_RUN_DEFAULT_HARD;

bool IsAutoRunRunning()          { return g_running; }
void SetAutoRunRunning(bool v)   { g_running = v; }
bool IsAutoRunHardSwing()        { return g_hard_swing; }
void SetAutoRunHardSwing(bool v) { g_hard_swing = v; }

// =========================================================================
// Utility
// =========================================================================
inline int irnd(int n) { return (int)(esp_random() % (uint32_t)(n)); }
inline int irand(int lo, int hi) { return lo + irnd(hi - lo + 1); }
inline float frnd() { return (float)esp_random() / (float)UINT32_MAX; }

inline float harden(float v) {
    float s = fabsf(v);
    s = s * s * (3.0f - 2.0f * s);
    return (v > 0.0f ? s : -s);
}

// Phase warp for organic motion
inline float organic_sin(float x) {
    float phase = fmodf(x / (2.0f * M_PI), 1.0f);
    if (phase < 0.0f) phase += 1.0f;
    float wx = phase * 2.0f * M_PI;
    return sinf(wx)
         + 0.12f * sinf(2.0f * wx + 0.5f)
         + 0.05f * sinf(3.0f * wx + 1.2f);
}

// =========================================================================
// Sound selection — category-based (🐾 animal for random, 🌿 ambient for relax)
// =========================================================================
static int g_last_sound = 0;

static void init_sound_indices() {
    int total = flash_audio_get_file_count();
    int animal = flash_audio_get_count_by_category("animal");
    int ambient = flash_audio_get_count_by_category("ambient");
    ESP_LOGI(TAG, "Audio: %d total (🐾%d animal  🌿%d ambient)", total, animal, ambient);
}

static bool trigger_sound() {
    int idx = flash_audio_get_random_in_category("animal");
    if (idx < 0) return false;
    g_last_sound = idx;
    PlayDinoSound(idx);
    return true;
}

// =========================================================================
// Neck modes — combined tilt (IO15) + lean (IO16)
// =========================================================================
enum NeckMode {
    NECK_BREATHE, NECK_IDLE, NECK_NOD, NECK_SWAY,
    NECK_LOOK_LEFT, NECK_LOOK_RIGHT, NECK_ALERT, NECK_SLEEP,
    NECK_PECK, NECK_CURIOUS,
    NECK_MODE_COUNT
};
// center_tilt, amp_tilt, center_lean, amp_lean, period_s, phase_offs
struct NeckParam { int ct, at, cl, al; float period, phase_offs; };
static constexpr NeckParam kNeckP[NECK_MODE_COUNT] = {
    {90,20,90,12, 5.0f,0.0f},   // BREATHE — gentle synced oscillation
    {90,10,90, 8, 4.0f,0.0f},   // IDLE — minimal
    {90,35,90, 8, 3.0f,0.1f},   // NOD — large tilt, slight lean
    {90,22,90,25, 3.5f,0.25f},  // SWAY — circular neck motion
    {90,12,125,15, 5.0f,0.5f},  // LOOK_LEFT — lean left with tilt
    {90,12, 55,15, 5.0f,0.5f},  // LOOK_RIGHT — lean right
    {115,10,90, 6, 5.5f,0.0f},  // ALERT — forward, minimal lean
    {65, 8,90, 5, 7.0f,0.0f},   // SLEEP — droopy back
    {90,40,90, 6, 1.8f,0.0f},   // PECK — quick forward bursts
    {90,18,90,20, 4.0f,0.2f},   // CURIOUS — alternating
};

// =========================================================================
// Head modes — turn (IO17)
// =========================================================================
enum HeadMode {
    HEAD_CENTER, HEAD_LOOK_LEFT, HEAD_LOOK_RIGHT, HEAD_SCAN,
    HEAD_TILT_CURIOUS,
    HEAD_MODE_COUNT
};
struct HeadParam { int center, amp; float period; };
static constexpr HeadParam kHeadP[HEAD_MODE_COUNT] = {
    {90, 5, 5.0f},  // CENTER — near still
    {135,8, 5.0f},  // LOOK_LEFT
    {45, 8, 5.0f},  // LOOK_RIGHT
    {90,55, 4.0f},  // SCAN — sweep left-right
    {90,35, 3.5f},  // TILT_CURIOUS
};

// =========================================================================
// Tail modes — UD (IO18) + LR (IO8)
// =========================================================================
enum TailMode {
    TAIL_RELAX, TAIL_WAG, TAIL_RAISE, TAIL_DROOP,
    TAIL_CIRCLE, TAIL_HAPPY, TAIL_ALERT, TAIL_TWITCH,
    TAIL_MODE_COUNT
};
struct TailParam { int uc, ua, lc, la; float period, phase_offs; };
static constexpr TailParam kTailP[TAIL_MODE_COUNT] = {
    {90,20,90,25, 4.0f,0.22f},  // RELAX — gentle sway
    {90, 5,90,65, 1.2f,0.0f},   // WAG — side to side
    {35,10,90,15, 5.0f,0.0f},   // RAISE — tail up + slight LR
    {145,10,90,15, 5.0f,0.0f},  // DROOP — tail down
    {90,40,90,40, 2.5f,0.25f},  // CIRCLE
    {90, 5,90,55, 0.8f,0.0f},   // HAPPY — fast wag
    {40, 8,90, 8, 6.0f,0.0f},   // ALERT — raised, near still
    {90,12,90,40, 1.5f,0.15f},  // TWITCH — quick flick
};

// =========================================================================
// Idle/relax pools
// =========================================================================
static constexpr NeckMode kRelaxNeck[] = {NECK_BREATHE, NECK_IDLE, NECK_SLEEP, NECK_SWAY};
static constexpr int kRelaxNeckN = sizeof(kRelaxNeck)/sizeof(kRelaxNeck[0]);

static constexpr HeadMode kRelaxHead[] = {HEAD_CENTER, HEAD_SCAN};
static constexpr int kRelaxHeadN = sizeof(kRelaxHead)/sizeof(kRelaxHead[0]);

static constexpr TailMode kRelaxTail[] = {TAIL_RELAX, TAIL_DROOP, TAIL_ALERT};
static constexpr int kRelaxTailN = sizeof(kRelaxTail)/sizeof(kRelaxTail[0]);

static constexpr NeckMode kIdleNeck[] = {NECK_BREATHE, NECK_IDLE, NECK_SWAY, NECK_CURIOUS, NECK_LOOK_LEFT, NECK_LOOK_RIGHT, NECK_SLEEP};
static constexpr int kIdleNeckN = sizeof(kIdleNeck)/sizeof(kIdleNeck[0]);

static constexpr HeadMode kIdleHead[] = {HEAD_CENTER, HEAD_SCAN, HEAD_TILT_CURIOUS, HEAD_LOOK_LEFT, HEAD_LOOK_RIGHT};
static constexpr int kIdleHeadN = sizeof(kIdleHead)/sizeof(kIdleHead[0]);

static constexpr TailMode kIdleTail[] = {TAIL_RELAX, TAIL_WAG, TAIL_CIRCLE, TAIL_ALERT, TAIL_TWITCH, TAIL_RAISE};
static constexpr int kIdleTailN = sizeof(kIdleTail)/sizeof(kIdleTail[0]);

static constexpr NeckMode kStillNeck[] = {NECK_SLEEP, NECK_IDLE, NECK_LOOK_LEFT, NECK_LOOK_RIGHT};
static constexpr int kStillNeckN = sizeof(kStillNeck)/sizeof(kStillNeck[0]);

static constexpr TailMode kStillTail[] = {TAIL_DROOP, TAIL_DROOP, TAIL_ALERT};
static constexpr int kStillTailN = sizeof(kStillTail)/sizeof(kStillTail[0]);

// =========================================================================
// Sound-to-action mapping
// =========================================================================
static void sound_to_action(int sound, NeckMode &nm, HeadMode &hm, TailMode &tm,
                             float &na, float &ha, float &ta, bool &hd) {
    int r = irnd(100);
    switch (sound) {
    case 1: case 2: // dinosaur calls
        if (r < 30)      { nm=NECK_ALERT; hm=HEAD_SCAN;  tm=TAIL_ALERT; na=0.90f; ha=0.80f; ta=0.85f; hd=false; }
        else if (r < 55) { nm=NECK_LOOK_LEFT; hm=HEAD_LOOK_LEFT; tm=TAIL_WAG; na=0.85f; ha=0.90f; ta=0.80f; hd=false; }
        else if (r < 75) { nm=NECK_NOD;  hm=HEAD_CENTER; tm=TAIL_TWITCH; na=0.80f; ha=0.50f; ta=0.90f; hd=true; }
        else             { nm=NECK_SWAY;  hm=HEAD_SCAN;  tm=TAIL_CIRCLE; na=0.85f; ha=0.80f; ta=0.80f; hd=false; }
        break;
    case 3: // eating sounds
        if (r < 40)      { nm=NECK_PECK; hm=HEAD_CENTER; tm=TAIL_RELAX; na=0.95f; ha=0.30f; ta=0.50f; hd=false; }
        else if (r < 70) { nm=NECK_NOD;  hm=HEAD_TILT_CURIOUS; tm=TAIL_TWITCH; na=0.80f; ha=0.60f; ta=0.50f; hd=false; }
        else             { nm=NECK_CURIOUS; hm=HEAD_TILT_CURIOUS; tm=TAIL_RELAX; na=0.75f; ha=0.70f; ta=0.60f; hd=false; }
        break;
    case 4: // baby sounds
        if (r < 50)      { nm=NECK_SWAY; hm=HEAD_SCAN; tm=TAIL_WAG; na=0.85f; ha=1.00f; ta=1.00f; hd=(r<20); }
        else if (r < 80) { nm=NECK_NOD;  hm=HEAD_CENTER; tm=TAIL_HAPPY; na=0.85f; ha=0.50f; ta=0.90f; hd=false; }
        else             { nm=NECK_ALERT; hm=HEAD_LOOK_LEFT; tm=TAIL_ALERT; na=0.80f; ha=0.80f; ta=0.85f; hd=true; }
        break;
    default:
        nm=NECK_IDLE; hm=HEAD_CENTER; tm=TAIL_RELAX; na=0.80f; ha=0.50f; ta=0.80f; hd=false; break;
    }
}

// =========================================================================
// Crossfade structure
// =========================================================================
struct XFade {
    bool     active     = false;
    uint32_t start_tick = 0;
    NeckMode from_neck; HeadMode from_head; TailMode from_tail;
    float    from_neck_amp, from_head_amp, from_tail_amp;
};

// =========================================================================
// Angle calculations with crossfade
// =========================================================================
static int calc_head_angle(uint32_t tick, int tick_ms,
                            HeadMode mode, float amp_eff,
                            const XFade *xfade) {
    auto &hp = kHeadP[mode];
    float t_s = tick * tick_ms / 1000.0f;
    float ph  = fmodf(t_s / hp.period, 1.0f);
    float sv  = organic_sin(ph * 2.0f * M_PI);

    if (!xfade || !xfade->active)
        return hp.center + (int)(hp.amp * sv * amp_eff);

    float bt = (tick - xfade->start_tick) * tick_ms / 1000.0f;
    if (bt >= 3.0f) return hp.center + (int)(hp.amp * sv * amp_eff);

    float mx  = bt * bt * (3.0f - 2.0f * bt);
    auto &ohp = kHeadP[xfade->from_head];
    float op  = fmodf(t_s / ohp.period, 1.0f);
    int old_a = ohp.center + (int)(ohp.amp * organic_sin(op * 2.0f * M_PI) * xfade->from_head_amp);
    int new_a = hp.center  + (int)(hp.amp * sv * amp_eff);
    return old_a + (int)((new_a - old_a) * mx);
}

static void calc_neck_angles(uint32_t tick, int tick_ms,
                              NeckMode mode, float amp_eff,
                              const XFade *xfade, bool hard,
                              int &tilt, int &lean) {
    auto &np = kNeckP[mode];
    float speed = g_hard_swing ? HARD_SWING_SPEED_X : 1.0f;
    float t_s   = tick * tick_ms / 1000.0f / speed;
    float pt    = fmodf(t_s / np.period, 1.0f);
    float pl    = fmodf(t_s / np.period + np.phase_offs, 1.0f);
    float st    = organic_sin(pt * 2.0f * M_PI);
    float sl    = organic_sin(pl * 2.0f * M_PI);
    if (hard) { st = harden(st); sl = harden(sl); }

    // Slow center drift
    float drift_t = 3.0f * sinf(t_s * 0.12f + 0.8f);
    float drift_l = 2.5f * sinf(t_s * 0.09f + 2.1f);

    if (!xfade || !xfade->active) {
        tilt = np.ct + (int)((np.at * st + drift_t) * amp_eff);
        lean = np.cl + (int)((np.al * sl + drift_l) * amp_eff);
        return;
    }

    float bt = (tick - xfade->start_tick) * tick_ms / 1000.0f;
    if (bt >= 3.0f) {
        tilt = np.ct + (int)((np.at * st + drift_t) * amp_eff);
        lean = np.cl + (int)((np.al * sl + drift_l) * amp_eff);
        return;
    }

    float mx   = bt * bt * (3.0f - 2.0f * bt);
    auto &onp  = kNeckP[xfade->from_neck];
    float opt  = fmodf(t_s / onp.period, 1.0f);
    float opl  = fmodf(t_s / onp.period + onp.phase_offs, 1.0f);
    float ost  = organic_sin(opt * 2.0f * M_PI);
    float osl  = organic_sin(opl * 2.0f * M_PI);
    if (hard) { ost = harden(ost); osl = harden(osl); }

    int o_tilt = onp.ct + (int)(onp.at * ost * xfade->from_neck_amp);
    int o_lean = onp.cl + (int)(onp.al * osl * xfade->from_neck_amp);
    int n_tilt = np.ct  + (int)((np.at * st + drift_t) * amp_eff);
    int n_lean = np.cl  + (int)((np.al * sl + drift_l) * amp_eff);
    tilt = o_tilt + (int)((n_tilt - o_tilt) * mx);
    lean = o_lean + (int)((n_lean - o_lean) * mx);
}

static void calc_tail_angles(uint32_t tick, int tick_ms,
                              TailMode mode, float amp_eff,
                              const XFade *xfade, bool hard,
                              int &ud, int &lr) {
    auto &tp = kTailP[mode];
    float speed = g_hard_swing ? HARD_SWING_SPEED_X : 1.0f;
    float t_s   = tick * tick_ms / 1000.0f / speed;
    float pu    = fmodf(t_s / tp.period, 1.0f);
    float pl    = fmodf(t_s / tp.period + tp.phase_offs, 1.0f);
    float su    = organic_sin(pu * 2.0f * M_PI);
    float sl    = organic_sin(pl * 2.0f * M_PI);
    if (hard) { su = harden(su); sl = harden(sl); }

    float drift_u = 2.0f * sinf(t_s * 0.08f + 1.5f);
    float drift_l = 3.0f * sinf(t_s * 0.10f + 3.0f);

    if (!xfade || !xfade->active) {
        ud = tp.uc + (int)((tp.ua * su + drift_u) * amp_eff);
        lr = tp.lc + (int)((tp.la * sl + drift_l) * amp_eff);
        return;
    }

    float bt = (tick - xfade->start_tick) * tick_ms / 1000.0f;
    if (bt >= 3.0f) {
        ud = tp.uc + (int)((tp.ua * su + drift_u) * amp_eff);
        lr = tp.lc + (int)((tp.la * sl + drift_l) * amp_eff);
        return;
    }

    float mx   = bt * bt * (3.0f - 2.0f * bt);
    auto &otp  = kTailP[xfade->from_tail];
    float opu  = fmodf(t_s / otp.period, 1.0f);
    float opl  = fmodf(t_s / otp.period + otp.phase_offs, 1.0f);
    float osu  = organic_sin(opu * 2.0f * M_PI), osl = organic_sin(opl * 2.0f * M_PI);
    if (hard) { osu = harden(osu); osl = harden(osl); }

    int o_ud = otp.uc + (int)(otp.ua * osu * xfade->from_tail_amp);
    int o_lr = otp.lc + (int)(otp.la * osl * xfade->from_tail_amp);
    int n_ud = tp.uc  + (int)((tp.ua * su + drift_u) * amp_eff);
    int n_lr = tp.lc  + (int)((tp.la * sl + drift_l) * amp_eff);
    ud = o_ud + (int)((n_ud - o_ud) * mx);
    lr = o_lr + (int)((n_lr - o_lr) * mx);
}

// =========================================================================
// Nature wobble overlay (for relax mode)
// =========================================================================
static void apply_nature_wobble(uint32_t tick, int tick_ms,
                                 int &neck_tilt, int &neck_lean,
                                 int &tail_ud, int &tail_lr) {
    float nt  = tick * tick_ms / 1000.0f;
    float w1  = sinf(nt * 0.17f)       * 18.0f;
    float w2  = sinf(nt * 0.35f + 1.0f) * 12.0f;
    float w1u = cosf(nt * 0.21f)       * 10.0f;
    float w2u = sinf(nt * 0.43f + 0.7f) *  8.0f;
    neck_tilt = (int)((float)neck_tilt * 0.35f + (90.0f + w1) * 0.65f);
    neck_lean = (int)((float)neck_lean * 0.35f + (90.0f + w2) * 0.65f);
    tail_ud   = (int)((float)tail_ud   * 0.35f + (90.0f + w1u) * 0.65f);
    tail_lr   = (int)((float)tail_lr   * 0.35f + (90.0f + w2u) * 0.65f);
}

// =========================================================================
// AutoRun main task
// =========================================================================
static void auto_run_task(void *arg) {
    constexpr int MS = 20;
    constexpr int TICK_DECAY = 5000 / MS;
    constexpr int TICK_DEBOUNCE = 500 / MS;

    init_sound_indices();

    uint32_t tick = 0;

    // Current state
    NeckMode neck_mode = NECK_BREATHE;
    HeadMode head_mode = HEAD_CENTER;
    TailMode tail_mode = TAIL_RELAX;
    float    neck_amp = 0.60f, head_amp = 0.50f, tail_amp = 0.55f;
    bool     is_hard  = false;
    uint32_t amp_start = 0;

    // Relax mode
    bool     in_relax = false, nature_on = false;
    uint32_t next_relax = (uint32_t)(300 * 1000 / MS);  // first relax after ~5min
    uint32_t next_var   = 0;

    // Sound triggers
    uint32_t next_sound   = (uint32_t)(8 * 1000 / MS);
    uint32_t react_end    = 0;
    bool     was_playing  = false;

    // Idle/silent timers
    uint32_t next_spont = (uint32_t)(irand(15, 35) * 1000 / MS);
    uint32_t next_idle  = (uint32_t)(irand(8, 15) * 1000 / MS);

    // Debounce
    uint32_t last_toggle = 0;
    bool     prev_running = g_running;

    // Crossfade
    XFade xfade;

    // ======================== Main loop ========================
    while (true) {
        if (!g_running) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        // ----- Relax mode timer -----
        if (!in_relax && tick >= next_relax) {
            int nature_idx = flash_audio_get_random_in_category("ambient");
            if (nature_idx >= 0) {
                in_relax  = true;
                nature_on = true;
                FlushAudioQueue();
                PlayDinoSound(nature_idx);
                neck_mode = kRelaxNeck[irnd(kRelaxNeckN)];
                head_mode = kRelaxHead[irnd(kRelaxHeadN)];
                tail_mode = kRelaxTail[irnd(kRelaxTailN)];
                neck_amp  = 0.75f + frnd() * 0.25f;
                head_amp  = 0.60f + frnd() * 0.30f;
                tail_amp  = 0.70f + frnd() * 0.30f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = false;
                next_var  = tick + (uint32_t)(irand(1500, 2500) / MS);
                ESP_LOGI(TAG, "Relax ON");
            } else {
                next_relax = tick + (uint32_t)(300 * 1000 / MS);
            }
        }

        // ----- Audio state machine -----
        bool now_playing = IsAudioPlaying();

        if (now_playing && !was_playing) {
            if (nature_on) {
                // Rotate relax pattern
                NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
                float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
                neck_mode = kRelaxNeck[irnd(kRelaxNeckN)];
                head_mode = kRelaxHead[irnd(kRelaxHeadN)];
                tail_mode = kRelaxTail[irnd(kRelaxTailN)];
                neck_amp  = 0.75f + frnd() * 0.25f;
                head_amp  = 0.60f + frnd() * 0.30f;
                tail_amp  = 0.70f + frnd() * 0.30f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = true; xfade.start_tick = tick;
                xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
                xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
                next_var  = tick + (uint32_t)(irand(3, 6) * 1000 / MS);
            } else {
                // Sound reaction
                NeckMode nm; HeadMode hm; TailMode tm;
                float na, ha, ta; bool hd;
                sound_to_action(g_last_sound, nm, hm, tm, na, ha, ta, hd);
                xfade.from_neck = neck_mode; xfade.from_head = head_mode; xfade.from_tail = tail_mode;
                xfade.from_neck_amp = neck_amp; xfade.from_head_amp = head_amp; xfade.from_tail_amp = tail_amp;
                xfade.active = true; xfade.start_tick = tick;
                neck_mode = nm; head_mode = hm; tail_mode = tm;
                neck_amp = na; head_amp = ha; tail_amp = ta; is_hard = hd;
                amp_start = tick;
                react_end = tick + (uint32_t)((4000 + irnd(3000)) / MS);
            }
        }
        if (!now_playing && was_playing && nature_on) {
            nature_on = false; in_relax = false;
            next_relax = tick + (uint32_t)(irand(300, 600) * 1000 / MS);  // 5~10min
            ESP_LOGI(TAG, "Relax OFF");
            NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
            neck_mode = kIdleNeck[irnd(kIdleNeckN)];
            head_mode = kIdleHead[irnd(kIdleHeadN)];
            tail_mode = kIdleTail[irnd(kIdleTailN)];
            neck_amp  = 0.65f + frnd() * 0.25f;
            head_amp  = 0.50f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
        }

        was_playing = now_playing;

        // ----- Reaction end -> idle -----
        if (!nature_on && !now_playing && react_end > 0 && tick >= react_end) {
            NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
            neck_mode = kIdleNeck[irnd(kIdleNeckN)];
            head_mode = kIdleHead[irnd(kIdleHeadN)];
            tail_mode = kIdleTail[irnd(kIdleTailN)];
            neck_amp  = 0.65f + frnd() * 0.25f;
            head_amp  = 0.50f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            react_end = 0;
        }

        // ----- Relax mode rotation -----
        if (nature_on && now_playing && tick >= next_var) {
            NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
            neck_mode = kRelaxNeck[irnd(kRelaxNeckN)];
            head_mode = kRelaxHead[irnd(kRelaxHeadN)];
            tail_mode = kRelaxTail[irnd(kRelaxTailN)];
            neck_amp  = 0.75f + frnd() * 0.25f;
            head_amp  = 0.60f + frnd() * 0.30f;
            tail_amp  = 0.70f + frnd() * 0.30f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            next_var  = tick + (uint32_t)(irand(3, 6) * 1000 / MS);
        }

        // ----- Spontaneous micro-movements -----
        if (!in_relax && !now_playing && react_end == 0 && tick >= next_spont) {
            if (irnd(5) == 0) {
                NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
                float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
                tail_mode = TAIL_TWITCH;
                head_mode = HEAD_TILT_CURIOUS;
                neck_amp  = 0.60f; head_amp = 0.50f; tail_amp = 0.50f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = true; xfade.start_tick = tick;
                xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
                xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
                react_end = tick + (uint32_t)(1200 / MS);
            }
            next_spont = tick + (uint32_t)(irand(15, 35) * 1000 / MS);
        }

        // ----- Periodic idle rotation -----
        if (!in_relax && !now_playing && react_end == 0 && tick >= next_idle) {
            NeckMode old_n = neck_mode; HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_na = neck_amp, old_ha = head_amp, old_ta = tail_amp;
            neck_mode = kIdleNeck[irnd(kIdleNeckN)];
            head_mode = kIdleHead[irnd(kIdleHeadN)];
            tail_mode = kIdleTail[irnd(kIdleTailN)];
            neck_amp  = 0.65f + frnd() * 0.25f;
            head_amp  = 0.50f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_neck = old_n; xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_neck_amp = old_na; xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            next_idle = tick + (uint32_t)(irand(10, 20) * 1000 / MS);
        }

        // ----- Sound trigger -----
        if (!in_relax && !now_playing && tick >= next_sound) {
            trigger_sound();
            next_sound = tick + (uint32_t)(1000 / MS);
        }
        if (now_playing) {
            next_sound  = tick + (uint32_t)(irand(AUDIO_SILENT_INTERVAL_MIN_S,
                                                   AUDIO_SILENT_INTERVAL_MAX_S) * 1000 / MS);
            next_spont  = tick + (uint32_t)(irand(15, 35) * 1000 / MS);
            next_idle   = tick + (uint32_t)(irand(8, 15) * 1000 / MS);
        }

        // ----- Debounce -----
        if (g_running != prev_running) {
            if (tick - last_toggle < TICK_DEBOUNCE)
                g_running = prev_running;
            else {
                last_toggle = tick;
                prev_running = g_running;
            }
        }

        // ----- Amplitude decay -----
        float decay = 1.0f;
        {
            uint32_t dt = tick - amp_start;
            if (dt < TICK_DECAY)
                decay = 1.0f - (float)dt / (float)TICK_DECAY * 0.25f;
            else
                decay = 0.75f;
        }
        float na_decay = neck_amp * decay;
        float ha_decay = head_amp * decay;
        float ta_decay = tail_amp * decay;

        // ----- Check crossfade expiry -----
        if (xfade.active && (tick - xfade.start_tick) * MS / 1000 >= 3.0f)
            xfade.active = false;

        // ----- Calculate and output angles -----
        int n_tilt, n_lean;
        calc_neck_angles(tick, MS, neck_mode, na_decay,
                          xfade.active ? &xfade : nullptr, is_hard, n_tilt, n_lean);
        SetServoAngle(SERVO_NECK_TILT, n_tilt);
        SetServoAngle(SERVO_NECK_LEAN, n_lean);

        int h_ang = calc_head_angle(tick, MS, head_mode, ha_decay,
                                     xfade.active ? &xfade : nullptr);
        SetServoAngle(SERVO_HEAD_TURN, h_ang);

        int t_ud, t_lr;
        calc_tail_angles(tick, MS, tail_mode, ta_decay,
                          xfade.active ? &xfade : nullptr, is_hard, t_ud, t_lr);

        if (nature_on && now_playing)
            apply_nature_wobble(tick, MS, n_tilt, n_lean, t_ud, t_lr);

        SetServoAngle(SERVO_TAIL_UD, t_ud);
        SetServoAngle(SERVO_TAIL_LR, t_lr);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(MS));
    }
}

void InitAutoRun() {
    xTaskCreate(auto_run_task, "dino_auto", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "Auto-run task created");
}

#endif
