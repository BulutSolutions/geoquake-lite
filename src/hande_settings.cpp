#include "hande_settings.h"

#include "hande_system_menu.h"
#include "version.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// Look: fill means interactive: rows sit on the black canvas, grouped by a muted
// section title, spacing and 1 px dividers (no card fills); only controls (switch, slider, segments, buttons), the
// selected segment and a pressed row are filled. Off-white text #F2F5F8 / #D0D7DE / #9DA7B5, 56 px targets + 16 px
// extended click area, no PRESSING on sliders. Gestures: sliders keep horizontal drags (a vertical swipe scrolls
// the list) and a swipe is never a tap (tap guard). The gear stays on app screens for hours: icon only (dim), on a
// translucent black disc so it reads over maps and photos.
// Navigation: hub of category tiles (app first, then the common DEVICE categories) -> category page -> deeper pages
// from link rows. Back = the top-left button or a swipe to the right; close = top right. A pushed page slides in
// from the right, a popped one from the left (a short 96 px slide of the content only: cheap to render).

namespace {

constexpr uint32_t kCanvas = 0x000000;
constexpr uint32_t kSurfaceSubtle = 0x0C0E14;
constexpr uint32_t kPressed = 0x141720;   // a row while the finger is on it
constexpr uint32_t kBorder = 0x242A38;
constexpr uint32_t kTextPrimary = 0xF2F5F8;
constexpr uint32_t kTextSecondary = 0xD0D7DE;
constexpr uint32_t kTextMuted = 0x9DA7B5;
constexpr uint32_t kDanger = 0xFF8A9B;
constexpr uint32_t kDangerBorder = 0x5A2430;

constexpr int32_t kScreen = 480;
constexpr int32_t kHeader = 72;
constexpr int32_t kButton = 56;
constexpr int32_t kExtClick = 16;
constexpr int32_t kTouchRow = 56;        // a control's row: its click area cannot leave the row, so >= 56 px tall
constexpr int32_t kSliderExtClick = 24;  // 8 px track + 24 above + 24 margin below = a 56 px finger band
constexpr int32_t kFadeH = 28;           // black fade under the header while a page is scrolled
constexpr int32_t kGearMargin = 16;
constexpr int32_t kRowInset = 8;      // rows and section titles sit 8 px in from the dividers
// Touch slop: a press that travels further than this is a swipe, never a tap (Android's 8 dp at this panel's
// ~314 ppi; LVGL itself clicks on release whenever the list did not scroll, however far the finger moved).
constexpr int32_t kTapSlop = 16;
constexpr uint8_t kMaxViews = 48;
constexpr uint8_t kMaxDepth = 4;      // the hub + 3 levels
constexpr uint8_t kMaxTiles = 8;      // app tiles (up to 4) + Display, Clock & HUD, Wi-Fi, About
constexpr int32_t kTileW = 219;       // two columns in the 448 px content width, 10 px apart
constexpr int32_t kTileH = 76;
constexpr int32_t kSlidePx = 96;
constexpr uint32_t kSlideMs = 180;
constexpr uint32_t kConfirmMs = 3000;

const lv_font_t* const kTitleFont = &lv_font_montserrat_24;
const lv_font_t* const kBodyFont = &lv_font_montserrat_20;
const lv_font_t* const kSmallFont = &lv_font_montserrat_16;

struct RowView {
    const HandeSettingsRow* row;
    lv_obj_t* widget;  // switch / slider / segment row / button
    lv_obj_t* value;   // value label (slider, stepper, info, link) or button label (action)
    bool armed;        // action waiting for its confirming tap
    uint32_t armed_at;
};

// One level of the navigation stack; page == nullptr is the hub.
struct Level {
    const HandeSettingsPage* page;
    const char* title;
    int32_t scroll_y;  // kept while a deeper page is shown, restored on the way back
};

struct TileView {
    const HandeSettingsTile* tile;
    lv_obj_t* obj;
    lv_obj_t* summary;
};

HandeSettingsConfig s_cfg{};
lv_obj_t* s_screen = nullptr;
lv_obj_t* s_previous = nullptr;
lv_obj_t* s_content = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_subtitle = nullptr;
lv_obj_t* s_back = nullptr;
lv_obj_t* s_fade = nullptr;
lv_timer_t* s_confirm_timer = nullptr;
Level s_stack[kMaxDepth];
uint8_t s_depth = 0;
RowView s_views[kMaxViews];
uint8_t s_view_count = 0;
HandeSettingsTile s_tiles[kMaxTiles];   // built at open: the app's tiles, then the common ones
uint8_t s_tile_count = 0;
uint8_t s_app_tile_count = 0;
TileView s_tile_views[kMaxTiles];
uint8_t s_tile_view_count = 0;
char s_group_title[24];                 // the app name in capitals

uint64_t s_visible_mask = 0;       // which rows of the page are shown (re-checked after every change)
bool s_rebuild_queued = false;
int8_t s_pending_nav = 0;          // a level change waits for the event to end: +1 pushed, -1 popped
uint32_t s_last_refresh_ms = 0;
constexpr uint32_t kRefreshMs = 2000;   // info values (Wi-Fi, IP) stay current while open

lv_indev_t* s_fed_indev = nullptr;
bool s_fed_pressed = false;
lv_point_t s_fed_point{0, 0};

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

// A layout box: no style, no scrolling, not clickable. lv_obj_create() makes every object clickable, and LVGL gives a
// press to the deepest clickable object under the finger: a clickable layout box inside a link row or tile took the
// tap, so only the gaps between the words opened the page. Rows and tiles that act on a tap add CLICKABLE back.
lv_obj_t* make_box(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

lv_obj_t* make_text(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t rgb) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(rgb), 0);
    return label;
}

// A horizontal row: left block grows, right block hugs its content.
lv_obj_t* make_row(lv_obj_t* parent, int32_t min_height) {
    lv_obj_t* row = make_box(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, min_height, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_hor(row, kRowInset, 0);
    return row;
}

lv_obj_t* make_label_block(lv_obj_t* row, const HandeSettingsRow& r, const lv_font_t* font, uint32_t rgb) {
    lv_obj_t* block = make_box(row);
    lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(block, 1);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 2, 0);
    lv_obj_t* label = make_text(block, r.label, font, rgb);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    if (r.detail) make_text(block, r.detail, kSmallFont, kTextMuted);
    return block;
}

lv_obj_t* make_pill_button(lv_obj_t* parent, int32_t w, int32_t h, uint32_t border) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, color(kSurfaceSubtle), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, color(border), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, color(kBorder), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return btn;
}

const char* number_text(const HandeSettingsRow& r, int32_t v, char* buf, size_t len) {
    if (r.format) return r.format(v, buf, len);
    snprintf(buf, len, "%ld", static_cast<long>(v));
    return buf;
}

int32_t clamp_value(const HandeSettingsRow& r, int32_t v) {
    return v < r.min ? r.min : (v > r.max ? r.max : v);
}

RowView* view_of(lv_event_t* e) {
    const uintptr_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    return i < s_view_count ? &s_views[i] : nullptr;
}

void* index_arg(uint8_t i) { return reinterpret_cast<void*>(static_cast<uintptr_t>(i)); }

// ---- value refresh ---------------------------------------------------------------------------------------

void style_segments(RowView& v, int32_t selected) {
    const uint32_t n = lv_obj_get_child_count(v.widget);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* seg = lv_obj_get_child(v.widget, static_cast<int32_t>(i));
        const bool on = static_cast<int32_t>(i) == selected;
        lv_obj_set_style_bg_opa(seg, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t* text = lv_obj_get_child(seg, 0);
        lv_obj_set_style_text_color(text, color(on ? kTextPrimary : kTextMuted), 0);
    }
}

void refresh_view(RowView& v) {
    const HandeSettingsRow& r = *v.row;
    char buf[48];
    switch (r.type) {
        case HandeSettingsRowType::kToggle:
            if (r.get_bool && r.get_bool()) lv_obj_add_state(v.widget, LV_STATE_CHECKED);
            else lv_obj_remove_state(v.widget, LV_STATE_CHECKED);
            break;
        case HandeSettingsRowType::kSlider: {
            const int32_t value = clamp_value(r, r.get_int ? r.get_int() : r.min);
            lv_slider_set_value(v.widget, value, LV_ANIM_OFF);
            lv_label_set_text(v.value, number_text(r, value, buf, sizeof(buf)));
            break;
        }
        case HandeSettingsRowType::kStepper: {
            const int32_t value = clamp_value(r, r.get_int ? r.get_int() : r.min);
            lv_label_set_text(v.value, number_text(r, value, buf, sizeof(buf)));
            break;
        }
        case HandeSettingsRowType::kChoice:
            style_segments(v, r.get_int ? r.get_int() : 0);
            break;
        case HandeSettingsRowType::kInfo:
        case HandeSettingsRowType::kLink:
            if (v.value) lv_label_set_text(v.value, r.value_text ? r.value_text(buf, sizeof(buf)) : "");
            break;
        default:
            break;
    }
}

void refresh_tile(TileView& t) {
    char buf[48];
    if (t.summary && t.tile->summary) lv_label_set_text(t.summary, t.tile->summary(buf, sizeof(buf)));
}

// ---- tap guard -----------------------------------------------------------------------------------------
// LVGL sends PRESSED / SHORT_CLICKED / CLICKED / LONG_PRESSED(_REPEAT) to the input device's own event list before
// the object. While Settings is shown, a press that moved beyond kTapSlop from where it went down is cancelled
// there: the row, switch, segment or stepper under the finger gets no click and no long press (the platform rule:
// a swipe never activates what it started on). Sliders take horizontal drags themselves (see build_slider).

lv_point_t s_press_point{0, 0};
bool s_press_cancelled = false;
bool s_slider_dragged = false;  // this press moved a slider while the finger was down

void on_indev_event(lv_event_t* e) {
    lv_indev_t* indev = static_cast<lv_indev_t*>(lv_event_get_current_target(e));
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_param(e));
    if (!s_screen || !obj || lv_obj_get_screen(obj) != s_screen) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_press_point = p;
        s_press_cancelled = false;
        s_slider_dragged = false;
        return;
    }
    if (!s_press_cancelled) {
        const int32_t dx = p.x - s_press_point.x;
        const int32_t dy = p.y - s_press_point.y;
        s_press_cancelled = dx * dx + dy * dy > kTapSlop * kTapSlop;
    }
    if (s_press_cancelled) lv_indev_stop_processing(indev);
}

void guard_taps(lv_indev_t* indev) {
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_indev_remove_event_cb_with_user_data(indev, on_indev_event, nullptr);  // once per device
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_SHORT_CLICKED, LV_EVENT_CLICKED, LV_EVENT_LONG_PRESSED,
                                 LV_EVENT_LONG_PRESSED_REPEAT}) {
        lv_indev_add_event_cb(indev, on_indev_event, code, nullptr);
    }
}

// ---- events --------------------------------------------------------------------------------------------

void value_changed();

void on_toggle(lv_event_t* e) {
    RowView* v = view_of(e);
    if (v && v->row->set_bool) v->row->set_bool(lv_obj_has_state(v->widget, LV_STATE_CHECKED));
    value_changed();
}

// LVGL flips a CHECKABLE object on RELEASED whenever nothing scrolled, before any click event, so the tap guard
// cannot stop it: a swipe that started on a switch toggled it. Our switches are not CHECKABLE; they flip on CLICKED,
// which the tap guard cancels for a swipe. VALUE_CHANGED then runs the switch's own animation and on_toggle.
void on_switch_clicked(lv_event_t* e) {
    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

// True when the finger has travelled beyond the tap slop since it went down (the press is a swipe).
bool press_travelled() {
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int32_t dx = p.x - s_press_point.x;
    const int32_t dy = p.y - s_press_point.y;
    return dx * dx + dy * dy > kTapSlop * kTapSlop;
}

// ---- slider guard (every slider in Settings, also an app's own in a custom row) -----------------------------
// - Horizontal drags stay in the slider (no horizontal scroll chain); a vertical swipe that starts on it scrolls
//   the list (vertical chain kept until the slider really drags, then claimed until release, so vertical wobble
//   cannot hand a drag to the list).
// - LVGL's slider jumps to the finger on release ("tap to set") whenever nothing scrolled: on a page that fits the
//   screen a vertical swipe over a slider set it. Tap-to-set stays for a tap, every value of a real drag stays;
//   a release after a swipe that never dragged the slider puts the press-time value back and the app's own
//   VALUE_CHANGED handlers never see it (pre-process callback + stop processing). LVGL's own "dragged" flag cannot
//   tell: its release path sets it just before this event.
int32_t s_slider_press_value = 0;

void on_slider_guard(lv_event_t* e) {
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            s_slider_press_value = lv_slider_get_value(slider);
            break;
        case LV_EVENT_VALUE_CHANGED: {
            lv_indev_t* indev = lv_indev_active();
            const bool released = !indev || lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED;
            if (!released) {
                s_slider_dragged = true;
                lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
            } else if (!s_slider_dragged && press_travelled()) {
                lv_slider_set_value(slider, s_slider_press_value, LV_ANIM_OFF);
                lv_event_stop_processing(e);
            }
            break;
        }
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            lv_obj_add_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
            break;
        default:
            break;
    }
}

void on_slider(lv_event_t* e) {
    RowView* v = view_of(e);
    if (!v) return;
    const int32_t value = lv_slider_get_value(v->widget);
    char buf[48];
    lv_label_set_text(v->value, number_text(*v->row, value, buf, sizeof(buf)));
    if (v->row->set_int) v->row->set_int(value);
}

void on_slider_release(lv_event_t* e) {
    RowView* v = view_of(e);
    if (!v) return;
    if (v->row->commit_int) v->row->commit_int(lv_slider_get_value(v->widget));
    value_changed();
}

void on_segment(lv_event_t* e) {
    RowView* v = view_of(e);
    if (!v) return;
    lv_obj_t* seg = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const int32_t index = lv_obj_get_index(seg);
    if (v->row->set_int) v->row->set_int(index);
    style_segments(*v, index);
    value_changed();
}

void step(lv_event_t* e, int32_t direction) {
    RowView* v = view_of(e);
    if (!v || !v->row->get_int || !v->row->set_int) return;
    const HandeSettingsRow& r = *v->row;
    int32_t value = r.get_int() + direction * (r.step > 0 ? r.step : 1);
    if (r.wrap) value = value > r.max ? r.min : (value < r.min ? r.max : value);
    r.set_int(clamp_value(r, value));
    refresh_view(*v);
    value_changed();
}
void on_step_down(lv_event_t* e) { step(e, -1); }
void on_step_up(lv_event_t* e) { step(e, 1); }

void disarm(RowView& v) {
    v.armed = false;
    lv_label_set_text(v.value, v.row->label);
}

void on_confirm_timer(lv_timer_t*) {
    const uint32_t now = millis();
    if (s_screen && now - s_last_refresh_ms >= kRefreshMs) {
        s_last_refresh_ms = now;
        hande_settings_refresh();
    }
    for (uint8_t i = 0; i < s_view_count; ++i) {
        if (s_views[i].armed && now - s_views[i].armed_at >= kConfirmMs) disarm(s_views[i]);
    }
}

void on_action(lv_event_t* e) {
    RowView* v = view_of(e);
    if (!v) return;
    if (v->row->confirm && !v->armed) {
        v->armed = true;
        v->armed_at = millis();
        lv_label_set_text(v->value, v->row->confirm);
        return;
    }
    if (v->armed) disarm(*v);
    if (v->row->on_action) v->row->on_action();
    if (s_screen) {
        hande_settings_refresh();
        value_changed();
    }
}

void update_fade() {
    if (!s_fade || !s_content) return;
    if (lv_obj_get_scroll_y(s_content) > 0) lv_obj_remove_flag(s_fade, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_fade, LV_OBJ_FLAG_HIDDEN);
}
void on_content_scroll(lv_event_t*) { update_fade(); }

void show_level(int8_t slide);
uint64_t visibility_mask(const HandeSettingsPage* page);

// Level changes and rebuilds delete the widget whose event is running: do them after the event, never inside it.
void apply_pending(void*) {
    s_rebuild_queued = false;
    if (!s_screen) return;
    const int8_t nav = s_pending_nav;
    s_pending_nav = 0;
    if (nav) {
        show_level(nav);
        return;
    }
    if (visibility_mask(s_stack[s_depth].page) == s_visible_mask) return;
    s_stack[s_depth].scroll_y = lv_obj_get_scroll_y(s_content);
    show_level(0);
}

void queue_pending() {
    if (s_rebuild_queued) return;
    s_rebuild_queued = true;
    lv_async_call(apply_pending, nullptr);
}

// After a value changed: other rows may appear or disappear (visible()).
void value_changed() { queue_pending(); }

void push(const HandeSettingsPage* page, const char* title) {
    if (!page || s_depth + 1 >= kMaxDepth || s_pending_nav) return;
    s_stack[s_depth].scroll_y = lv_obj_get_scroll_y(s_content);
    s_stack[++s_depth] = Level{page, title, 0};
    s_pending_nav = 1;
    queue_pending();
}

void go_back() {
    if (s_pending_nav) return;
    if (s_depth == 0) {
        hande_settings_close();
        return;
    }
    --s_depth;
    s_pending_nav = -1;
    queue_pending();
}

void on_link(lv_event_t* e) {
    RowView* v = view_of(e);
    if (v) push(v->row->page, v->row->page && v->row->page->title ? v->row->page->title : v->row->label);
}

void on_tile(lv_event_t* e) {
    const uintptr_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (i < s_tile_count) push(s_tiles[i].page, s_tiles[i].title);
}

void on_back(lv_event_t*) { go_back(); }
void on_close(lv_event_t*) { hande_settings_close(); }

bool on_slider_obj(lv_obj_t* obj) {
    for (; obj && obj != s_screen; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_check_type(obj, &lv_slider_class)) return true;
    }
    return false;
}

// A swipe to the right anywhere goes back (watch and phone convention). LVGL only reports a gesture for a quick
// drag that did not scroll. Sliders keep their own horizontal drags, also an app's own slider in a custom row.
void on_gesture(lv_event_t*) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    if (on_slider_obj(lv_indev_get_active_obj())) return;
    lv_indev_wait_release(indev);  // the finger is still down: nothing under it may act on the rest of this press
    go_back();
}

void on_gear(lv_event_t* e) {
    auto cb = reinterpret_cast<void (*)()>(lv_event_get_user_data(e));
    if (cb) cb();
}

// ---- building ------------------------------------------------------------------------------------------

// A section's rows: no fill and no frame (black canvas); the title above, spacing and dividers group them.
lv_obj_t* make_group(lv_obj_t* parent) {
    lv_obj_t* group = make_box(parent);
    lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(group, 10, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    return group;
}

void make_divider(lv_obj_t* card) {
    lv_obj_t* line = make_box(card);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, color(kBorder), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

void build_switch(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* row = make_row(card, kTouchRow);
    make_label_block(row, *v.row, kBodyFont, kTextPrimary);
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 30);
    lv_obj_set_style_bg_color(sw, color(kBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, color(s_cfg.accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, color(kTextMuted), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, color(kCanvas), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, kExtClick);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CHECKABLE);  // flips on CLICKED (tap guard), see on_switch_clicked
    lv_obj_add_event_cb(sw, on_switch_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(sw, on_toggle, LV_EVENT_VALUE_CHANGED, index_arg(index));
    v.widget = sw;
}

void build_slider(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* column = make_box(card);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_pad_hor(column, kRowInset, 0);
    lv_obj_t* row = make_row(column, 28);
    lv_obj_set_style_pad_hor(row, 0, 0);
    make_label_block(row, *v.row, kBodyFont, kTextPrimary);
    v.value = make_text(row, "", kSmallFont, kTextSecondary);

    lv_obj_t* slider = lv_slider_create(column);
    lv_obj_set_size(slider, LV_PCT(100), 8);
    // A child's click area cannot leave its parent (lv_indev_search_obj), so the margins make the touch band.
    lv_obj_set_style_margin_top(slider, 10, 0);
    lv_obj_set_style_margin_bottom(slider, kSliderExtClick, 0);
    lv_slider_set_range(slider, v.row->min, v.row->max);
    lv_obj_set_style_bg_color(slider, color(kBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color(s_cfg.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color(kTextPrimary), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, kSliderExtClick);
    hande_settings_slider_guard(slider);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, index_arg(index));
    lv_obj_add_event_cb(slider, on_slider_release, LV_EVENT_RELEASED, index_arg(index));
    lv_obj_add_event_cb(slider, on_slider_release, LV_EVENT_PRESS_LOST, index_arg(index));
    v.widget = slider;
}

void build_choice(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* column = make_box(card);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 10, 0);
    lv_obj_set_style_pad_hor(column, kRowInset, 0);
    lv_obj_t* row = make_row(column, 28);
    lv_obj_set_style_pad_hor(row, 0, 0);
    make_label_block(row, *v.row, kBodyFont, kTextPrimary);

    lv_obj_t* segments = make_box(column);
    lv_obj_set_size(segments, LV_PCT(100), LV_SIZE_CONTENT);
    const bool two_lines = v.row->choice_count > 4;
    lv_obj_set_flex_flow(segments, two_lines ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_row(segments, 4, 0);
    lv_obj_set_style_pad_all(segments, 4, 0);
    lv_obj_set_style_pad_column(segments, 4, 0);
    lv_obj_set_style_radius(segments, 12, 0);
    lv_obj_set_style_bg_color(segments, color(kSurfaceSubtle), 0);
    lv_obj_set_style_bg_opa(segments, LV_OPA_COVER, 0);
    for (uint8_t i = 0; i < v.row->choice_count; ++i) {
        lv_obj_t* seg = lv_button_create(segments);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, 44);
        if (two_lines) lv_obj_set_width(seg, LV_PCT(32));
        else lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 9, 0);
        lv_obj_set_style_bg_color(seg, color(kBorder), 0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_t* text = make_text(seg, v.row->choices[i], kSmallFont, kTextMuted);
        lv_obj_center(text);
        lv_obj_add_event_cb(seg, on_segment, LV_EVENT_CLICKED, index_arg(index));
    }
    v.widget = segments;
}

// Holding [-]/[+] repeats (after LVGL's long-press time, then every repeat time), like a platform stepper's
// autorepeat; hande_row_tap_only() rows step once per tap instead.
void add_step_events(lv_obj_t* button, lv_event_cb_t cb, const HandeSettingsRow& r, uint8_t index) {
    if (!r.repeat) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, index_arg(index));
        return;
    }
    // A short tap steps on release; a hold steps at the long press and on every repeat (no extra step on release).
    lv_obj_add_event_cb(button, cb, LV_EVENT_SHORT_CLICKED, index_arg(index));
    lv_obj_add_event_cb(button, cb, LV_EVENT_LONG_PRESSED, index_arg(index));
    lv_obj_add_event_cb(button, cb, LV_EVENT_LONG_PRESSED_REPEAT, index_arg(index));
}

void build_stepper(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* row = make_row(card, kTouchRow);
    make_label_block(row, *v.row, kBodyFont, kTextPrimary);
    lv_obj_t* group = make_box(row);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(group, (kTouchRow - 44) / 2, 0);  // room for the buttons' click areas (56 px)
    lv_obj_set_style_pad_right(row, kRowInset - (kTouchRow - 44) / 2, 0);  // the + button lines up with switches
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(group, 8, 0);
    lv_obj_t* down = make_pill_button(group, 44, 44, kBorder);
    lv_obj_center(make_text(down, LV_SYMBOL_MINUS, kSmallFont, kTextSecondary));
    add_step_events(down, on_step_down, *v.row, index);
    v.value = make_text(group, "", kBodyFont, kTextPrimary);
    lv_obj_set_style_min_width(v.value, 48, 0);
    lv_obj_set_style_text_align(v.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* up = make_pill_button(group, 44, 44, kBorder);
    lv_obj_center(make_text(up, LV_SYMBOL_PLUS, kSmallFont, kTextSecondary));
    add_step_events(up, on_step_up, *v.row, index);
    v.widget = group;
}

void build_info(RowView& v, lv_obj_t* card) {
    lv_obj_t* row = make_row(card, 32);
    make_text(row, v.row->label, kSmallFont, kTextSecondary);
    v.value = make_text(row, "", kSmallFont, kTextPrimary);
    lv_label_set_long_mode(v.value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v.value, 260, 0);
    v.widget = row;
}

void build_action(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* btn = make_pill_button(card, LV_PCT(100), 52, v.row->danger ? kDangerBorder : kBorder);
    lv_obj_set_style_radius(btn, 14, 0);
    v.value = make_text(btn, v.row->label, kSmallFont, v.row->danger ? kDanger : kTextPrimary);
    lv_obj_center(v.value);
    lv_obj_add_event_cb(btn, on_action, LV_EVENT_CLICKED, index_arg(index));
    v.widget = btn;
}

void build_link(RowView& v, lv_obj_t* card, uint8_t index) {
    lv_obj_t* row = make_row(card, 44);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 6);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, color(kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    make_label_block(row, *v.row, kBodyFont, kTextPrimary);
    lv_obj_t* right = make_box(row);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);
    v.value = make_text(right, "", kSmallFont, kTextMuted);
    make_text(right, LV_SYMBOL_RIGHT, kSmallFont, kTextMuted);
    lv_obj_add_event_cb(row, on_link, LV_EVENT_CLICKED, index_arg(index));
    v.widget = row;
}

void build_custom(RowView& v, lv_obj_t* card) {
    lv_obj_t* box = make_box(card);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_style_pad_hor(box, kRowInset, 0);
    if (v.row->label && v.row->label[0]) make_text(box, v.row->label, kBodyFont, kTextPrimary);
    if (v.row->build) v.row->build(box);
    v.widget = box;
}

void add_row(lv_obj_t* card, const HandeSettingsRow& r) {
    if (s_view_count >= kMaxViews) return;
    const uint8_t index = s_view_count++;
    RowView& v = s_views[index];
    v = RowView{&r, nullptr, nullptr, false, 0};
    switch (r.type) {
        case HandeSettingsRowType::kToggle: build_switch(v, card, index); break;
        case HandeSettingsRowType::kSlider: build_slider(v, card, index); break;
        case HandeSettingsRowType::kChoice: build_choice(v, card, index); break;
        case HandeSettingsRowType::kStepper: build_stepper(v, card, index); break;
        case HandeSettingsRowType::kInfo: build_info(v, card); break;
        case HandeSettingsRowType::kAction: build_action(v, card, index); break;
        case HandeSettingsRowType::kLink: build_link(v, card, index); break;
        case HandeSettingsRowType::kCustom: build_custom(v, card); break;
    }
    refresh_view(v);
}

void add_section(const HandeSettingsSection& section) {
    uint8_t visible = 0;
    for (uint8_t i = 0; i < section.row_count; ++i) {
        if (!section.rows[i].visible || section.rows[i].visible()) visible++;
    }
    if (visible == 0) return;

    if (section.title && section.title[0]) {
        lv_obj_t* title = make_text(s_content, section.title, kSmallFont, kTextMuted);
        lv_obj_set_style_text_letter_space(title, 1, 0);
        lv_obj_set_style_pad_left(title, kRowInset, 0);
        lv_obj_set_style_pad_top(title, 16, 0);
    }

    lv_obj_t* card = make_group(s_content);
    if (!section.title || !section.title[0]) lv_obj_set_style_pad_top(card, 12, 0);  // same air as under a title
    bool first = true;
    for (uint8_t i = 0; i < section.row_count; ++i) {
        const HandeSettingsRow& r = section.rows[i];
        if (r.visible && !r.visible()) continue;
        if (!first && r.type != HandeSettingsRowType::kAction) make_divider(card);
        first = false;
        add_row(card, r);
    }
}

bool row_shown(const HandeSettingsRow& r) { return !r.visible || r.visible(); }

uint64_t visibility_mask(const HandeSettingsPage* page) {
    if (!page) return 0;  // the hub: tiles never hide
    uint64_t mask = 0;
    uint8_t bit = 0;
    for (uint8_t s = 0; s < page->section_count; ++s) {
        const HandeSettingsSection& section = page->sections[s];
        for (uint8_t i = 0; i < section.row_count && bit < 64; ++i, ++bit) {
            if (row_shown(section.rows[i])) mask |= (1ULL << bit);
        }
    }
    return mask;
}

// ---- the hub --------------------------------------------------------------------------------------------

void add_group_title(const char* text, int32_t pad_top) {
    lv_obj_t* title = make_text(s_content, text, kSmallFont, kTextMuted);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_style_pad_left(title, kRowInset, 0);
    lv_obj_set_style_pad_top(title, pad_top, 0);
}

// A tile is a button (R2: it may carry a fill): 1 px ring on black, filled only while pressed. App tiles have the
// icon in the app's accent, the common ones in the neutral text colour, so "this app" and "this device" read apart.
void add_tile(lv_obj_t* grid, uint8_t index, bool app) {
    if (s_tile_view_count >= kMaxTiles) return;
    const HandeSettingsTile& t = s_tiles[index];
    lv_obj_t* tile = make_box(grid);
    lv_obj_set_size(tile, kTileW, kTileH);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_border_color(tile, color(kBorder), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_bg_color(tile, color(kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(tile, 14, 0);
    lv_obj_set_style_pad_column(tile, 10, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* icon = make_text(tile, t.icon, kTitleFont, app ? s_cfg.accent : kTextSecondary);
    lv_obj_set_width(icon, 28);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* text = make_box(tile);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 2, 0);
    // One line each: LV_LABEL_LONG_DOT only shortens a label whose height is fixed (else it wraps).
    lv_obj_t* name = make_text(text, t.title, kBodyFont, kTextPrimary);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, LV_PCT(100), lv_font_get_line_height(kBodyFont));
    TileView& view = s_tile_views[s_tile_view_count++];
    view = TileView{&t, tile, nullptr};
    if (t.summary) {
        view.summary = make_text(text, "", kSmallFont, kTextMuted);
        lv_label_set_long_mode(view.summary, LV_LABEL_LONG_DOT);
        lv_obj_set_size(view.summary, LV_PCT(100), lv_font_get_line_height(kSmallFont));
        refresh_tile(view);
    }
    lv_obj_add_event_cb(tile, on_tile, LV_EVENT_CLICKED, index_arg(index));
}

lv_obj_t* make_grid() {
    lv_obj_t* grid = make_box(s_content);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    return grid;
}

void build_hub() {
    if (s_app_tile_count) {
        add_group_title(s_group_title, 4);
        lv_obj_t* grid = make_grid();
        for (uint8_t i = 0; i < s_app_tile_count; ++i) add_tile(grid, i, true);
    }
    add_group_title("DEVICE", s_app_tile_count ? 12 : 4);
    lv_obj_t* grid = make_grid();
    for (uint8_t i = s_app_tile_count; i < s_tile_count; ++i) add_tile(grid, i, false);
}

void slide_x(void* obj, int32_t x) { lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), x, 0); }

void show_level(int8_t slide) {
    const Level& level = s_stack[s_depth];
    s_visible_mask = visibility_mask(level.page);
    s_view_count = 0;
    s_tile_view_count = 0;
    lv_anim_delete(s_content, slide_x);
    lv_obj_set_style_translate_x(s_content, 0, 0);
    lv_obj_clean(s_content);

    const bool root = s_depth == 0;
    lv_label_set_text(s_title, root ? "Settings" : level.title);
    const int32_t x = root ? 16 + kRowInset : 8 + kButton + 12;
    lv_obj_set_x(s_title, x);
    lv_obj_set_x(s_subtitle, x);
    if (root) lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);

    if (root) build_hub();
    else for (uint8_t i = 0; i < level.page->section_count; ++i) add_section(level.page->sections[i]);
    if (!root) {  // room under the last row of a long page; the hub fits the screen with up to 3 app tiles
        lv_obj_t* spacer = make_box(s_content);
        lv_obj_set_size(spacer, LV_PCT(100), 24);
    }
    lv_obj_update_layout(s_content);
    lv_obj_scroll_to_y(s_content, level.scroll_y, LV_ANIM_OFF);
    update_fade();

    if (slide == 0) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_content);
    lv_anim_set_exec_cb(&a, slide_x);
    lv_anim_set_values(&a, slide * kSlidePx, 0);
    lv_anim_set_duration(&a, kSlideMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

lv_obj_t* make_ring_button(int32_t x, const char* symbol, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(s_screen);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kButton, kButton);
    lv_obj_set_pos(btn, x, 8);
    lv_obj_set_style_radius(btn, kButton / 2, 0);
    lv_obj_set_style_border_color(btn, color(kBorder), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, color(kBorder), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(btn, kExtClick);
    lv_obj_center(make_text(btn, symbol, kBodyFont, kTextSecondary));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void build_screen() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, kScreen, kScreen);
    lv_obj_set_style_bg_color(s_screen, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);

    s_title = make_text(s_screen, "Settings", kTitleFont, kTextPrimary);
    lv_obj_set_pos(s_title, 16 + kRowInset, 12);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, kScreen - 2 * (8 + kButton + 12));
    s_subtitle = make_text(s_screen, s_cfg.app_name, kSmallFont, s_cfg.accent);
    lv_obj_set_pos(s_subtitle, 16 + kRowInset, 44);

    s_back = make_ring_button(8, LV_SYMBOL_LEFT, on_back);
    make_ring_button(kScreen - kButton - 8, LV_SYMBOL_CLOSE, on_close);

    s_content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, kHeader);
    lv_obj_set_size(s_content, kScreen, kScreen - kHeader);
    lv_obj_set_style_pad_hor(s_content, 16, 0);
    lv_obj_set_style_pad_row(s_content, 8, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    // AUTO: the bar stays while a page is taller than the screen, so a cut-off last row reads as "more below".
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_content, color(kTextMuted), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_50, LV_PART_SCROLLBAR);  // remove_style_all left it transparent
    lv_obj_set_style_width(s_content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_content, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(s_content, 12, LV_PART_SCROLLBAR);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_content, on_content_scroll, LV_EVENT_SCROLL, nullptr);

    // Rows scrolled up fade out under the header instead of ending as cut text fragments. Not clickable, so touches
    // reach the rows under it; hidden at the top of a page, where it would dim the first section title.
    s_fade = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_fade);
    lv_obj_remove_flag(s_fade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_fade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_fade, 0, kHeader);
    lv_obj_set_size(s_fade, kScreen, kFadeH);
    lv_obj_set_style_bg_color(s_fade, color(kCanvas), 0);
    lv_obj_set_style_bg_grad_color(s_fade, color(kCanvas), 0);
    lv_obj_set_style_bg_grad_dir(s_fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(s_fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(s_fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_fade, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_fade, LV_OBJ_FLAG_HIDDEN);
}

void fed_read_cb(lv_indev_t*, lv_indev_data_t* data) {
    data->point = s_fed_point;
    data->state = s_fed_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---- the common Wi-Fi and About pages (built here) ----------------------------------------------------------

bool wifi_connected() { return s_cfg.wifi_connected && s_cfg.wifi_connected(); }
const char* network_text(char* buf, size_t len) {
    if (s_cfg.wifi_state_text) {
        const char* state = s_cfg.wifi_state_text();
        if (state && state[0]) return state;
    }
    if (!s_cfg.wifi_connected) return "-";
    if (!s_cfg.wifi_connected()) return "Not connected";
    snprintf(buf, len, "%s", s_cfg.wifi_ssid ? s_cfg.wifi_ssid() : "");
    return buf;
}
const char* signal_text(char* buf, size_t len) {
    const int32_t rssi = s_cfg.wifi_rssi ? s_cfg.wifi_rssi() : -70;
    const char* quality = rssi >= -60 ? "Strong" : (rssi >= -72 ? "Good" : "Weak");
    snprintf(buf, len, "%s (%ld dBm)", quality, static_cast<long>(rssi));
    return buf;
}
const char* ip_text(char* buf, size_t len) {
    snprintf(buf, len, "%s", s_cfg.wifi_ip ? s_cfg.wifi_ip() : "");
    return buf;
}
const char* app_text(char* buf, size_t len) {
    snprintf(buf, len, "%s", s_cfg.app_name ? s_cfg.app_name : "");
    return buf;
}
const char* version_text(char* buf, size_t len) {
    snprintf(buf, len, "%s", HANDE_APP_VERSION_STRING);
    return buf;
}
const char* version_summary(char* buf, size_t len) {
    snprintf(buf, len, "Version %s", HANDE_APP_VERSION_STRING);
    return buf;
}
const char* more_apps_text(char*, size_t) { return "handeos.com"; }
void factory_reset() { hande_system_menu_open_reset(); }

constexpr HandeSettingsRow kWifiStatusRows[] = {
    hande_row_info("Network", network_text),
    hande_row_when(hande_row_info("Signal", signal_text), wifi_connected),
    hande_row_when(hande_row_info("IP address", ip_text), wifi_connected),
};
constexpr HandeSettingsRow kAboutAppRows[] = {
    hande_row_info("App", app_text),
    hande_row_info("Version", version_text),
    hande_row_info("More apps for this board", more_apps_text),
    hande_row_action("Factory reset", factory_reset, nullptr, true),
};
#define HANDE_ROWS(rows) rows, static_cast<uint8_t>(sizeof(rows) / sizeof(rows[0]))

// The app's extra section goes in the middle, so the shared pages need a few bytes of RAM (filled at open).
HandeSettingsSection s_wifi_sections[2];
HandeSettingsPage s_wifi_page{"Wi-Fi", s_wifi_sections, 0};
HandeSettingsSection s_about_sections[3];
HandeSettingsPage s_about_page{"About", s_about_sections, 0};

void build_tiles() {
    uint8_t n = 0;
    s_wifi_sections[n++] = HandeSettingsSection{"STATUS", HANDE_ROWS(kWifiStatusRows)};
    if (s_cfg.wifi_extra) s_wifi_sections[n++] = *s_cfg.wifi_extra;
    s_wifi_page.section_count = n;
    n = 0;
    s_about_sections[n++] = HandeSettingsSection{"APP", HANDE_ROWS(kAboutAppRows)};
    if (s_cfg.about_extra) s_about_sections[n++] = *s_cfg.about_extra;
    s_about_page.section_count = n;

    n = 0;
    for (uint8_t i = 0; i < s_cfg.tile_count && n < kMaxTiles - 4; ++i) s_tiles[n++] = s_cfg.tiles[i];
    s_app_tile_count = n;
    // The common categories: same names, icons and order.
    if (s_cfg.display) s_tiles[n++] = HandeSettingsTile{LV_SYMBOL_IMAGE, "Display", s_cfg.display, s_cfg.display_summary};
    if (s_cfg.clock) s_tiles[n++] = HandeSettingsTile{LV_SYMBOL_EYE_OPEN, "Clock & HUD", s_cfg.clock, s_cfg.clock_summary};
    s_tiles[n++] = HandeSettingsTile{LV_SYMBOL_WIFI, "Wi-Fi", &s_wifi_page, network_text};
    s_tiles[n++] = HandeSettingsTile{LV_SYMBOL_LIST, "About", &s_about_page, version_summary};
    s_tile_count = n;

    size_t i = 0;
    for (const char* c = s_cfg.app_name ? s_cfg.app_name : ""; *c && i + 1 < sizeof(s_group_title); ++c) {
        s_group_title[i++] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
    }
    s_group_title[i] = '\0';
}

}  // namespace

lv_obj_t* hande_settings_gear_create(lv_obj_t* parent, void (*on_click)()) {
    lv_obj_t* gear = lv_button_create(parent);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, kButton, kButton);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -kGearMargin, kGearMargin);
    lv_obj_set_style_radius(gear, kButton / 2, 0);
    // Black at 60 %: invisible on a black screen, darkens a map or photo behind the icon (fewer lit pixels, and the
    // icon stays readable). No ring and a dim icon: these pixels stay lit for hours (burn-in).
    lv_obj_set_style_bg_color(gear, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(gear, color(kBorder), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(gear, kExtClick);
    lv_obj_t* icon = make_text(gear, LV_SYMBOL_SETTINGS, kTitleFont, kTextMuted);
    lv_obj_center(icon);
    lv_obj_add_event_cb(gear, on_gear, LV_EVENT_CLICKED, reinterpret_cast<void*>(on_click));
    return gear;
}

void hande_settings_gear_area(lv_area_t* area) {
    area->x1 = kScreen - kGearMargin - kButton - kExtClick;
    area->y1 = kGearMargin - kExtClick;
    area->x2 = kScreen - kGearMargin + kExtClick - 1;
    area->y2 = kGearMargin + kButton + kExtClick - 1;
}

void hande_settings_open(const HandeSettingsConfig& config) {
    if (s_screen) return;
    s_cfg = config;
    build_tiles();
    s_depth = 0;
    s_stack[0] = Level{nullptr, "Settings", 0};
    s_previous = lv_screen_active();
    s_pending_nav = 0;
    s_last_refresh_ms = millis();
    build_screen();
    show_level(0);
    if (!s_confirm_timer) s_confirm_timer = lv_timer_create(on_confirm_timer, 500, nullptr);
    lv_screen_load(s_screen);
    for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) guard_taps(i);
    if (s_cfg.on_open) s_cfg.on_open();
    Serial.println("[SETTINGS] opened");
}

// hande_touch (weak, so a build without the touch module still links).
void hande_touch_cancel_gesture() __attribute__((weak));

void hande_settings_close() {
    if (!s_screen) return;
    // Settings may close while the finger is still down (swipe right on the hub): that press must not also become
    // an app gesture on release (a card moved back one once).
    if (hande_touch_cancel_gesture) hande_touch_cancel_gesture();
    lv_obj_t* screen = s_screen;
    s_screen = nullptr;
    s_content = nullptr;
    s_fade = nullptr;
    s_view_count = 0;
    s_tile_view_count = 0;
    s_depth = 0;
    if (s_previous) lv_screen_load(s_previous);
    lv_obj_delete_async(screen);
    s_fed_pressed = false;
    if (s_cfg.on_close) s_cfg.on_close();
    Serial.println("[SETTINGS] closed");
}

bool hande_settings_is_open() { return s_screen != nullptr; }
void hande_settings_slider_guard(lv_obj_t* slider) {
    if (!slider) return;
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(slider, on_slider_guard, static_cast<lv_event_code_t>(code | LV_EVENT_PREPROCESS), nullptr);
    }
}

uint8_t hande_settings_depth() { return s_screen ? s_depth : 0; }
const char* hande_settings_title() { return s_screen && s_title ? lv_label_get_text(s_title) : ""; }

void hande_settings_refresh() {
    for (uint8_t i = 0; i < s_view_count; ++i) refresh_view(s_views[i]);
    for (uint8_t i = 0; i < s_tile_view_count; ++i) refresh_tile(s_tile_views[i]);
    if (s_screen) queue_pending();  // a row may have appeared or gone (visible() reads app state too)
}

void hande_settings_set_accent(uint32_t accent) {
    if (s_cfg.accent == accent) return;
    s_cfg.accent = accent;
    if (!s_screen) return;
    lv_obj_set_style_text_color(s_subtitle, color(accent), 0);
    s_visible_mask = ~s_visible_mask;  // force a repaint of the page in the new colour
    queue_pending();
}

void hande_settings_touch(bool pressed, int16_t x, int16_t y) {
    if (!s_fed_indev) {
        s_fed_indev = lv_indev_create();
        lv_indev_set_type(s_fed_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_fed_indev, fed_read_cb);
        guard_taps(s_fed_indev);
    }
    s_fed_pressed = pressed && s_screen != nullptr;
    s_fed_point.x = x;
    s_fed_point.y = y;
    // Read now: LVGL polls every 33 ms, and a quick tap between two polls would otherwise be lost.
    lv_indev_read(s_fed_indev);
}
