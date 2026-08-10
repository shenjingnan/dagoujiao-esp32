#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BARK_SYLLABLE_DA = 0,
    BARK_SYLLABLE_GOU,
    BARK_SYLLABLE_JIAO,
} bark_syllable_t;

void bark_audio_init(void);
int64_t bark_audio_enqueue(bark_syllable_t syllable);
void bark_audio_set_volume(uint8_t percent);
uint8_t bark_audio_get_volume(void);
void bark_audio_toggle_mute(void);
bool bark_audio_is_muted(void);
