#include "splash_screen.h"
#include "version.h"
#include "ui_touch.h"
#include <Arduino.h>
#include <math.h>

LV_FONT_DECLARE(font_bruno_32);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_18);

namespace {

lv_obj_t* s_splash_root = nullptr;
lv_obj_t* s_logo_platter = nullptr;
lv_obj_t* s_epicenter_core = nullptr;
lv_obj_t* s_epicenter_spark = nullptr;
lv_obj_t* s_wave_ring_inner = nullptr;
lv_obj_t* s_wave_ring_outer = nullptr;
lv_obj_t* s_axis_n = nullptr;
lv_obj_t* s_axis_s = nullptr;
lv_obj_t* s_axis_w = nullptr;
lv_obj_t* s_axis_e = nullptr;
lv_obj_t* s_progress_bar = nullptr;
lv_obj_t* s_status_label = nullptr;
lv_obj_t* s_title_label = nullptr;
lv_obj_t* s_sub_label = nullptr;
lv_obj_t* s_ver_label = nullptr;

uint32_t s_anim_start_ms = 0;
volatile bool s_dismiss_requested = false;

static void splash_click_cb(lv_event_t*) {
    s_dismiss_requested = true;
}

static inline float ease_out_cubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float f = t - 1.0f;
    return f * f * f + 1.0f;
}

static inline float ease_out_back(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float c1 = 1.70158f;
    float c3 = c1 + 1.0f;
    float f = t - 1.0f;
    return 1.0f + c3 * f * f * f + c1 * f * f;
}

static inline float clamp01(float val) {
    if (val < 0.0f) return 0.0f;
    if (val > 1.0f) return 1.0f;
    return val;
}

static void splash_apply_anim(uint32_t elapsed) {
    if (!s_logo_platter) return;

    // 1. Platter aura bloom (0ms -> 1000ms)
    float p_t = clamp01(static_cast<float>(elapsed) / 1000.0f);
    int32_t shadow_w = static_cast<int32_t>(p_t * 24.0f);
    uint8_t shadow_opa = static_cast<uint8_t>(p_t * 90.0f);
    lv_obj_set_style_shadow_width(s_logo_platter, shadow_w, 0);
    lv_obj_set_style_shadow_opa(s_logo_platter, shadow_opa, 0);

    // 2. Radial Fault Axes extension (0ms -> 600ms)
    float ax_t = clamp01(static_cast<float>(elapsed) / 600.0f);
    float ax_prog = ease_out_cubic(ax_t);
    uint8_t ax_opa = static_cast<uint8_t>(ax_prog * 180.0f); // 70% opacity
    int32_t ax_offset = static_cast<int32_t>(12.0f + ax_prog * 14.0f); // 12 -> 26px

    if (s_axis_n) {
        lv_obj_align(s_axis_n, LV_ALIGN_CENTER, 0, -ax_offset);
        lv_obj_set_style_opa(s_axis_n, ax_opa, 0);
    }
    if (s_axis_s) {
        lv_obj_align(s_axis_s, LV_ALIGN_CENTER, 0, ax_offset);
        lv_obj_set_style_opa(s_axis_s, ax_opa, 0);
    }
    if (s_axis_w) {
        lv_obj_align(s_axis_w, LV_ALIGN_CENTER, -ax_offset, 0);
        lv_obj_set_style_opa(s_axis_w, ax_opa, 0);
    }
    if (s_axis_e) {
        lv_obj_align(s_axis_e, LV_ALIGN_CENTER, ax_offset, 0);
        lv_obj_set_style_opa(s_axis_e, ax_opa, 0);
    }

    // 3. Inner P-Wave Shockwave Ring expansion (150ms -> 850ms)
    if (s_wave_ring_inner) {
        float in_t = clamp01(static_cast<float>((int32_t)elapsed - 150) / 700.0f);
        float in_scale = ease_out_cubic(in_t) * 256.0f;
        uint8_t in_opa = static_cast<uint8_t>((1.0f - in_t * 0.3f) * clamp01(in_t * 3.0f) * 230.0f);
        lv_obj_set_style_transform_scale(s_wave_ring_inner, static_cast<int32_t>(in_scale), 0);
        lv_obj_set_style_opa(s_wave_ring_inner, in_opa, 0);
    }

    // 4. Outer S-Wave Shockwave Ring expansion (350ms -> 1050ms)
    if (s_wave_ring_outer) {
        float out_t = clamp01(static_cast<float>((int32_t)elapsed - 350) / 700.0f);
        float out_scale = ease_out_cubic(out_t) * 256.0f;
        uint8_t out_opa = static_cast<uint8_t>((1.0f - out_t * 0.5f) * clamp01(out_t * 3.0f) * 160.0f);
        lv_obj_set_style_transform_scale(s_wave_ring_outer, static_cast<int32_t>(out_scale), 0);
        lv_obj_set_style_opa(s_wave_ring_outer, out_opa, 0);
    }

    // 5. Epicenter Core Diamond Ignition (450ms -> 1100ms)
    if (s_epicenter_core) {
        float core_t = clamp01(static_cast<float>((int32_t)elapsed - 450) / 650.0f);
        float core_prog = ease_out_back(core_t);
        if (core_prog < 0.0f) core_prog = 0.0f;
        int32_t core_scale = static_cast<int32_t>(core_prog * 256.0f);
        uint8_t core_opa = static_cast<uint8_t>(clamp01(core_t * 2.0f) * 255.0f);
        lv_obj_set_style_transform_scale(s_epicenter_core, core_scale, 0);
        lv_obj_set_style_opa(s_epicenter_core, core_opa, 0);
    }

    // 6. Apex Focal Spark (700ms -> 1150ms)
    if (s_epicenter_spark) {
        float sp_t = clamp01(static_cast<float>((int32_t)elapsed - 700) / 450.0f);
        float sp_prog = ease_out_back(sp_t);
        if (sp_prog < 0.0f) sp_prog = 0.0f;
        int32_t sp_scale = static_cast<int32_t>(sp_prog * 256.0f);
        uint8_t sp_opa = static_cast<uint8_t>(clamp01(sp_t * 2.5f) * 255.0f);
        lv_obj_set_style_transform_scale(s_epicenter_spark, sp_scale, 0);
        lv_obj_set_style_opa(s_epicenter_spark, sp_opa, 0);
    }
}

static void splash_apply_cyclic_anim(uint32_t t_ms) {
    if (!s_logo_platter) return;

    const uint32_t ASSEMBLE_DUR = 1100;
    const uint32_t HOLD_DUR = 1600;
    const uint32_t RETRACT_DUR = 600;
    const uint32_t PAUSE_DUR = 300;
    const uint32_t CYCLE_PERIOD = ASSEMBLE_DUR + HOLD_DUR + RETRACT_DUR + PAUSE_DUR;  // 3600ms

    const uint32_t phase_t = t_ms % CYCLE_PERIOD;

    if (phase_t <= ASSEMBLE_DUR) {
        // Phase 1: Seismic Wavefront Reveal (0ms -> 1100ms)
        splash_apply_anim(phase_t);
    } else if (phase_t <= ASSEMBLE_DUR + HOLD_DUR) {
        // Phase 2: Grand Seismic Hold (1100ms -> 2700ms, 1.6s full radiance)
        splash_apply_anim(ASSEMBLE_DUR);

        // Subtle kinetic epicenter heartbeat during hold
        const uint32_t hold_elapsed = phase_t - ASSEMBLE_DUR;
        const float pulse_f = static_cast<float>(hold_elapsed % 800) / 800.0f;
        const float pulse_scale = 1.0f + 0.08f * sinf(pulse_f * 6.283185f);
        if (s_epicenter_core) {
            lv_obj_set_style_transform_scale(s_epicenter_core, static_cast<int32_t>(pulse_scale * 256.0f), 0);
        }
    } else if (phase_t <= ASSEMBLE_DUR + HOLD_DUR + RETRACT_DUR) {
        // Phase 3: Lithospheric Retract (2700ms -> 3300ms, 0.6s)
        float r_t = clamp01(static_cast<float>(phase_t - (ASSEMBLE_DUR + HOLD_DUR)) / static_cast<float>(RETRACT_DUR));
        float r_prog = ease_out_cubic(r_t);

        // Retract rings & core
        int32_t core_scale = static_cast<int32_t>((1.0f - r_prog) * 256.0f);
        uint8_t core_opa = static_cast<uint8_t>((1.0f - r_prog) * 255.0f);
        if (s_epicenter_core) {
            lv_obj_set_style_transform_scale(s_epicenter_core, core_scale, 0);
            lv_obj_set_style_opa(s_epicenter_core, core_opa, 0);
        }
        if (s_epicenter_spark) {
            lv_obj_set_style_transform_scale(s_epicenter_spark, core_scale, 0);
            lv_obj_set_style_opa(s_epicenter_spark, core_opa, 0);
        }
        if (s_wave_ring_inner) {
            lv_obj_set_style_transform_scale(s_wave_ring_inner, core_scale, 0);
            lv_obj_set_style_opa(s_wave_ring_inner, core_opa, 0);
        }
        if (s_wave_ring_outer) {
            lv_obj_set_style_transform_scale(s_wave_ring_outer, core_scale, 0);
            lv_obj_set_style_opa(s_wave_ring_outer, core_opa, 0);
        }

        // Retract fault axes
        int32_t ax_offset = static_cast<int32_t>(26.0f - r_prog * 14.0f);
        uint8_t ax_opa = static_cast<uint8_t>((1.0f - r_prog) * 180.0f);
        if (s_axis_n) { lv_obj_align(s_axis_n, LV_ALIGN_CENTER, 0, -ax_offset); lv_obj_set_style_opa(s_axis_n, ax_opa, 0); }
        if (s_axis_s) { lv_obj_align(s_axis_s, LV_ALIGN_CENTER, 0, ax_offset); lv_obj_set_style_opa(s_axis_s, ax_opa, 0); }
        if (s_axis_w) { lv_obj_align(s_axis_w, LV_ALIGN_CENTER, -ax_offset, 0); lv_obj_set_style_opa(s_axis_w, ax_opa, 0); }
        if (s_axis_e) { lv_obj_align(s_axis_e, LV_ALIGN_CENTER, ax_offset, 0); lv_obj_set_style_opa(s_axis_e, ax_opa, 0); }

        int32_t shadow_w = static_cast<int32_t>((1.0f - r_prog) * 24.0f);
        uint8_t shadow_opa = static_cast<uint8_t>((1.0f - r_prog) * 90.0f);
        lv_obj_set_style_shadow_width(s_logo_platter, shadow_w, 0);
        lv_obj_set_style_shadow_opa(s_logo_platter, shadow_opa, 0);
    } else {
        // Phase 4: Calm Reset (3300ms -> 3600ms)
        splash_apply_anim(0);
    }
}

}  // namespace

void splash_screen_show() {
    if (s_splash_root) return;

    s_anim_start_ms = millis();

    // Pure-black AMOLED base on lv_layer_top()
    s_splash_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_splash_root, 480, 480);
    lv_obj_align(s_splash_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_splash_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_splash_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash_root, 0, 0);
    lv_obj_set_style_pad_all(s_splash_root, 0, 0);
    lv_obj_remove_flag(s_splash_root, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Hero Platter (Deep Space Cyan Radar)
    s_logo_platter = lv_obj_create(s_splash_root);
    lv_obj_set_size(s_logo_platter, 104, 104);
    lv_obj_align(s_logo_platter, LV_ALIGN_CENTER, 0, -68);
    lv_obj_set_style_bg_color(s_logo_platter, lv_color_hex(0x070D14), 0);
    lv_obj_set_style_bg_opa(s_logo_platter, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_logo_platter, 28, 0);
    lv_obj_set_style_border_color(s_logo_platter, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_border_width(s_logo_platter, 2, 0);
    lv_obj_set_style_shadow_color(s_logo_platter, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_shadow_width(s_logo_platter, 0, 0);
    lv_obj_set_style_shadow_spread(s_logo_platter, 2, 0);
    lv_obj_set_style_shadow_opa(s_logo_platter, 0, 0);
    lv_obj_set_style_pad_all(s_logo_platter, 0, 0);
    lv_obj_remove_flag(s_logo_platter, LV_OBJ_FLAG_SCROLLABLE);

    // Outer S-Wave Shockwave Ring
    s_wave_ring_outer = lv_obj_create(s_logo_platter);
    lv_obj_set_size(s_wave_ring_outer, 68, 68);
    lv_obj_align(s_wave_ring_outer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_wave_ring_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_wave_ring_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_wave_ring_outer, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_border_width(s_wave_ring_outer, 1, 0);
    lv_obj_set_style_transform_pivot_x(s_wave_ring_outer, 34, 0);
    lv_obj_set_style_transform_pivot_y(s_wave_ring_outer, 34, 0);
    lv_obj_remove_flag(s_wave_ring_outer, LV_OBJ_FLAG_SCROLLABLE);

    // Inner P-Wave Shockwave Ring
    s_wave_ring_inner = lv_obj_create(s_logo_platter);
    lv_obj_set_size(s_wave_ring_inner, 44, 44);
    lv_obj_align(s_wave_ring_inner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_wave_ring_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_wave_ring_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_wave_ring_inner, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_border_width(s_wave_ring_inner, 2, 0);
    lv_obj_set_style_transform_pivot_x(s_wave_ring_inner, 22, 0);
    lv_obj_set_style_transform_pivot_y(s_wave_ring_inner, 22, 0);
    lv_obj_remove_flag(s_wave_ring_inner, LV_OBJ_FLAG_SCROLLABLE);

    // Radial Fault Axes
    auto create_axis = [&](int32_t w, int32_t h) {
        lv_obj_t* ax = lv_obj_create(s_logo_platter);
        lv_obj_set_size(ax, w, h);
        lv_obj_set_style_bg_color(ax, lv_color_hex(0x06B6D4), 0);
        lv_obj_set_style_bg_opa(ax, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ax, 1, 0);
        lv_obj_set_style_border_width(ax, 0, 0);
        lv_obj_set_style_pad_all(ax, 0, 0);
        lv_obj_remove_flag(ax, LV_OBJ_FLAG_SCROLLABLE);
        return ax;
    };
    s_axis_n = create_axis(2, 16);
    s_axis_s = create_axis(2, 16);
    s_axis_w = create_axis(16, 2);
    s_axis_e = create_axis(16, 2);

    // Epicenter Core Diamond (45 degrees rotated)
    s_epicenter_core = lv_obj_create(s_logo_platter);
    lv_obj_set_size(s_epicenter_core, 18, 18);
    lv_obj_align(s_epicenter_core, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_epicenter_core, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_bg_opa(s_epicenter_core, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_epicenter_core, 3, 0);
    lv_obj_set_style_border_width(s_epicenter_core, 0, 0);
    lv_obj_set_style_pad_all(s_epicenter_core, 0, 0);
    lv_obj_set_style_transform_rotation(s_epicenter_core, 450, 0);
    lv_obj_set_style_transform_pivot_x(s_epicenter_core, 9, 0);
    lv_obj_set_style_transform_pivot_y(s_epicenter_core, 9, 0);
    lv_obj_remove_flag(s_epicenter_core, LV_OBJ_FLAG_SCROLLABLE);

    // Apex Focal Spark
    s_epicenter_spark = lv_obj_create(s_logo_platter);
    lv_obj_set_size(s_epicenter_spark, 6, 6);
    lv_obj_align(s_epicenter_spark, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_epicenter_spark, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_epicenter_spark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_epicenter_spark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_epicenter_spark, 0, 0);
    lv_obj_set_style_pad_all(s_epicenter_spark, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_epicenter_spark, 3, 0);
    lv_obj_set_style_transform_pivot_y(s_epicenter_spark, 3, 0);
    lv_obj_remove_flag(s_epicenter_spark, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Main Title (Bruno Ace 32)
    s_title_label = lv_label_create(s_splash_root);
    lv_label_set_text(s_title_label, "GEOQUAKE LITE");
    lv_obj_set_style_text_font(s_title_label, &font_bruno_32, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_letter_space(s_title_label, 2, 0);
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, 24);

    // 3. Subtitle
    s_sub_label = lv_label_create(s_splash_root);
    lv_label_set_text(s_sub_label, "GLOBAL SEISMIC MONITOR");
    lv_obj_set_style_text_font(s_sub_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sub_label, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_text_letter_space(s_sub_label, 2, 0);
    lv_obj_align(s_sub_label, LV_ALIGN_CENTER, 0, 54);

    // 4. Progress Bar
    s_progress_bar = lv_bar_create(s_splash_root);
    lv_obj_set_size(s_progress_bar, 180, 4);
    lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 108);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x141A23), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x06B6D4), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(s_progress_bar, 10, LV_ANIM_OFF);

    // 5. Status Label
    s_status_label = lv_label_create(s_splash_root);
    lv_label_set_text(s_status_label, "Calibrating seismic sensors...");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8B949E), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);

    // 6. Version Footer
    s_ver_label = lv_label_create(s_splash_root);
    char ver_buf[32];
    snprintf(ver_buf, sizeof(ver_buf), "v%s", FIRMWARE_VERSION_STRING);
    lv_label_set_text(s_ver_label, ver_buf);
    lv_obj_set_style_text_font(s_ver_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ver_label, lv_color_hex(0x484F58), 0);
    lv_obj_align(s_ver_label, LV_ALIGN_BOTTOM_MID, 0, -26);

    // Initialize elements at t=0
    splash_apply_anim(0);

    // Move to top layer
    lv_obj_move_to_index(s_splash_root, -1);
    lv_timer_handler();
}

void splash_screen_update(const char* status, int progress) {
    if (!s_splash_root) return;

    lv_obj_move_foreground(s_splash_root);

    if (s_status_label && status && status[0]) {
        lv_label_set_text(s_status_label, status);
        lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);
    }
    if (s_progress_bar && progress >= 0) {
        if (progress > 100) progress = 100;
        lv_bar_set_value(s_progress_bar, progress, LV_ANIM_OFF);
    }

    lv_timer_handler();
}

static bool s_is_ready = false;
static uint32_t s_ready_time_ms = 0;

void splash_screen_set_ready() {
    if (!s_splash_root || s_is_ready) return;
    s_is_ready = true;
    s_ready_time_ms = millis();

    if (s_progress_bar) {
        lv_bar_set_value(s_progress_bar, 100, LV_ANIM_OFF);
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, "TAP ANYWHERE TO START");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x06B6D4), 0);
        lv_obj_set_style_text_letter_space(s_status_label, 1, 0);
        lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 130);
    }

    s_dismiss_requested = false;
    lv_obj_add_flag(s_splash_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_splash_root, splash_click_cb, LV_EVENT_CLICKED, nullptr);
}

void splash_screen_dismiss() {
    if (!s_splash_root) return;

    // Zero-lag handoff: immediately delete splash root in one frame (< 15ms)
    ui_disable_input_temporarily(300);

    lv_obj_delete(s_splash_root);
    s_splash_root = nullptr;
    s_logo_platter = nullptr;
    s_epicenter_core = nullptr;
    s_epicenter_spark = nullptr;
    s_wave_ring_inner = nullptr;
    s_wave_ring_outer = nullptr;
    s_axis_n = nullptr;
    s_axis_s = nullptr;
    s_axis_w = nullptr;
    s_axis_e = nullptr;
    s_progress_bar = nullptr;
    s_status_label = nullptr;
    s_title_label = nullptr;
    s_sub_label = nullptr;
    s_ver_label = nullptr;
    s_is_ready = false;
    s_dismiss_requested = false;

    ui_touch_update_interaction_time();
}

// How long the ready splash ("TAP ANYWHERE TO START") waits for a tap.
static constexpr uint32_t SPLASH_AUTO_DISMISS_MS = 10000;

void splash_screen_tick() {
    if (!s_splash_root) return;

    const uint32_t elapsed = millis() - s_anim_start_ms;

    // Safety timeout: after 7.5 seconds, arm ready even if network stalled
    if (!s_is_ready && elapsed > 7500) {
        splash_screen_set_ready();
    }

    if (!s_is_ready) {
        // Continuous living animation cycle while loading
        splash_apply_cyclic_anim(elapsed);
        return;
    }

    // System is ready: run repeating wavefront loop and breathing prompt pulse
    const uint32_t ready_elapsed = millis() - s_ready_time_ms;
    splash_apply_cyclic_anim(ready_elapsed);

    const float phase = static_cast<float>(ready_elapsed % 2000) / 2000.0f;
    const uint8_t pulse_opa = static_cast<uint8_t>(120.0f + 135.0f * (0.5f + 0.5f * sinf(phase * 6.283185f)));
    if (s_status_label) {
        lv_obj_set_style_text_opa(s_status_label, pulse_opa, 0);
    }

    // Without a tap the splash used to stay on lv_layer_top() forever, redrawing
    // its scaled/translucent layers every frame on top of the map (also with the
    // panel asleep at 80 MHz) until one refresh outlasted the task watchdog.
    if (s_dismiss_requested || ui_touch_is_pressed() ||
        ready_elapsed >= SPLASH_AUTO_DISMISS_MS) {
        if (!s_dismiss_requested && !ui_touch_is_pressed()) {
            Serial.println("[SPLASH] No tap; continuing to the map");
        }
        splash_screen_dismiss();
    }
}

void splash_screen_finish(bool wait_for_tap, uint32_t hold_ms) {
    if (!s_splash_root) return;

    splash_screen_set_ready();

    if (wait_for_tap) {
        // Drain touches for clean release
        uint32_t release_time = millis();
        while (millis() - release_time < 250) {
            lv_timer_handler();
            if (ui_touch_is_pressed()) {
                release_time = millis();
            }
            delay(15);
        }

        // Loop living animation until tap
        while (!s_dismiss_requested) {
            splash_screen_tick();
            lv_timer_handler();
            if (ui_touch_is_pressed()) {
                break;
            }
            delay(15);
        }

        splash_screen_dismiss();
    } else {
        const uint32_t anim_start = millis();
        while (millis() - anim_start < 1200) {
            uint32_t elapsed = millis() - anim_start;
            splash_apply_anim(elapsed);
            lv_timer_handler();
            delay(15);
        }
        splash_apply_anim(1200);
        lv_timer_handler();
        delay(hold_ms);
        splash_screen_dismiss();
    }
}

bool splash_screen_is_active() {
    return s_splash_root != nullptr;
}
