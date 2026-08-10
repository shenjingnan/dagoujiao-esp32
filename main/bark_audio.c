#include "bark_audio.h"

#include <limits.h>
#include <math.h>

#include "bark_samples.h"
#include "bgm.h"
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
#define MAX_VOICES 10
#define STEP_Q16_1 (1U << 16)

static const char *TAG = "bark_audio";

typedef struct {
    bark_syllable_t syllable;
    uint8_t tier;
} bark_event_t;

typedef struct {
    const bark_sample_t *sample;
    uint32_t pos_q16;
    uint32_t step_q16;   /* 变速播放步长（16.16 定点，rate=1.0 时为 STEP_Q16_1） */
    bool active;
} bark_voice_t;

static QueueHandle_t s_event_queue;
static esp_codec_dev_handle_t s_speaker;
static bark_voice_t s_voices[MAX_VOICES];
static uint8_t s_volume = 80;
static uint8_t s_bgm_percent = 100;
static bool s_muted;
static volatile bool s_bgm_armed;  /* 跨核：enqueue 写，audio_task 读清除 */
static bool s_bgm_started;         /* 首次点按惰性启动判据 */

/* 网页 main.js 实测原声音高（BARK_SOURCE_MIDI）与 4 档目标音高（BARK_TARGET_MIDI），
 * 下标顺序与 bark_syllable_t 一致；变速率 = 2^((target - source) / 12)。 */
static const float S_SOURCE_MIDI[3] = { 71.1950846771f, 65.5950930881f, 71.1226079346f };
static const float S_TARGET_MIDI[3][BARK_PITCH_TIERS] = {
    { 79, 76, 72, 69 },  /* da  */
    { 72, 69, 67, 64 },  /* gou */
    { 79, 76, 72, 69 },  /* jiao */
};
static uint32_t s_step_q16[3][BARK_PITCH_TIERS];  /* 仅 audio_task（Core 0）读写 */

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
        .step_q16 = s_step_q16[event->syllable][event->tier],
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
    voice->pos_q16 += voice->step_q16;
    return a + (((b - a) * (int32_t)fraction) >> 16);
}

static void drain_events(void)
{
    bark_event_t event;
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        start_voice(&event);
    }
    /* 首次点按后惰性启动 BGM；bgm_start() 只在 audio_task（Core 0）上下文执行，
     * 避免与渲染线程竞争音色池。首次点按的狗叫与 BGM step 0 落在同一批 drain 里。 */
    if (s_bgm_armed) {
        s_bgm_armed = false;
        bgm_start();
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
            int32_t bark = 0;
            for (size_t voice = 0; voice < MAX_VOICES; ++voice) bark += render_voice(&s_voices[voice]);
            bark = (bark * s_volume) / 100;                    /* 狗叫跟随 VOL */
            int32_t bgm = bgm_render_sample(s_muted);          /* BGM 内部已乘固定增益，不乘 s_volume */
            int32_t mixed = bark + bgm;
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
    /* 音量独立：codec 输出固定满量程，VOL 只做数字域狗叫增益；BGM 由固定增益控制 */
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_speaker, 100));
    bgm_init();
    /* 变速率表：rate = 2^((target - source) / 12)，转 16.16 定点 */
    for (int s = 0; s < 3; ++s) {
        for (int t = 0; t < BARK_PITCH_TIERS; ++t) {
            s_step_q16[s][t] = (uint32_t)(powf(2.0f, (S_TARGET_MIDI[s][t] - S_SOURCE_MIDI[s]) / 12.0f) * STEP_Q16_1);
        }
    }
    /* 优先级 9 高于 LVGL task(6)：实时音频必须按时补充 I2S 缓冲，
     * 不能被 FX 全屏动画渲染抢占导致 underrun 卡顿。 */
    xTaskCreatePinnedToCore(audio_task, "bark_audio", 4096, NULL, 9, NULL, 0);
    ESP_LOGI(TAG, "audio engine ready at %d Hz", SAMPLE_RATE);
}

int64_t bark_audio_enqueue(bark_syllable_t syllable, uint8_t tier)
{
    const int64_t now = esp_timer_get_time();
    /* 首次点按：登记惰性启动 BGM（真正的 bgm_start() 由 audio_task 消费标志后执行） */
    if (!s_bgm_started) {
        s_bgm_started = true;
        s_bgm_armed = true;
    }
    if (tier >= BARK_PITCH_TIERS) tier = BARK_PITCH_TIERS - 1;
    const bark_event_t event = {.syllable = syllable, .tier = tier};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) ESP_LOGW(TAG, "input queue full; dropping bark");
    return now;
}

void bark_audio_set_volume(uint8_t percent) { s_volume = percent > 100 ? 100 : percent; }
uint8_t bark_audio_get_volume(void) { return s_volume; }
void bark_audio_toggle_mute(void) { s_muted = !s_muted; }
bool bark_audio_is_muted(void) { return s_muted; }
void bark_audio_set_bgm_volume(uint8_t percent)
{
    s_bgm_percent = percent > 100 ? 100 : percent;
    bgm_set_gain(BGM_DEFAULT_GAIN * (float)s_bgm_percent / 100.0f);
}
uint8_t bark_audio_get_bgm_volume(void) { return s_bgm_percent; }
