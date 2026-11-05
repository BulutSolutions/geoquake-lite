#pragma once
// The System Menu's behaviour, without any hardware or LVGL: the BOOT-button hold, the timings and the
// screen layout. Plain C++, so it is tested on the host.

#include <cstdint>

namespace hande_system_menu {
constexpr uint8_t kButtonPin = 0;            // BOOT, active low
constexpr uint32_t kHoldMs = 5000U;          // hold this long to open the menu
constexpr uint32_t kDebounceMs = 30U;
constexpr uint32_t kChoiceTimeoutMs = 15000U;  // the menu closes by itself after this long untouched
constexpr int16_t kTouchSlop = 16;           // extra touch margin around every button

enum class HoldEvent : uint8_t { NONE, STARTED, CANCELLED, COMPLETED };

// Debounced BOOT hold. A button that is already down at reset() (held at power-on, or a stuck strap) must be
// released once before a hold can start.
struct ButtonHold {
    uint32_t candidate_since = 0;
    uint32_t hold_since = 0;
    bool candidate_down = false;
    bool stable_down = false;
    bool armed = true;
    bool tracking = false;
    uint8_t progress = 0;  // 0..100 while a hold is being tracked

    void reset(bool down, uint32_t now) {
        candidate_since = hold_since = now;
        candidate_down = stable_down = down;
        armed = !down;
        tracking = false;
        progress = 0;
    }

    HoldEvent update(bool down, uint32_t now) {
        if (down != candidate_down) {
            candidate_down = down;
            candidate_since = now;
        }
        HoldEvent event = HoldEvent::NONE;
        if (candidate_down != stable_down &&
            static_cast<uint32_t>(now - candidate_since) >= kDebounceMs) {
            stable_down = candidate_down;
            if (stable_down && armed) {
                armed = false;
                tracking = true;
                hold_since = now;
                progress = 0;
                event = HoldEvent::STARTED;
            } else if (!stable_down) {
                armed = true;
                if (tracking) event = HoldEvent::CANCELLED;
                tracking = false;
                progress = 0;
            }
        }
        // Never complete a hold while a real release is being debounced.
        if (tracking && stable_down && candidate_down) {
            const uint32_t elapsed = now - hold_since;
            progress = elapsed >= kHoldMs ? 100U : static_cast<uint8_t>(elapsed / (kHoldMs / 100U));
            if (elapsed >= kHoldMs) {
                tracking = false;
                event = HoldEvent::COMPLETED;
            }
        }
        return event;
    }
};

enum class Page : uint8_t { HOLD, CHOICES, RESET_CONFIRM };
enum class Action : uint8_t { NONE, RESTART, CANCEL, RESET_PROMPT, RESET_CONFIRMED, OUTSIDE };

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x - kTouchSlop && px < x + w + kTouchSlop && py >= y - kTouchSlop && py < y + h + kTouchSlop;
    }
};

// 480 x 480 layout.
constexpr Rect kRestartButton{48, 176, 176, 64};
constexpr Rect kCancelButton{256, 176, 176, 64};
constexpr Rect kResetButton{152, 272, 176, 64};
constexpr Rect kConfirmCancelButton{48, 298, 176, 64};
constexpr Rect kConfirmResetButton{256, 298, 176, 64};

// What a tap at (x, y) means on a page. A tap that hits no button is OUTSIDE (it closes the menu).
inline Action hit_test(Page page, int16_t x, int16_t y) {
    if (page == Page::CHOICES) {
        if (kRestartButton.contains(x, y)) return Action::RESTART;
        if (kCancelButton.contains(x, y)) return Action::CANCEL;
        if (kResetButton.contains(x, y)) return Action::RESET_PROMPT;
        return Action::OUTSIDE;
    }
    if (page == Page::RESET_CONFIRM) {
        if (kConfirmCancelButton.contains(x, y)) return Action::CANCEL;
        if (kConfirmResetButton.contains(x, y)) return Action::RESET_CONFIRMED;
        return Action::OUTSIDE;
    }
    return Action::NONE;
}

inline bool choice_expired(uint32_t now, uint32_t shown_since) {
    return static_cast<uint32_t>(now - shown_since) >= kChoiceTimeoutMs;
}
}  // namespace hande_system_menu
