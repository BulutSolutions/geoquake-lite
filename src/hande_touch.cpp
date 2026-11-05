#include "hande_touch.h"

#include "hande_display.h"
#include "hande_display_policy.h"
#include "hande_touch_ring.h"
#include "i2c_mutex.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace tp = hande_touch_policy;

namespace {

// Waveshare ESP32-S3-Touch-AMOLED-2.16: CST9217 on the shared I2C bus.
constexpr int kPinSda = 15;
constexpr int kPinScl = 14;
constexpr int kPinInt = 11;
constexpr int kPinReset = 40;
constexpr uint8_t kRegisterMsb = 0xD0;
constexpr uint8_t kRegisterLsb = 0x00;

constexpr uint8_t kLvglRingSize = 64;  // sampler -> LVGL, every press and release in order (push_keeping_edges)
constexpr uint8_t kQueueSize = 16;     // sampler -> consumer depth
constexpr uint8_t kGestureRingSize = 32;
// Bus health (HandeTouchStats). bus_errors: report reads that failed (information only: on Arduino-ESP32 2.0.17 a
// controller without a report and a bus fault look alike there). not_answering: the controller did not acknowledge
// its own address kTouchDeadStreak times in a row. That address-only ping runs in the sampler task every kProbeMs
// while nothing reads the controller (no finger, INT idle); never a report read: one without INT could hand back a
// stale contact.
constexpr uint16_t kTouchDeadStreak = 3;
constexpr uint32_t kProbeMs = 60u * 1000u;

HandeTouchConfig s_cfg;
tp::FilterState s_filter;
lv_indev_t* s_indev = nullptr;
TaskHandle_t s_task = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_int_fired = false;
volatile bool s_int_attached = false;  // touch_isr is attached to INT (sampler wake or poll-mode latch)
volatile bool s_cancel_requested = false;

HandeTouchSample s_current{};
uint32_t s_last_activity_ms = 0;
HandeTouchStats s_stats{};
uint16_t s_probe_fails = 0;  // failed address pings in a row, under s_mux
uint32_t s_last_bus_ms = 0;  // last transfer attempt with the controller, under s_mux

using hande_touch_ring::Ring;

// Storage is allocated at init only for the deliveries an app turns on.
template <typename T>
bool allocate(Ring<T>& r, uint8_t n) {
    T* storage = static_cast<T*>(heap_caps_calloc(n, sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    r.attach(storage, n);
    return storage != nullptr;
}
Ring<HandeTouchSample> s_lvgl_ring;
Ring<HandeTouchSample> s_queue;
Ring<HandeTouchGesture> s_gestures;

// Gesture tracking for the press in progress (only track_gesture's context touches it: LVGL's read callback when
// LVGL takes the touch, else the reading context).
struct PressTrack {
    tp::Point start;
    tp::Point last;
    uint32_t start_ms;
    uint16_t samples;
    uint8_t max_fingers;
    bool long_fired;
    bool cancelled;
} s_press{};

void IRAM_ATTR touch_isr() {
    s_int_fired = true;
    if (s_task) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
}

// A report read finished (`ok`: it worked).
void note_bus(bool ok) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    s_last_bus_ms = now;
    if (ok) {
        s_probe_fails = 0;  // a working report read proves the controller answers
    } else {
        s_stats.bus_errors++;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool probe_due() {
    portENTER_CRITICAL(&s_mux);
    const uint32_t last = s_last_bus_ms;
    portEXIT_CRITICAL(&s_mux);
    return millis() - last >= kProbeMs;
}

// Address-only ping (no report read, no state change): does the controller still answer? Sampler task only.
void probe_controller(uint32_t lock_timeout_ms) {
    if (!i2c_mutex_lock(lock_timeout_ms)) return;  // the bus is busy: try at the next chance
    Wire.beginTransmission(tp::kCst92xxAddress);
    const bool answered = Wire.endTransmission() == 0;
    i2c_mutex_unlock();
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    s_last_bus_ms = now;
    if (answered) {
        s_probe_fails = 0;
    } else if (s_probe_fails < 0xffff && ++s_probe_fails == kTouchDeadStreak) {
        s_stats.not_answering++;
    }
    portEXIT_CRITICAL(&s_mux);
}

tp::Reading read_controller(uint32_t lock_timeout_ms) {
    const tp::Reading busy{tp::ReadStatus::kBusBusy, 0, {{0, 0}, {0, 0}}};
    const tp::Reading none{tp::ReadStatus::kNoContact, 0, {{0, 0}, {0, 0}}};
    if (!i2c_mutex_lock(lock_timeout_ms)) return busy;

    Wire.beginTransmission(tp::kCst92xxAddress);
    Wire.write(kRegisterMsb);
    Wire.write(kRegisterLsb);
    const uint8_t status = Wire.endTransmission(false);
    if (status != 0) {
        i2c_mutex_unlock();
        // A NAK on the data byte: the controller answered but has no report. Anything else is a bus failure.
        note_bus(status == 3);
        return status == 3 ? none : busy;
    }
    if (s_cfg.report_delay_us) delayMicroseconds(s_cfg.report_delay_us);
    const int received = Wire.requestFrom(static_cast<int>(tp::kCst92xxAddress), static_cast<int>(tp::kCst92xxReportLength));
    if (received != tp::kCst92xxReportLength) {
        while (Wire.available()) Wire.read();
        i2c_mutex_unlock();
        note_bus(false);
        return busy;
    }
    uint8_t data[tp::kCst92xxReportLength];
    for (uint8_t i = 0; i < tp::kCst92xxReportLength; ++i) data[i] = static_cast<uint8_t>(Wire.read());

    // Acknowledge the report so the controller publishes the next one (protocol step, not a contact).
    Wire.beginTransmission(tp::kCst92xxAddress);
    Wire.write(kRegisterMsb);
    Wire.write(kRegisterLsb);
    Wire.write(tp::kCst92xxAck);
    const bool acked = Wire.endTransmission() == 0;
    i2c_mutex_unlock();
    note_bus(acked);
    if (!acked) return busy;

    return tp::parse_cst92xx(data, s_cfg.mirror_x, s_cfg.mirror_y, s_cfg.size);
}

void push_gesture(tp::GestureType type, const tp::Point& start, const tp::Point& end, uint32_t now) {
    const HandeTouchGesture g{type, start, end, now};
    portENTER_CRITICAL(&s_mux);
    if (!s_gestures.push(g)) s_stats.gesture_overruns++;
    portEXIT_CRITICAL(&s_mux);
}

void track_gesture(const HandeTouchSample& s) {
    const tp::GestureConfig* g = s_cfg.gestures;
    if (s_cancel_requested) {
        s_cancel_requested = false;
        s_press.cancelled = true;
    }
    if (s.press_edge) {
        s_press = PressTrack{s.visual[0], s.visual[0], s.ms, 1, s.count, false, false};
        return;
    }
    // Fingers from every pressed sample: a merged move can arrive as a held one (hande_touch_ring.h).
    if (s.pressed && s.count > s_press.max_fingers) s_press.max_fingers = s.count;
    if (s.pressed && s.fresh) {
        s_press.last = s.visual[0];
        s_press.samples++;
        if (g && !s_press.long_fired && !s_press.cancelled &&
            tp::is_long_press(*g, s_press.start, s_press.last, s.ms - s_press.start_ms)) {
            s_press.long_fired = true;
            push_gesture(tp::GestureType::kLongPress, s_press.start, s_press.last, s.ms);
        }
        return;
    }
    if (s.release_edge) {  // moves may have been merged on the way to LVGL (hande_touch_ring.h)
        s_press.last = s.visual[0];  // the release carries the last contact
        if (s.count > s_press.max_fingers) s_press.max_fingers = s.count;  // and a merged move's fingers
    }
    if (s.release_edge && g && !s.forced_release && !s_press.cancelled && !s_press.long_fired) {
        const tp::PressSummary p{s_press.start, s_press.last, s.ms - s_press.start_ms, s_press.samples,
                                 s_press.max_fingers};
        const tp::GestureType type = tp::classify_press(*g, p);
        if (type != tp::GestureType::kNone) push_gesture(type, s_press.start, s_press.last, s.ms);
    }
}

// Filters one controller reading into a sample and updates everything that depends on it.
HandeTouchSample process(const tp::Reading& reading, uint32_t now) {
    const tp::FilterOutput f = tp::filter_step(s_filter, s_cfg.filter, reading);
    HandeTouchSample s{};
    s.pressed = f.pressed;
    s.count = f.count;
    s.press_edge = f.press_edge;
    s.release_edge = f.release_edge;
    s.forced_release = f.forced_release;
    s.fresh = f.fresh;
    s.ms = now;
    const uint8_t orientation = hande_display_get_orientation();
    for (uint8_t i = 0; i < 2; ++i) {
        s.raw[i] = f.points[i];
        const hande_display_policy::Point v =
            hande_display_policy::visual_point({f.points[i].x, f.points[i].y}, orientation, s_cfg.size);
        s.visual[i] = tp::Point{static_cast<int16_t>(v.x), static_cast<int16_t>(v.y)};
    }
    // When LVGL takes the touch, gestures are recognised as LVGL consumes each sample (deliver_to_lvgl), in order
    // with on_sample, which cancels and drains them: a sampler task runs ahead of LVGL, so a gesture recognised
    // here could be drained, or a cancel could land, on the wrong press.
    if (!(HANDE_TOUCH_LVGL && s_cfg.lvgl_indev)) track_gesture(s);

    portENTER_CRITICAL(&s_mux);
    s_current = s;
    if (s.pressed) s_last_activity_ms = now;
    s_stats.reads++;
    if (reading.status == tp::ReadStatus::kBusBusy) s_stats.bus_busy++;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

bool may_skip_read() {
    return !s_int_fired && !s_filter.pressed && s_filter.confirm == 0 && digitalRead(kPinInt) == HIGH;
}

#if HANDE_TOUCH_LVGL
void deliver_to_lvgl(const HandeTouchSample& s, lv_indev_data_t* data) {
    track_gesture(s);  // before on_sample: a release's gesture is queued when on_sample drains them
    HandeTouchLvglInput input{s.pressed, s.raw[0]};
    if (s_cfg.on_sample) s_cfg.on_sample(s, input);
    data->point.x = input.point.x;
    data->point.y = input.point.y;
    data->state = input.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void indev_read_cb(lv_indev_t*, lv_indev_data_t* data) {
    data->continue_reading = false;
    if (s_cfg.sampler_period_ms == 0) {
        const uint32_t now = millis();
        HandeTouchSample s;
        // The controller pulses INT low for each report; a pulse between two LVGL reads is latched by touch_isr,
        // so the skip below never hides a touch whose INT is already high again when LVGL looks.
        if (s_cfg.may_skip_idle_read && may_skip_read() && s_cfg.may_skip_idle_read()) {
            portENTER_CRITICAL(&s_mux);
            s = s_current;
            portEXIT_CRITICAL(&s_mux);
            s.press_edge = s.release_edge = s.forced_release = s.fresh = false;
            s.ms = now;
        } else {
            s_int_fired = false;  // before the read: a pulse during it is latched again for the next one
            const uint32_t timeout = s_cfg.lock_timeout_for_read ? s_cfg.lock_timeout_for_read() : s_cfg.lock_timeout_ms;
            const tp::Reading reading = read_controller(timeout);
            // The bus was busy, so the report is still unread: keep it pending for the next LVGL read.
            if (reading.status == tp::ReadStatus::kBusBusy) s_int_fired = true;
            s = process(reading, now);
        }
        deliver_to_lvgl(s, data);
        return;
    }

    // Sampler mode: hand LVGL every queued sample in order, then repeat the latest state.
    HandeTouchSample s;
    bool more = false;
    portENTER_CRITICAL(&s_mux);
    const bool popped = s_lvgl_ring.pop(&s);
    if (!popped) {
        s = s_current;
        s.press_edge = s.release_edge = s.forced_release = s.fresh = false;
    }
    more = s_lvgl_ring.count > 0;
    portEXIT_CRITICAL(&s_mux);
    deliver_to_lvgl(s, data);
    data->continue_reading = more;
}
#endif  // HANDE_TOUCH_LVGL

void sampler_task(void*) {
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(s_cfg.sampler_period_ms);
    for (;;) {
        if (s_cfg.sampler_wake_on_int) {
            ulTaskNotifyTake(pdTRUE, period);
            if (may_skip_read()) {
                if (probe_due()) probe_controller(s_cfg.lock_timeout_ms);
                continue;
            }
            s_int_fired = false;
        } else {
            vTaskDelayUntil(&last_wake, period);
        }

        const tp::Reading reading = read_controller(s_cfg.lock_timeout_ms);
        const HandeTouchSample s = process(reading, millis());
        const bool busy_hold = reading.status == tp::ReadStatus::kBusBusy && !s.release_edge;

        portENTER_CRITICAL(&s_mux);
        if (s_cfg.lvgl_indev && (s.pressed || s.release_edge)) {
            if (!hande_touch_ring::push_keeping_edges(s_lvgl_ring, s)) s_stats.sample_overruns++;
        }
        if (s_cfg.sample_queue && !busy_hold) {
            if (!s_queue.push(s)) s_stats.sample_overruns++;
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

}  // namespace

bool hande_touch_init(const HandeTouchConfig& config) {
    s_cfg = config;
    s_filter = tp::FilterState{};
    s_cfg.filter.size = config.size;

    if (config.reset_pin) {
        pinMode(kPinReset, OUTPUT);
        digitalWrite(kPinReset, LOW);
        delay(10);
        digitalWrite(kPinReset, HIGH);
        delay(50);
    }
    pinMode(kPinInt, INPUT_PULLUP);

    const bool sampler = config.sampler_period_ms > 0;
    if ((sampler && config.lvgl_indev && !allocate(s_lvgl_ring, kLvglRingSize)) ||
        (config.sample_queue && !allocate(s_queue, kQueueSize)) ||
        (config.gestures && !allocate(s_gestures, kGestureRingSize))) {
        Serial.println("[TOUCH] ERROR: no memory for the touch queues");
        return false;
    }

    bool answered = false;
    if (i2c_mutex_lock(100)) {
        if (config.begin_wire) Wire.begin(kPinSda, kPinScl);
        Wire.setClock(config.i2c_clock_hz);  // after Wire.begin(), or it is overwritten
        if (config.wire_timeout_ms) Wire.setTimeOut(config.wire_timeout_ms);
        Wire.beginTransmission(tp::kCst92xxAddress);
        answered = Wire.endTransmission() == 0;
        i2c_mutex_unlock();
    }
    Serial.printf("[TOUCH] CST92xx %s\n", answered ? "found" : "NOT answering");

#if HANDE_TOUCH_LVGL
    if (config.lvgl_indev) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        if (config.display) lv_indev_set_display(s_indev, config.display);
        lv_indev_set_read_cb(s_indev, indev_read_cb);
    }
#else
    if (config.lvgl_indev) Serial.println("[TOUCH] ERROR: built with HANDE_TOUCH_LVGL=0 but lvgl_indev is on");
#endif

    if (config.sampler_period_ms > 0) {
        if (xTaskCreatePinnedToCore(sampler_task, "touch_sampler", config.sampler_stack, nullptr,
                                    config.sampler_priority, &s_task, config.sampler_core) != pdPASS) {
            Serial.println("[TOUCH] ERROR: sampler task could not start");
            s_task = nullptr;
            return false;
        }
        if (config.sampler_wake_on_int) {
            attachInterrupt(digitalPinToInterrupt(kPinInt), touch_isr, FALLING);
            s_int_attached = true;
        }
    } else if (config.may_skip_idle_read) {
        // Read in LVGL's callback with idle reads skipped while INT is high: latch every INT pulse (no task to wake).
        attachInterrupt(digitalPinToInterrupt(kPinInt), touch_isr, FALLING);
        s_int_attached = true;
    }
    return answered;
}

lv_indev_t* hande_touch_indev() { return s_indev; }

bool hande_touch_is_pressed() {
    portENTER_CRITICAL(&s_mux);
    const bool pressed = s_current.pressed;
    portEXIT_CRITICAL(&s_mux);
    return pressed;
}

HandeTouchSample hande_touch_current() {
    portENTER_CRITICAL(&s_mux);
    const HandeTouchSample s = s_current;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

bool hande_touch_pop_sample(HandeTouchSample* out) {
    portENTER_CRITICAL(&s_mux);
    const bool popped = s_queue.pop(out);
    portEXIT_CRITICAL(&s_mux);
    return popped;
}

bool hande_touch_pop_gesture(HandeTouchGesture* out) {
    portENTER_CRITICAL(&s_mux);
    const bool popped = s_gestures.pop(out);
    portEXIT_CRITICAL(&s_mux);
    return popped;
}

void hande_touch_flush() {
    portENTER_CRITICAL(&s_mux);
    s_queue.clear();
    s_lvgl_ring.clear();  // samples LVGL has not taken yet would otherwise make gestures after the flush
    s_gestures.clear();
    portEXIT_CRITICAL(&s_mux);
    s_cancel_requested = true;  // a press in progress gives no gesture either (the next press_edge starts clean)
}

void hande_touch_cancel_gesture() { s_cancel_requested = true; }

bool hande_touch_wait_ready(uint32_t timeout_ms) {
    const uint32_t start = millis();
    do {
        if (i2c_mutex_lock(20)) {
            Wire.beginTransmission(tp::kCst92xxAddress);
            const bool answered = Wire.endTransmission() == 0;
            i2c_mutex_unlock();
            if (answered && (s_cfg.sampler_period_ms == 0 || s_task)) return true;
        }
        delay(10);
    } while (millis() - start < timeout_ms);
    return false;
}

uint32_t hande_touch_last_activity_ms() {
    portENTER_CRITICAL(&s_mux);
    const uint32_t ms = s_last_activity_ms;
    portEXIT_CRITICAL(&s_mux);
    return ms;
}

HandeTouchStats hande_touch_stats() {
    portENTER_CRITICAL(&s_mux);
    const HandeTouchStats stats = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return stats;
}

uint32_t hande_touch_sampler_stack_headroom_bytes() {
    return s_task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task)) * sizeof(StackType_t) : 0;
}
