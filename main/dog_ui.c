#include "dog_ui.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "bark_audio.h"
#include "bsp/esp-bsp.h"
#include "dog_images.h"
#include "fx_effects.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

#define DISPLAY_SIZE 360
#define SETTINGS_SIZE 52
#define DOG_PIXELS (360 * 360 * 4) /* ARGB8888 360x360 */

/* 大狗节拍律动（复刻网页版 main.js：BPM128 拍头跳起 + 压扁拉伸 + 两拍一周期旋转晃动） */
#define DOG_BASE_SCALE 205      /* 256=100%，缩到 80% 让四周透出动效 */
#define DOG_SPB_S      0.46875f /* 秒/拍 = 60/128（BPM128） */
#define DOG_JUMP_PX    9.0f     /* 拍头向上跳 px */
#define DOG_SWAY_PX    5.0f     /* 左右晃 px */
#define DOG_SWAY_DEG   2.4f     /* 旋转角 ° */
#define DOG_SQUASH_X   0.06f    /* 拍头 x 放大比例（压扁拉伸） */
#define DOG_SQUASH_Y   0.05f    /* 拍头 y 压缩比例 */

static lv_obj_t *s_dog;
static lv_obj_t *s_status;
static lv_obj_t *s_settings_panel;
static int64_t s_bark_at_us;
static int64_t s_close_at_us;
static bool s_open;

static lv_image_dsc_t s_dog_closed;
static lv_image_dsc_t s_dog_open;
static void *s_dog_closed_data;
static void *s_dog_open_data;

static void load_dog_art(void)
{
    /* 优先把图片像素拷贝到 PSRAM，渲染时避免从 flash 反复读取；
       分配失败则回退到 flash 中的原始数据（仍可显示）。 */
    s_dog_closed = dog_close;
    s_dog_open = dog_open;

    s_dog_closed_data = heap_caps_malloc(DOG_PIXELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_dog_closed_data != NULL) {
        memcpy(s_dog_closed_data, dog_close.data, DOG_PIXELS);
        s_dog_closed.data = s_dog_closed_data;
    }

    s_dog_open_data = heap_caps_malloc(DOG_PIXELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_dog_open_data != NULL) {
        memcpy(s_dog_open_data, dog_open.data, DOG_PIXELS);
        s_dog_open.data = s_dog_open_data;
    }
}

static void set_open(bool open)
{
    if (s_open == open) return;
    s_open = open;
    lv_image_set_src(s_dog, open ? &s_dog_open : &s_dog_closed);
    /* y 位移统一由 animation_timer 的节拍律动计算（张嘴时额外跳起 -4px） */
}

static void update_status(void)
{
    char status[32];
    if (bark_audio_is_muted()) lv_snprintf(status, sizeof(status), "MUTED");
    else lv_snprintf(status, sizeof(status), "VOL %d", bark_audio_get_volume());
    lv_label_set_text(s_status, status);
}

static void close_settings(lv_event_t *event) { (void)event; lv_obj_add_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN); }
static void volume_down(lv_event_t *event) { (void)event; uint8_t volume = bark_audio_get_volume(); bark_audio_set_volume(volume < 10 ? 0 : volume - 10); update_status(); }
static void volume_up(lv_event_t *event) { (void)event; bark_audio_set_volume(bark_audio_get_volume() + 10); update_status(); }
static void show_settings(lv_event_t *event) { (void)event; update_status(); lv_obj_remove_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN); }

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int x, int y, int width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 42);
    lv_obj_set_pos(button, x, y);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void play_cell_at(lv_point_t point)
{
    if (point.x >= DISPLAY_SIZE - SETTINGS_SIZE && point.y < SETTINGS_SIZE) return;

    const uint8_t column = point.x >= 240 ? 2 : (point.x >= 120 ? 1 : 0);
    dog_ui_schedule_bark(bark_audio_enqueue((bark_syllable_t)column));

    /* 每次有效点击随机播放一种全屏动效；设置面板打开时不触发 */
    if (lv_obj_has_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN)) {
        fx_effects_spawn(point.x, point.y);
    }
}

static void stage_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev != NULL) lv_indev_get_point(indev, &point);
    play_cell_at(point);
}

static void animation_timer(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now = esp_timer_get_time();
    if (s_bark_at_us != 0 && now >= s_bark_at_us) {
        set_open(true);
        s_close_at_us = now + 280000;
        s_bark_at_us = 0;
    }
    if (s_close_at_us != 0 && now >= s_close_at_us) {
        set_open(false);
        s_close_at_us = 0;
    }

    /* 大狗节拍律动（复刻网页版 main.js）：拍头向上跳 + 压扁拉伸 + 两拍一周期旋转晃动 */
    const float t = (float)(now % 2000000) * 1e-6f; /* 限制在 2s 内保证 float 精度 */
    const float phase = fmodf(t, DOG_SPB_S) / DOG_SPB_S;
    const float beat_p = powf(1.0f - phase, 2.4f);
    const float sway = sinf(t * (float)M_PI / DOG_SPB_S);
    lv_obj_set_x(s_dog, (lv_coord_t)(sway * DOG_SWAY_PX));
    lv_obj_set_y(s_dog, (lv_coord_t)(-DOG_JUMP_PX * beat_p) + (s_open ? -4 : 0));
    lv_image_set_rotation(s_dog, (int32_t)(sway * DOG_SWAY_DEG * 10.0f));
    lv_image_set_scale_x(s_dog, (uint32_t)(DOG_BASE_SCALE * (1.0f + DOG_SQUASH_X * beat_p)));
    lv_image_set_scale_y(s_dog, (uint32_t)(DOG_BASE_SCALE * (1.0f - DOG_SQUASH_Y * beat_p)));
}

void dog_ui_schedule_bark(int64_t when_us) { s_bark_at_us = when_us; }

void dog_ui_init(void)
{
    load_dog_art();

    bsp_display_cfg_t dcfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {0},
    };
    /* LVGL 渲染固定 Core 1；音频任务（bark_audio）固定 Core 0，
     * 分核运行避免 FX 全屏动画渲染抢占实时音频导致卡顿。 */
    dcfg.lv_adapter_cfg.task_core_id = 1;
    bsp_display_start_with_config(&dcfg);
    bsp_display_backlight_on();
    bsp_display_lock(-1);
    lv_obj_t *stage = lv_screen_active();
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xf7f1df), 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(stage, stage_event, LV_EVENT_CLICKED, NULL);

    /* 全屏动效层：叠在背景之上、大狗之下（大狗在前不被遮挡），不可点击 */
    fx_effects_init();

    s_dog = lv_image_create(stage);
    lv_image_set_src(s_dog, &s_dog_closed);
    lv_image_set_scale(s_dog, DOG_BASE_SCALE);
    lv_image_set_pivot(s_dog, 180, 180);
    lv_obj_center(s_dog);
    lv_obj_remove_flag(s_dog, LV_OBJ_FLAG_CLICKABLE);

    s_status = lv_label_create(stage);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x6b6459), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -10);
    update_status();

    lv_obj_t *settings = add_button(stage, "SET", show_settings, DISPLAY_SIZE - SETTINGS_SIZE, 0, SETTINGS_SIZE);
    lv_obj_set_style_bg_opa(settings, LV_OPA_50, 0);

    s_settings_panel = lv_obj_create(stage);
    lv_obj_set_size(s_settings_panel, 252, 160);
    lv_obj_center(s_settings_panel);
    lv_obj_set_style_bg_color(s_settings_panel, lv_color_hex(0xfffbf1), 0);
    lv_obj_set_style_border_width(s_settings_panel, 2, 0);
    lv_obj_set_style_border_color(s_settings_panel, lv_color_hex(0x6b6459), 0);
    lv_obj_t *title = lv_label_create(s_settings_panel);
    lv_label_set_text(title, "DOG SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    add_button(s_settings_panel, "VOL -", volume_down, 12, 52, 105);
    add_button(s_settings_panel, "VOL +", volume_up, 135, 52, 105);
    add_button(s_settings_panel, "CLOSE", close_settings, 12, 102, 228);
    lv_obj_add_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(animation_timer, 16, NULL);
    bsp_display_unlock();
}
