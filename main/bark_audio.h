#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BARK_SYLLABLE_DA = 0,
    BARK_SYLLABLE_GOU,
    BARK_SYLLABLE_JIAO,
} bark_syllable_t;

/* 每音节可选的音高档数（0 最高音，匹配网页 BARK_TARGET_MIDI 下标顺序） */
#define BARK_PITCH_TIERS 4

void bark_audio_init(void);
int64_t bark_audio_enqueue(bark_syllable_t syllable, uint8_t tier);
void bark_audio_set_volume(uint8_t percent);
uint8_t bark_audio_get_volume(void);
void bark_audio_toggle_mute(void);
bool bark_audio_is_muted(void);
/* BGM 音量（本次 UI 未接线，仅留接口）：percent 0-100，作用于 BGM_DEFAULT_GAIN 之上 */
void bark_audio_set_bgm_volume(uint8_t percent);
uint8_t bark_audio_get_bgm_volume(void);
