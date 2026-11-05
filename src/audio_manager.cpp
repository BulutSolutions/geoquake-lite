#include "audio_manager.h"

#include "config.h"
#include "es8311/es8311.h"
#include "i2c_mutex.h"
#include "settings_manager.h"
#include "ui_power.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <math.h>

#if __has_include(<ESP_I2S.h>)
#include <ESP_I2S.h>
#define GEOQUAKE_USE_ESP_I2S_CLASS 1
#else
#include <driver/i2s.h>
#define GEOQUAKE_USE_ESP_I2S_CLASS 0
#endif

namespace {

constexpr uint8_t ES8311_I2C_PORT = 0;
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t AUDIO_CHANNELS = 2;
constexpr uint8_t AUDIO_BITS_PER_SAMPLE = 16;
constexpr uint16_t BEEP_TONE_HZ = 880;
constexpr uint16_t BEEP_TONE_MS = 450;
constexpr uint16_t DOUBLE_BEEP_TONE_HZ = 880;
constexpr uint16_t DOUBLE_BEEP_TONE_MS = 120;
constexpr uint16_t DOUBLE_BEEP_GAP_MS = 80;
constexpr uint16_t CHIME_START_HZ = 660;
constexpr uint16_t CHIME_END_HZ = 1320;
constexpr uint16_t CHIME_TONE_MS = 350;
constexpr uint16_t ALERT_TONE_HZ = 440;
constexpr uint16_t ALERT_TONE_MS = 500;
constexpr uint16_t LONG_ALERT_TONE_MS = 1000;
constexpr float EFFECT_TONE_GAIN = 0.28f;
constexpr int CODEC_OUTPUT_VOLUME = 70;
constexpr es8311_mic_gain_t CODEC_MIC_GAIN = ES8311_MIC_GAIN_12DB;
constexpr uint32_t CODEC_I2C_TIMEOUT_MS = 250;
constexpr uint32_t PA_SETTLE_DELAY_MS = 75;
constexpr uint32_t ES8311_MCLK_MULTIPLE = 256;
constexpr uint32_t AUDIO_MCLK_HZ = AUDIO_SAMPLE_RATE * ES8311_MCLK_MULTIPLE;

// PA_CTRL polarity is confirmed by Waveshare's ESP32-S3-Touch-AMOLED-2.16
// Arduino ES8311 example: digitalWrite(PA, HIGH) enables the amplifier.
#ifndef AUDIO_PA_CTRL_ACTIVE_HIGH
#define AUDIO_PA_CTRL_ACTIVE_HIGH 1
#endif

#if GEOQUAKE_USE_ESP_I2S_CLASS
I2SClass i2s;
#else
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
#endif

es8311_handle_t es8311 = nullptr;
bool audio_ready = false;
bool i2s_active = false;
bool tone_playing = false;
TaskHandle_t tone_task_handle = nullptr;
SoundEffect pending_effect = SOUND_EFFECT_BEEP;
uint8_t pending_volume = CODEC_OUTPUT_VOLUME;
static portMUX_TYPE audio_state_mux = portMUX_INITIALIZER_UNLOCKED;

bool lock_codec_i2c(const char* operation) {
    if (i2c_mutex_lock(CODEC_I2C_TIMEOUT_MS)) {
        return true;
    }

    Serial.printf("[AUDIO] ERROR: I2C mutex timeout during %s\n", operation);
    return false;
}

void set_pa_enabled(bool enabled) {
    const uint8_t active_level = AUDIO_PA_CTRL_ACTIVE_HIGH ? HIGH : LOW;
    const uint8_t inactive_level = AUDIO_PA_CTRL_ACTIVE_HIGH ? LOW : HIGH;
    digitalWrite(PA_CTRL, enabled ? active_level : inactive_level);
}

bool check_esp(const char* operation, esp_err_t err) {
    if (err == ESP_OK) {
        return true;
    }

    Serial.printf("[AUDIO] ERROR: %s failed: %s\n", operation, esp_err_to_name(err));
    return false;
}

void log_internal_heap(const char* label) {
    Serial.printf("[AUDIO] Heap %s: free=%u KB largest=%u KB min=%u KB\n",
                  label,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

#if GEOQUAKE_USE_ESP_I2S_CLASS
bool i2s_begin_native() {
    if (i2s_active) {
        return true;
    }

    i2s.setPins(I2S_SCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
    i2s_active = i2s.begin(I2S_MODE_STD,
                           AUDIO_SAMPLE_RATE,
                           I2S_DATA_BIT_WIDTH_16BIT,
                           I2S_SLOT_MODE_STEREO,
                           I2S_STD_SLOT_BOTH);
    return i2s_active;
}

void i2s_end_native() {
    if (!i2s_active) {
        return;
    }

    i2s.end();
    i2s_active = false;
}

size_t i2s_write_native(const uint8_t* data, size_t bytes) {
    return i2s.write(data, bytes);
}
#else
bool i2s_begin_native() {
    if (i2s_active) {
        return true;
    }

    const i2s_config_t i2s_config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = static_cast<int>(AUDIO_MCLK_HZ),
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr);
    if (err == ESP_ERR_INVALID_STATE) {
        i2s_driver_uninstall(I2S_PORT);
        err = i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr);
    }
    if (!check_esp("I2S driver install", err)) {
        return false;
    }

    const i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK,
        .bck_io_num = I2S_SCLK,
        .ws_io_num = I2S_LRCK,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN,
    };

    if (!check_esp("I2S pin config", i2s_set_pin(I2S_PORT, &pin_config))) {
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    if (!check_esp("I2S zero DMA", i2s_zero_dma_buffer(I2S_PORT))) {
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    i2s_active = true;
    return true;
}

void i2s_end_native() {
    if (!i2s_active) {
        return;
    }

    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[AUDIO] WARN: I2S driver uninstall failed: %s\n", esp_err_to_name(err));
    }
    i2s_active = false;
}

size_t i2s_write_native(const uint8_t* data, size_t bytes) {
    size_t bytes_written = 0;
    esp_err_t err = i2s_write(I2S_PORT, data, bytes, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: I2S write failed: %s\n", esp_err_to_name(err));
        return bytes_written;
    }
    return bytes_written;
}
#endif

bool codec_set_mute(bool muted) {
    if (!es8311 || !lock_codec_i2c("codec mute")) {
        return false;
    }

    esp_err_t err = es8311_voice_mute(es8311, muted);
    i2c_mutex_unlock();

    return check_esp("Codec mute", err);
}

bool codec_set_volume(int volume) {
    if (!es8311 || !lock_codec_i2c("codec volume")) {
        return false;
    }

    int volume_set = 0;
    esp_err_t err = es8311_voice_volume_set(es8311, volume, &volume_set);
    i2c_mutex_unlock();

    if (!check_esp("Codec volume", err)) {
        return false;
    }

    Serial.printf("[AUDIO] Codec volume set to %d\n", volume_set);
    return true;
}

bool codec_init() {
    es8311 = es8311_create(ES8311_I2C_PORT, ES8311_ADDRRES_0);
    if (!es8311) {
        Serial.println("[AUDIO] ERROR: es8311_create failed");
        return false;
    }

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = static_cast<int>(AUDIO_MCLK_HZ),
        .sample_frequency = static_cast<int>(AUDIO_SAMPLE_RATE),
    };

    if (!lock_codec_i2c("codec init")) {
        es8311_delete(es8311);
        es8311 = nullptr;
        return false;
    }

    esp_err_t err = es8311_init(es8311, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (err == ESP_OK) {
        err = es8311_sample_frequency_config(es8311, es_clk.mclk_frequency, es_clk.sample_frequency);
    }
    if (err == ESP_OK) {
        err = es8311_microphone_config(es8311, false);
    }
    if (err == ESP_OK) {
        err = es8311_voice_volume_set(es8311, CODEC_OUTPUT_VOLUME, nullptr);
    }
    if (err == ESP_OK) {
        err = es8311_microphone_gain_set(es8311, CODEC_MIC_GAIN);
    }
    if (err == ESP_OK) {
        err = es8311_voice_mute(es8311, true);
    }
    i2c_mutex_unlock();

    if (!check_esp("ES8311 init sequence", err)) {
        es8311_delete(es8311);
        es8311 = nullptr;
        return false;
    }

    Serial.printf("[AUDIO] ES8311 clock: MCLK=%lu Hz, sample_rate=%lu Hz\n",
                  AUDIO_MCLK_HZ,
                  AUDIO_SAMPLE_RATE);
    return true;
}

void write_silence(uint16_t frames) {
    int16_t silence[128 * AUDIO_CHANNELS] = {0};
    while (frames > 0) {
        uint16_t chunk_frames = frames < 128 ? frames : 128;
        i2s_write_native(reinterpret_cast<const uint8_t*>(silence),
                         chunk_frames * AUDIO_CHANNELS * sizeof(int16_t));
        frames -= chunk_frames;
    }
}

void write_silence_ms(uint16_t duration_ms) {
    write_silence((AUDIO_SAMPLE_RATE * duration_ms) / 1000);
}

void write_tone_segment(uint16_t start_hz, uint16_t end_hz, uint16_t duration_ms, float& phase) {
    const uint32_t total_frames = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const int16_t amplitude = static_cast<int16_t>(32767.0f * EFFECT_TONE_GAIN);
    uint32_t frames_written = 0;

    int16_t samples[128 * AUDIO_CHANNELS];
    while (frames_written < total_frames) {
        const uint32_t remaining_frames = total_frames - frames_written;
        const uint32_t frames_this_chunk = remaining_frames < 128 ? remaining_frames : 128;
        for (uint32_t frame = 0; frame < frames_this_chunk; ++frame) {
            const uint32_t absolute_frame = frames_written + frame;
            const float progress = total_frames > 1
                ? static_cast<float>(absolute_frame) / static_cast<float>(total_frames - 1)
                : 0.0f;
            const float frequency = static_cast<float>(start_hz) +
                ((static_cast<float>(end_hz) - static_cast<float>(start_hz)) * progress);
            const float phase_step = 2.0f * PI * frequency / AUDIO_SAMPLE_RATE;
            int16_t sample = static_cast<int16_t>(sinf(phase) * amplitude);
            samples[(frame * AUDIO_CHANNELS) + 0] = sample;
            samples[(frame * AUDIO_CHANNELS) + 1] = sample;

            phase += phase_step;
            if (phase >= 2.0f * PI) {
                phase = fmodf(phase, 2.0f * PI);
            }
        }

        const size_t bytes_requested = frames_this_chunk * AUDIO_CHANNELS * sizeof(int16_t);
        const size_t bytes_written = i2s_write_native(reinterpret_cast<const uint8_t*>(samples),
                                                      bytes_requested);
        if (bytes_written != bytes_requested) {
            Serial.printf("[AUDIO] WARN: I2S write short: requested %u bytes, wrote %u bytes\n",
                          static_cast<unsigned>(bytes_requested),
                          static_cast<unsigned>(bytes_written));
        }
        frames_written += frames_this_chunk;
    }
}

const char* effect_name(SoundEffect effect) {
    switch (effect) {
        case SOUND_EFFECT_BEEP: return "Beep";
        case SOUND_EFFECT_DOUBLE_BEEP: return "Double Beep";
        case SOUND_EFFECT_CHIME: return "Chime";
        case SOUND_EFFECT_ALERT: return "Alert";
        case SOUND_EFFECT_TRIPLE_BEEP: return "Triple Beep";
        case SOUND_EFFECT_QUAD_PULSE: return "Quad Pulse";
        case SOUND_EFFECT_DESCENDING_CHIME: return "Descending Chime";
        case SOUND_EFFECT_WARBLE: return "Warble";
        case SOUND_EFFECT_TWO_TONE: return "Two-Tone";
        case SOUND_EFFECT_LONG_ALERT: return "Long Alert";
        case SOUND_EFFECT_SOFT_PING: return "Soft Ping";
        case SOUND_EFFECT_DEEP_RUMBLE: return "Deep Rumble";
        case SOUND_EFFECT_SIREN: return "Siren";
        case SOUND_EFFECT_TRIPLET_CHIME: return "Triplet Chime";
        case SOUND_EFFECT_URGENT_BUZZ: return "Urgent Buzz";
        case SOUND_EFFECT_FADE_SWEEP: return "Fade Sweep";
        case SOUND_EFFECT_BELL: return "Bell";
        case SOUND_EFFECT_TWO_STEP_RISE: return "Two-Step Rise";
        case SOUND_EFFECT_WHISTLE: return "Whistle";
        default: return "Beep";
    }
}

int half_volume_at_least_one(uint8_t volume) {
    const int half_volume = volume / 2;
    return half_volume < 1 ? 1 : half_volume;
}

void write_effect_waveform(SoundEffect effect, uint8_t volume) {
    float phase = 0.0f;
    switch (effect) {
        case SOUND_EFFECT_DOUBLE_BEEP:
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_MS, phase);
            write_silence_ms(DOUBLE_BEEP_GAP_MS);
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_MS, phase);
            break;
        case SOUND_EFFECT_CHIME:
            write_tone_segment(CHIME_START_HZ, CHIME_END_HZ, CHIME_TONE_MS, phase);
            break;
        case SOUND_EFFECT_ALERT:
            write_tone_segment(ALERT_TONE_HZ, ALERT_TONE_HZ, ALERT_TONE_MS, phase);
            break;
        case SOUND_EFFECT_TRIPLE_BEEP:
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_MS, phase);
            write_silence_ms(DOUBLE_BEEP_GAP_MS);
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_MS, phase);
            write_silence_ms(DOUBLE_BEEP_GAP_MS);
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_MS, phase);
            break;
        case SOUND_EFFECT_QUAD_PULSE:
            codec_set_volume(volume);
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 90, phase);
            write_silence_ms(60);
            codec_set_volume(static_cast<int>(volume * 0.8f));
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 90, phase);
            write_silence_ms(60);
            codec_set_volume(static_cast<int>(volume * 0.6f));
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 90, phase);
            write_silence_ms(60);
            codec_set_volume(static_cast<int>(volume * 0.4f));
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 90, phase);
            break;
        case SOUND_EFFECT_DESCENDING_CHIME:
            write_tone_segment(CHIME_END_HZ, CHIME_START_HZ, CHIME_TONE_MS, phase);
            break;
        case SOUND_EFFECT_WARBLE:
            write_tone_segment(600, 900, 100, phase);
            write_tone_segment(900, 600, 100, phase);
            write_tone_segment(600, 900, 100, phase);
            write_tone_segment(900, 600, 100, phase);
            break;
        case SOUND_EFFECT_TWO_TONE:
            write_tone_segment(784, 784, 200, phase);
            write_silence_ms(40);
            write_tone_segment(523, 523, 300, phase);
            break;
        case SOUND_EFFECT_LONG_ALERT:
            write_tone_segment(ALERT_TONE_HZ, ALERT_TONE_HZ, LONG_ALERT_TONE_MS, phase);
            break;
        case SOUND_EFFECT_SOFT_PING:
            write_tone_segment(1200, 1200, 150, phase);
            break;
        case SOUND_EFFECT_DEEP_RUMBLE:
            write_tone_segment(220, 220, 600, phase);
            break;
        case SOUND_EFFECT_SIREN:
            write_tone_segment(500, 1000, 200, phase);
            write_tone_segment(1000, 500, 200, phase);
            write_tone_segment(500, 1000, 200, phase);
            write_tone_segment(1000, 500, 200, phase);
            break;
        case SOUND_EFFECT_TRIPLET_CHIME:
            write_tone_segment(523, 523, 150, phase);
            write_silence_ms(30);
            write_tone_segment(659, 659, 150, phase);
            write_silence_ms(30);
            write_tone_segment(784, 784, 150, phase);
            break;
        case SOUND_EFFECT_URGENT_BUZZ:
            for (uint8_t pulse = 0; pulse < 6; ++pulse) {
                write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 40, phase);
                write_silence_ms(40);
            }
            break;
        case SOUND_EFFECT_FADE_SWEEP:
            write_tone_segment(1000, 750, 200, phase);
            codec_set_volume(half_volume_at_least_one(volume));
            write_tone_segment(750, 500, 200, phase);
            break;
        case SOUND_EFFECT_BELL:
            write_tone_segment(1000, 1000, 100, phase);
            codec_set_volume(half_volume_at_least_one(volume));
            write_tone_segment(1000, 1000, 300, phase);
            break;
        case SOUND_EFFECT_TWO_STEP_RISE:
            write_tone_segment(ALERT_TONE_HZ, ALERT_TONE_HZ, 200, phase);
            write_silence_ms(20);
            write_tone_segment(DOUBLE_BEEP_TONE_HZ, DOUBLE_BEEP_TONE_HZ, 200, phase);
            break;
        case SOUND_EFFECT_WHISTLE:
            write_tone_segment(1800, 1800, 300, phase);
            break;
        case SOUND_EFFECT_BEEP:
        default:
            write_tone_segment(BEEP_TONE_HZ, BEEP_TONE_HZ, BEEP_TONE_MS, phase);
            break;
    }
}

void tone_task(void*) {
    const SoundEffect effect = pending_effect;
    const uint8_t volume = pending_volume;
    Serial.printf("[AUDIO] Playing sound effect: %s (%d), volume=%u\n",
                  effect_name(effect),
                  static_cast<int>(effect),
                  volume);

    log_internal_heap("before playback I2S start");
    if (!i2s_begin_native()) {
        Serial.println("[AUDIO] ERROR: Cannot play effect; I2S init failed");
        portENTER_CRITICAL(&audio_state_mux);
        tone_playing = false;
        tone_task_handle = nullptr;
        portEXIT_CRITICAL(&audio_state_mux);
        vTaskDelete(nullptr);
        return;
    }
    log_internal_heap("after playback I2S start");

    codec_set_mute(false);
    codec_set_volume(volume);

    write_effect_waveform(effect, volume);
    write_silence(AUDIO_SAMPLE_RATE / 20);
    codec_set_mute(true);
    i2s_end_native();
    log_internal_heap("after playback I2S stop");

    Serial.println("[AUDIO] Sound effect complete");
    portENTER_CRITICAL(&audio_state_mux);
    tone_playing = false;
    tone_task_handle = nullptr;
    portEXIT_CRITICAL(&audio_state_mux);
    vTaskDelete(nullptr);
}

}  // namespace

bool audio_init() {
    if (audio_ready) {
        return true;
    }

    Serial.println("[AUDIO] Initializing ES8311 codec with native I2S");
    log_internal_heap("before init");

    pinMode(PA_CTRL, OUTPUT);
    set_pa_enabled(false);
    Serial.printf("[AUDIO] PA disabled for codec setup (PA_CTRL %s=enable)\n",
                  AUDIO_PA_CTRL_ACTIVE_HIGH ? "HIGH" : "LOW");

    if (!i2s_begin_native()) {
        Serial.println("[AUDIO] ERROR: I2S init failed");
        set_pa_enabled(false);
        return false;
    }
    log_internal_heap("after I2S init");

    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    if (!codec_init()) {
        i2s_end_native();
        set_pa_enabled(false);
        return false;
    }

    delay(PA_SETTLE_DELAY_MS);
    set_pa_enabled(true);
    Serial.printf("[AUDIO] PA enabled after %lu ms settle delay\n", PA_SETTLE_DELAY_MS);

    audio_ready = true;
    i2s_end_native();
    log_internal_heap("after idle I2S release");
    Serial.println("[AUDIO] ES8311 audio pipeline ready");
    return true;
}

bool audio_is_ready() {
    return audio_ready;
}

bool audio_play_effect(SoundEffect effect, uint8_t volume_percent) {
    if (!audio_ready && !audio_init()) {
        Serial.println("[AUDIO] ERROR: Cannot play effect; audio init failed");
        return false;
    }

    portENTER_CRITICAL(&audio_state_mux);
    if (tone_playing) {
        portEXIT_CRITICAL(&audio_state_mux);
        Serial.println("[AUDIO] Sound effect already playing");
        return false;
    }

    if (effect < SOUND_EFFECT_BEEP || effect > SOUND_EFFECT_WHISTLE) {
        effect = SOUND_EFFECT_BEEP;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }

    pending_effect = effect;
    pending_volume = volume_percent;
    tone_playing = true;
    portEXIT_CRITICAL(&audio_state_mux);
    BaseType_t result = xTaskCreatePinnedToCore(
        tone_task,
        "AudioTone",
        4096,
        nullptr,
        1,
        &tone_task_handle,
        0);

    if (result != pdPASS) {
        portENTER_CRITICAL(&audio_state_mux);
        tone_playing = false;
        tone_task_handle = nullptr;
        portEXIT_CRITICAL(&audio_state_mux);
        Serial.println("[AUDIO] ERROR: Failed to create tone task");
        return false;
    }

    return true;
}

bool audio_play_notification() {
    AppSettings* settings = settings_get();
    if (!settings) {
        Serial.println("[AUDIO] ERROR: Cannot play notification; settings unavailable");
        return false;
    }

    if (settings->sound_muted) {
        Serial.println("[AUDIO] Notification sound skipped; silent mode is enabled");
        return false;
    }

    if (ui_auto_dim_is_active()) {
        Serial.println("[AUDIO] Notification sound skipped; night mode (auto-dim) is active");
        return false;
    }

    int volume = settings->sound_volume;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    SoundEffect effect = static_cast<SoundEffect>(settings->sound_effect_id);
    if (effect < SOUND_EFFECT_BEEP || effect > SOUND_EFFECT_WHISTLE) {
        effect = SOUND_EFFECT_BEEP;
    }

    return audio_play_effect(effect, static_cast<uint8_t>(volume));
}

bool audio_play_test_tone() {
    return audio_play_notification();
}
