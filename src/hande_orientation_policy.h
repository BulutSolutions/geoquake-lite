#pragma once

// Pure auto-rotation logic (no Arduino, no LVGL): host-tested on the host. EMA filter, table-flat lockout, motion
// gate, 120 mg dominance margin, 0.45 g classification threshold, a 1 s settle, and no commit while a veto holds
// (finger on the screen, a card transition, ...). Orientation numbers: hande_display_policy.h (USB position).

#include <math.h>
#include <stdint.h>

namespace hande_orientation_policy {

constexpr float kEmaAlpha = 0.20f;
constexpr float kFlatRatio = 0.88f;       // |az| / |a| above this = lying flat: never rotate
constexpr float kMotionGate = 0.35f;      // | |a| - 1 g | above this = moving: wait
constexpr float kDominanceMargin = 0.12f; // dominant axis must beat the other by 120 mg (no 45-degree jitter)
constexpr float kAxisThreshold = 0.45f;
constexpr uint32_t kSettleMs = 1000;

// Device axes -> orientation. false = no clear answer (keep the current one).
inline bool classify(float ax, float ay, uint8_t& result) {
    const float abs_x = fabsf(ax);
    const float abs_y = fabsf(ay);
    if (fabsf(abs_x - abs_y) < kDominanceMargin) return false;
    if (abs_x > abs_y && abs_x > kAxisThreshold) {
        result = ax > 0.0f ? 0 : 2;  // USB bottom / USB top
        return true;
    }
    if (abs_y > abs_x && abs_y > kAxisThreshold) {
        result = ay < 0.0f ? 1 : 3;  // USB left / USB right
        return true;
    }
    return false;
}

struct Filter {
    float ax = 1.0f, ay = 0.0f, az = 0.0f;  // filtered acceleration (g)
    uint8_t current = 0;
    uint8_t candidate = 0;
    uint32_t candidate_start_ms = 0;
    bool restored = false;        // current came from the NVS restore, not from a classification yet
    bool raw_flat = false;        // the last raw reading was flat (set by filter_accel, read by decide)
    bool flat_timing = false;     // the flat-reset timer is running (separate from flat_start_ms so now == 0 works)
    uint32_t flat_start_ms = 0;   // first usable raw flat reading while restored
};

// Result of one decide() step: no change, a normal commit (save it), or the restored orientation dropped because the
// board lies flat (apply 0 but keep the saved value).
enum Decision { kNone, kCommit, kFlatReset };

// Seeds a filter for a restored orientation: current and candidate become o, and the EMA is o's canonical gravity
// vector so the first ticks agree with it and do not flip back. candidate_start_ms is left as it is (a fresh filter
// starts at 0). o is masked to 0..3; the vectors match classify(): 0 (1,0,0), 2 (-1,0,0), 1 (0,-1,0), 3 (0,1,0).
inline void seed(Filter& f, uint8_t o) {
    o &= 3;
    f.current = f.candidate = o;
    f.restored = true;
    f.flat_timing = false;
    f.flat_start_ms = 0;
    switch (o) {
        case 0: f.ax = 1.0f; f.ay = 0.0f; f.az = 0.0f; break;
        case 1: f.ax = 0.0f; f.ay = -1.0f; f.az = 0.0f; break;
        case 2: f.ax = -1.0f; f.ay = 0.0f; f.az = 0.0f; break;
        default: f.ax = 0.0f; f.ay = 1.0f; f.az = 0.0f; break;
    }
}

// Feeds the EMA only. false = the reading is unusable (|a| ~ 0): skip this tick. Also remembers whether the raw
// reading was flat: a seeded EMA still points at the restored orientation for a moment, so decide() must not count
// that transient as a real classification when the board is actually lying flat.
inline bool filter_accel(Filter& f, float x, float y, float z) {
    f.ax = kEmaAlpha * x + (1.0f - kEmaAlpha) * f.ax;
    f.ay = kEmaAlpha * y + (1.0f - kEmaAlpha) * f.ay;
    f.az = kEmaAlpha * z + (1.0f - kEmaAlpha) * f.az;
    const float raw_magnitude = sqrtf(x * x + y * y + z * z);
    f.raw_flat = raw_magnitude >= 0.1f && fabsf(z) / raw_magnitude > kFlatRatio;
    return f.ax * f.ax + f.ay * f.ay + f.az * f.az >= 0.01f;
}

// One decision step after filter_accel() returned true. kCommit when f.current changed (apply and save it);
// kFlatReset when a restored orientation is dropped to 0 because the board lies flat (apply 0, do not save).
inline Decision decide(Filter& f, uint32_t now, bool vetoed) {
    const float magnitude = sqrtf(f.ax * f.ax + f.ay * f.ay + f.az * f.az);
    const bool filtered_flat = fabsf(f.az) / magnitude > kFlatRatio;
    // Only a usable raw flat sample runs the reset timer: a tick where the board was already picked up must stop it,
    // even while the EMA still looks flat.
    if (!f.raw_flat) {
        f.flat_timing = false;
        f.flat_start_ms = 0;
    }
    if (f.restored && f.raw_flat) {
        f.candidate_start_ms = now;
        if (!f.flat_timing) {
            f.flat_timing = true;
            f.flat_start_ms = now;
        }
        if (now - f.flat_start_ms >= kSettleMs && f.current != 0) {
            // Like a normal commit, a veto (finger on the screen, ...) postpones it; the timer keeps running.
            if (vetoed) return kNone;
            f.current = f.candidate = 0;
            f.restored = false;
            f.flat_timing = false;
            f.flat_start_ms = 0;
            return kFlatReset;
        }
        return kNone;
    }
    if (filtered_flat) {
        f.candidate_start_ms = now;
        return kNone;
    }
    if (fabsf(magnitude - 1.0f) > kMotionGate) {
        f.candidate_start_ms = now;
        return kNone;
    }
    uint8_t detected = f.current;
    if (!classify(f.ax, f.ay, detected)) {
        f.candidate = f.current;
        f.candidate_start_ms = now;
        return kNone;
    }
    f.restored = false;
    if (vetoed) {
        f.candidate_start_ms = now;
        return kNone;
    }
    if (detected == f.current) {
        f.candidate = f.current;
        f.candidate_start_ms = now;
        return kNone;
    }
    if (detected != f.candidate) {
        f.candidate = detected;
        f.candidate_start_ms = now;
        return kNone;
    }
    if (now - f.candidate_start_ms < kSettleMs) return kNone;
    f.current = f.candidate;
    return kCommit;
}

}  // namespace hande_orientation_policy
