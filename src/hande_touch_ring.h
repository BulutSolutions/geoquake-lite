#pragma once

// Touch rings (hande_touch.cpp): fixed storage handed in by the owner, pure C++ (host-tested).

#include <stdint.h>

namespace hande_touch_ring {

// Drop-oldest ring over storage the owner allocates (attach).
template <typename T>
struct Ring {
    T* items = nullptr;
    uint8_t size = 0;
    uint8_t head = 0;
    uint8_t count = 0;
    void attach(T* storage, uint8_t n) {
        items = storage;
        size = storage ? n : 0;
        head = count = 0;
    }
    T& at(uint8_t i) { return items[(head + i) % size]; }
    // Returns false when something was dropped (or the ring has no storage).
    bool push(const T& item) {
        if (size == 0) return false;
        const bool dropped = count == size;
        if (dropped) {
            head = static_cast<uint8_t>((head + 1) % size);
            count--;
        }
        items[(head + count) % size] = item;
        count++;
        return !dropped;
    }
    bool pop(T* out) {
        if (count == 0) return false;
        *out = items[head];
        head = static_cast<uint8_t>((head + 1) % size);
        count--;
        return true;
    }
    void clear() { head = count = 0; }
};

template <typename S>
bool is_move(const S& s) {
    return !s.press_edge && !s.release_edge;
}

// Sampler -> LVGL while LVGL is behind (a long frame). Gestures are recognised from what LVGL consumes, so a press's
// edges stay paired and in order. A full ring lets the newest plain move stand for the moves in between (LVGL sees
// one jump); a move after an edge waits for room; an edge takes the place of the most recent plain move (gestures
// end at the release point, so a lost move can only cost a swipe its minimum sample count, never turn it into another
// gesture). A merged or dropped move hands its finger count to a neighbour of the same press (a second finger
// cancels a gesture). A ring of edges only (some 32 taps during one stall) gives up its oldest complete tap.
// Returns false when something was merged or dropped.
template <typename S>
bool push_keeping_edges(Ring<S>& r, const S& s) {
    if (r.size == 0) return false;
    if (r.count < r.size) return r.push(s);
    if (is_move(s)) {
        S& newest = r.at(r.count - 1);  // a move follows its own press's edge or move
        const uint8_t fingers = newest.count > s.count ? newest.count : s.count;
        if (is_move(newest)) newest = s;
        newest.count = fingers;
        return false;
    }
    for (int i = r.count - 1; i >= 0; --i) {  // drop the most recent plain move, keep the order of the rest
        if (!is_move(r.at(i))) continue;
        // The neighbour before a move is its press edge or an earlier move; a move at the head is followed by its
        // own press's move or release.
        S& same_press = r.at(i > 0 ? i - 1 : i + 1);
        if (r.at(i).count > same_press.count) same_press.count = r.at(i).count;
        for (int j = i; j < r.count - 1; ++j) r.at(j) = r.at(j + 1);
        r.count--;
        r.push(s);
        return false;
    }
    for (int i = 0; i + 1 < r.count; ++i) {  // edges only: the oldest press edge with its release
        if (!r.at(i).press_edge || !r.at(i + 1).release_edge) continue;
        for (int j = i; j + 2 < r.count; ++j) r.at(j) = r.at(j + 2);
        r.count -= 2;
        r.push(s);
        return false;
    }
    return false;
}

}  // namespace hande_touch_ring
