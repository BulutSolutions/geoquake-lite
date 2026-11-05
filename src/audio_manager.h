#pragma once

#include <stdint.h>

enum SoundEffect {
    SOUND_EFFECT_BEEP = 0,
    SOUND_EFFECT_DOUBLE_BEEP = 1,
    SOUND_EFFECT_CHIME = 2,
    SOUND_EFFECT_ALERT = 3,
    SOUND_EFFECT_TRIPLE_BEEP = 4,
    SOUND_EFFECT_QUAD_PULSE = 5,
    SOUND_EFFECT_DESCENDING_CHIME = 6,
    SOUND_EFFECT_WARBLE = 7,
    SOUND_EFFECT_TWO_TONE = 8,
    SOUND_EFFECT_LONG_ALERT = 9,
    SOUND_EFFECT_SOFT_PING = 10,
    SOUND_EFFECT_DEEP_RUMBLE = 11,
    SOUND_EFFECT_SIREN = 12,
    SOUND_EFFECT_TRIPLET_CHIME = 13,
    SOUND_EFFECT_URGENT_BUZZ = 14,
    SOUND_EFFECT_FADE_SWEEP = 15,
    SOUND_EFFECT_BELL = 16,
    SOUND_EFFECT_TWO_STEP_RISE = 17,
    SOUND_EFFECT_WHISTLE = 18,
};

bool audio_init();
bool audio_is_ready();
bool audio_play_effect(SoundEffect effect, uint8_t volume_percent);
bool audio_play_notification();
bool audio_play_test_tone();
