#include "dog_ui.h"

#include <stdbool.h>

#include "bark_audio.h"
#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "lvgl.h"

#define DISPLAY_SIZE 360
#define SETTINGS_SIZE 52

extern const uint8_t dagou_close_mouth_png_start[] asm("_binary_dagou_close_mouth_png_start");
extern const uint8_t dagou_close_mouth_png_end[] asm("_binary_dagou_close_mouth_png_end");
extern const uint8_t dagou_open_mouth_png_start[] asm("_binary_dagou_open_mouth_png_start");
extern const uint8_t dagou_open_mouth_png_end[] asm("_binary_dagou_open_mouth_png_end");

static lv_obj_t *s_dog;
static lv_obj_t *s_status;
static lv_obj_t *s_settings_panel;
static int64_t s_bark_at_us;
static int64_t s_close_at_us;
static bool s_open;

static lv_image_dsc_t s_dog_closed;
static lv_image_dsc_t s_dog_open;

static void set_open(bool open)
{
    if (s_open == open) return;
    s_open = open;
    lv_image_set_src(s_dog, open ? &s_dog_open : &s_dog_closed);
    lv_obj_set_y(s_dog, open ? -4 : 0);
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
}

void dog_ui_schedule_bark(int64_t when_us) { s_bark_at_us = when_us; }

void dog_ui_init(void)
{
    s_dog_closed.header = (lv_image_header_t){
        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .w = 0, .h = 0, .stride = 0,
    };
    s_dog_closed.data_size = (uint32_t)(dagou_close_mouth_png_end - dagou_close_mouth_png_start);
    s_dog_closed.data = dagou_close_mouth_png_start;
    s_dog_open.header = (lv_image_header_t){
        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .w = 0, .h = 0, .stride = 0,
    };
    s_dog_open.data_size = (uint32_t)(dagou_open_mouth_png_end - dagou_open_mouth_png_start);
    s_dog_open.data = dagou_open_mouth_png_start;

    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_lock(-1);
    lv_obj_t *stage = lv_screen_active();
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xf7f1df), 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(stage, stage_event, LV_EVENT_CLICKED, NULL);

    s_dog = lv_image_create(stage);
    lv_image_set_src(s_dog, &s_dog_closed);
    lv_image_set_scale(s_dog, 90); // source art is 1024 px; 90/256 fits 360 px
    lv_image_set_pivot(s_dog, 512, 512);
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
