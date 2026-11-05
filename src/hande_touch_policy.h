#pragma once

// Pure touch logic (no Arduino): host-tested on the host.
//  - filter_step(): turns raw controller reads into a clean press/release stream (bus-busy hold, release debounce,
//    press confirmation, jump rejection, clamping).
//  - Gesture: classifies one finished press (tap, 4-way swipe) and spots a long press while held.
// Every threshold is configuration so the feel can be tuned per build.

#include <stdint.h>

namespace hande_touch_policy {

struct Point {
    int16_t x, y;
};

enum class ReadStatus : uint8_t {
    kBusBusy,    // the I2C lock timed out or the transfer failed: nothing is known about the finger
    kNoContact,  // the controller answered: no finger
    kContact,    // the controller answered: count >= 1 points
};

struct Reading {
    ReadStatus status;
    uint8_t count;  // 1..2 for kContact
    Point points[2];
};

struct FilterConfig {
    // Contact samples needed (each within press_confirm_radius of the previous) before a press starts.
    uint8_t press_confirm_samples = 1;
    int16_t press_confirm_radius = 48;
    // The press ends on the Nth consecutive no-contact read (1 = immediately).
    uint8_t release_after_no_contact = 1;
    // Bus-busy reads during a press keep it at the last point this many times; the next one is a forced release.
    // 255 = hold for as long as the bus stays busy.
    uint8_t hold_on_bus_busy = 3;
    // true: busy and no-contact reads share one miss counter (release on miss hold_on_bus_busy + 1).
    bool shared_miss_counter = false;
    // A contact point further than this from the last accepted point is dropped as a glitch (0 = off), unless the
    // next point agrees with it (within the same distance): then the finger really moved there (a fast swipe
    // across a gap in the reads) and the press follows it.
    int16_t jump_reject_px = 0;
    // Points are clamped to 0..size-1.
    int16_t size = 480;
};

struct FilterState {
    bool pressed = false;
    uint8_t confirm = 0;
    uint8_t no_contact = 0;
    uint8_t busy = 0;
    uint8_t count = 0;
    Point last[2] = {{0, 0}, {0, 0}};
    bool has_jump = false;  // the previous contact point was dropped as a jump...
    Point jump{0, 0};       // ...from here
};

struct FilterOutput {
    bool pressed;
    uint8_t count;  // 0 when released
    Point points[2];
    bool press_edge;      // this read started a press
    bool release_edge;    // this read ended a press
    bool forced_release;  // ...because the bus stayed busy (apps end their gestures)
    bool fresh;           // points come from this read (false = held or released)
};

inline int16_t clamp_coord(int32_t v, int16_t size) {
    return static_cast<int16_t>(v < 0 ? 0 : (v > size - 1 ? size - 1 : v));
}

inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

inline FilterOutput hold_output(const FilterState& s) {
    FilterOutput out{};
    out.pressed = s.pressed;
    out.count = s.pressed ? s.count : 0;
    out.points[0] = s.last[0];
    out.points[1] = s.last[1];
    return out;
}

inline FilterOutput release(FilterState& s, bool forced) {
    const bool was_pressed = s.pressed;
    s.pressed = false;
    s.has_jump = false;
    s.confirm = 0;
    s.no_contact = 0;
    s.busy = 0;
    s.count = 0;
    FilterOutput out = hold_output(s);
    out.release_edge = was_pressed;
    out.forced_release = was_pressed && forced;
    return out;
}

inline FilterOutput filter_step(FilterState& s, const FilterConfig& c, const Reading& r) {
    if (r.status == ReadStatus::kBusBusy) {
        if (!s.pressed) {
            s.confirm = 0;
            return hold_output(s);
        }
        const uint8_t misses = c.shared_miss_counter ? static_cast<uint8_t>(s.busy + s.no_contact) : s.busy;
        if (c.hold_on_bus_busy != 255 && misses >= c.hold_on_bus_busy) return release(s, true);
        if (s.busy < 254) s.busy++;
        return hold_output(s);
    }

    if (r.status == ReadStatus::kNoContact || r.count == 0) {
        if (!s.pressed) {
            s.confirm = 0;
            return hold_output(s);
        }
        if (c.shared_miss_counter) {
            if (static_cast<uint8_t>(s.busy + s.no_contact) >= c.hold_on_bus_busy) return release(s, false);
            s.no_contact++;
            return hold_output(s);
        }
        s.no_contact++;
        if (s.no_contact >= c.release_after_no_contact) return release(s, false);
        return hold_output(s);
    }

    // Contact.
    Point p[2];
    const uint8_t count = r.count > 2 ? 2 : r.count;
    for (uint8_t i = 0; i < 2; ++i) {
        p[i] = i < count ? Point{clamp_coord(r.points[i].x, c.size), clamp_coord(r.points[i].y, c.size)} : s.last[i];
    }

    if (c.jump_reject_px > 0 && s.pressed &&
        (abs32(p[0].x - s.last[0].x) > c.jump_reject_px || abs32(p[0].y - s.last[0].y) > c.jump_reject_px)) {
        const bool confirms_jump = s.has_jump && abs32(p[0].x - s.jump.x) <= c.jump_reject_px &&
                                   abs32(p[0].y - s.jump.y) <= c.jump_reject_px;
        if (!confirms_jump) {
            s.has_jump = true;
            s.jump = p[0];
            return hold_output(s);  // drop the sample, keep everything as it was
        }
    }
    s.has_jump = false;

    const bool was_pressed = s.pressed;
    if (!s.pressed) {
        if (s.confirm > 0 && (abs32(p[0].x - s.last[0].x) > c.press_confirm_radius ||
                              abs32(p[0].y - s.last[0].y) > c.press_confirm_radius)) {
            s.confirm = 0;  // unstable first packets: start a new candidate here
        }
        s.confirm++;
        s.last[0] = p[0];
        s.last[1] = p[1];
        if (s.confirm < c.press_confirm_samples) return hold_output(s);
        s.pressed = true;
        s.confirm = 0;
    }

    s.no_contact = 0;
    s.busy = 0;
    s.count = count;
    s.last[0] = p[0];
    s.last[1] = p[1];
    FilterOutput out = hold_output(s);
    out.fresh = true;
    out.press_edge = !was_pressed;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// CST92xx report (CST9217 on the Waveshare board), as SensorLib's TouchDrvCST92xx parses it: 15 bytes read from
// register 0xD000. Finger 1 = bytes 0-4, ACK byte 6 = 0xAB, finger count = byte 5 & 0x7F, finger 2 = bytes 7-11.
// Finger bytes: [0] = id << 4 | event (0x06 = pressed), x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0x0F.
// An empty, foreign or not-pressed report is "no contact" (the bus worked); bus failures are decided by the caller.

constexpr uint8_t kCst92xxAddress = 0x5A;
constexpr uint8_t kCst92xxReportLength = 15;
constexpr uint8_t kCst92xxAck = 0xAB;

inline Reading parse_cst92xx(const uint8_t* d, bool mirror_x, bool mirror_y, int16_t size) {
    Reading r{ReadStatus::kNoContact, 0, {{0, 0}, {0, 0}}};
    if (d[6] != kCst92xxAck) return r;
    const uint8_t count = d[5] & 0x7F;
    if (count == 0 || count > 2) return r;
    if ((d[0] & 0x0F) != 0x06 || (d[0] >> 4) >= 2) return r;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* f = d + i * 5 + (i == 0 ? 0 : 2);
        if (i > 0 && ((f[0] & 0x0F) != 0x06 || (f[0] >> 4) >= 2)) break;
        int32_t x = (f[1] << 4) | (f[3] >> 4);
        int32_t y = (f[2] << 4) | (f[3] & 0x0F);
        if (mirror_x) x = size - 1 - x;
        if (mirror_y) y = size - 1 - y;
        r.points[i] = Point{clamp_coord(x, size), clamp_coord(y, size)};
        r.count = static_cast<uint8_t>(i + 1);
    }
    r.status = ReadStatus::kContact;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Gestures: one finished press -> tap / swipe, plus long press while held. All coordinates are VISUAL (upright UI).

enum class GestureType : uint8_t { kNone, kTap, kSwipeLeft, kSwipeRight, kSwipeUp, kSwipeDown, kLongPress };

struct GestureConfig {
    // Tap: shorter than tap_max_ms (exclusive) and moved less than tap_max_move_px (exclusive) on both axes.
    uint32_t tap_max_ms = 450;
    int16_t tap_max_move_px = 30;
    // Swipe: the dominant axis moved MORE than min_px; when dominance_num/den is set the dominant axis must also
    // exceed the other times num/den; when cross_max_px is set the other axis must stay within it.
    int16_t swipe_min_x_px = 60;
    int16_t swipe_min_y_px = 70;
    uint8_t dominance_num = 3;
    uint8_t dominance_den = 2;
    int16_t cross_max_px = 0;  // 0 = no limit
    bool vertical_swipes = true;
    uint32_t swipe_min_ms = 0;
    uint16_t swipe_min_samples = 0;
    int16_t edge_guard_px = 0;  // swipes must START at least this far from the left/right edges (0 = off)
    int16_t size = 480;
    bool multi_touch_cancels = true;  // a press that ever had 2 fingers is not a tap or swipe
    // Long press: held at least long_press_ms within long_press_radius_px of the start (0 = off).
    uint32_t long_press_ms = 0;
    int16_t long_press_radius_px = 25;
};

struct PressSummary {
    Point start;
    Point end;
    uint32_t duration_ms;
    uint16_t samples;
    uint8_t max_fingers;
};

inline GestureType classify_press(const GestureConfig& c, const PressSummary& p) {
    if (c.multi_touch_cancels && p.max_fingers > 1) return GestureType::kNone;
    const int32_t dx = p.end.x - p.start.x;
    const int32_t dy = p.end.y - p.start.y;
    const int32_t ax = abs32(dx);
    const int32_t ay = abs32(dy);

    const bool swipe_timing = p.duration_ms >= c.swipe_min_ms && p.samples >= c.swipe_min_samples;
    const bool in_edge_band =
        c.edge_guard_px > 0 && (p.start.x < c.edge_guard_px || p.start.x >= c.size - c.edge_guard_px);
    if (swipe_timing && !in_edge_band) {
        const bool x_dominant =
            ax > c.swipe_min_x_px && (c.dominance_den == 0 || ax * c.dominance_den > ay * c.dominance_num) &&
            (c.cross_max_px == 0 || ay <= c.cross_max_px);
        if (x_dominant) return dx < 0 ? GestureType::kSwipeLeft : GestureType::kSwipeRight;
        const bool y_dominant = c.vertical_swipes && ay > c.swipe_min_y_px &&
                                (c.dominance_den == 0 || ay * c.dominance_den > ax * c.dominance_num) &&
                                (c.cross_max_px == 0 || ax <= c.cross_max_px);
        if (y_dominant) return dy < 0 ? GestureType::kSwipeUp : GestureType::kSwipeDown;
    }
    if (p.duration_ms < c.tap_max_ms && ax < c.tap_max_move_px && ay < c.tap_max_move_px) return GestureType::kTap;
    return GestureType::kNone;
}

// True once a held press qualifies as a long press (caller fires it once per press).
inline bool is_long_press(const GestureConfig& c, Point start, Point now, uint32_t held_ms) {
    return c.long_press_ms > 0 && held_ms >= c.long_press_ms && abs32(now.x - start.x) <= c.long_press_radius_px &&
           abs32(now.y - start.y) <= c.long_press_radius_px;
}

}  // namespace hande_touch_policy
