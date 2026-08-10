#include "bark_audio.h"

#include <limits.h>

#include "bark_samples.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SAMPLE_RATE 32000
#define MIX_FRAMES 128
#define MAX_VOICES 6

static const char *TAG = "bark_audio";

typedef struct {
    bark_syllable_t syllable;
} bark_event_t;

typedef struct {
    const bark_sample_t *sample;
    uint32_t pos_q16;
    bool active;
} bark_voice_t;

static QueueHandle_t s_event_queue;
static esp_codec_dev_handle_t s_speaker;
static bark_voice_t s_voices[MAX_VOICES];
static uint8_t s_volume = 80;
static bool s_muted;

static const bark_sample_t *sample_for(bark_syllable_t syllable)
{
    switch (syllable) {
    case BARK_SYLLABLE_DA: return &k_bark_da;
    case BARK_SYLLABLE_GOU: return &k_bark_gou;
    case BARK_SYLLABLE_JIAO: return &k_bark_jiao;
    default: return &k_bark_da;
    }
}

static void start_voice(const bark_event_t *event)
{
    bark_voice_t *voice = &s_voices[0];
    for (size_t i = 0; i < MAX_VOICES; ++i) {
        if (!s_voices[i].active) {
            voice = &s_voices[i];
            break;
        }
    }
    *voice = (bark_voice_t) {
        .sample = sample_for(event->syllable),
        .active = true,
    };
}

static int32_t render_voice(bark_voice_t *voice)
{
    if (!voice->active || s_muted) return 0;
    const uint32_t index = voice->pos_q16 >> 16;
    if (index + 1 >= voice->sample->frames) {
        voice->active = false;
        return 0;
    }
    const uint32_t fraction = voice->pos_q16 & 0xffffU;
    const int32_t a = voice->sample->pcm[index];
    const int32_t b = voice->sample->pcm[index + 1];
    voice->pos_q16 += 1U << 16;
    return a + (((b - a) * (int32_t)fraction) >> 16);
}

static void drain_events(void)
{
    bark_event_t event;
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        start_voice(&event);
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t *buffer = heap_caps_malloc(MIX_FRAMES * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(buffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    while (true) {
        drain_events();
        for (size_t frame = 0; frame < MIX_FRAMES; ++frame) {
            int32_t mixed = 0;
            for (size_t voice = 0; voice < MAX_VOICES; ++voice) mixed += render_voice(&s_voices[voice]);
            mixed = (mixed * s_volume) / 100;
            buffer[frame] = mixed > INT16_MAX ? INT16_MAX : (mixed < INT16_MIN ? INT16_MIN : (int16_t)mixed);
        }
        ESP_ERROR_CHECK(esp_codec_dev_write(s_speaker, buffer, MIX_FRAMES * sizeof(int16_t)));
        // The codec write is buffered and returns immediately. Pace synthesis to
        // one block so the audio task does not starve the idle task.
        vTaskDelay(pdMS_TO_TICKS((MIX_FRAMES * 1000) / SAMPLE_RATE));
    }
}

void bark_audio_init(void)
{
    s_event_queue = xQueueCreate(20, sizeof(bark_event_t));
    ESP_ERROR_CHECK(s_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK, .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN},
    };
    // Supplying a custom I2S configuration bypasses the BSP's lazy I2C setup.
    // The ES8311 control interface still needs that bus before it is constructed.
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_audio_init(&i2s_cfg));
    s_speaker = bsp_audio_codec_speaker_init();
    ESP_ERROR_CHECK(s_speaker == NULL ? ESP_FAIL : ESP_OK);
    esp_codec_dev_sample_info_t format = {.bits_per_sample = 16, .channel = 1, .sample_rate = SAMPLE_RATE};
    ESP_ERROR_CHECK(esp_codec_dev_open(s_speaker, &format));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_speaker, s_volume));
    xTaskCreatePinnedToCore(audio_task, "bark_audio", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "audio engine ready at %d Hz", SAMPLE_RATE);
}

int64_t bark_audio_enqueue(bark_syllable_t syllable)
{
    const int64_t now = esp_timer_get_time();
    const bark_event_t event = {.syllable = syllable};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) ESP_LOGW(TAG, "input queue full; dropping bark");
    return now;
}

void bark_audio_set_volume(uint8_t percent) { s_volume = percent > 100 ? 100 : percent; esp_codec_dev_set_out_vol(s_speaker, s_volume); }
uint8_t bark_audio_get_volume(void) { return s_volume; }
void bark_audio_toggle_mute(void) { s_muted = !s_muted; }
bool bark_audio_is_muted(void) { return s_muted; }
