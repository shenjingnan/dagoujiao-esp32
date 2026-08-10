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
#include "touch_async.h"

#define DISPLAY_SIZE 360
#define DOG_PIXELS (360 * 360 * 4) /* ARGB8888 360x360 */

/* 大狗节拍律动（复刻网页版 main.js：BPM128 拍头跳起 + 两拍一周期左右晃动，无旋转/压扁拉伸） */
#define DOG_BASE_SCALE 180      /* 256=100%，缩到 70% */
#define DOG_SPB_S      0.46875f /* 秒/拍 = 60/128（BPM128） */
#define DOG_JUMP_PX    9.0f     /* 拍头向上跳 px */
#define DOG_SWAY_PX    5.0f     /* 左右晃 px */

static lv_obj_t *s_dog;
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
}

/* 由 touch_async 独立触摸 task 调用（Core 0）：按下即触发狗叫，
 * 不经过 LVGL 渲染任务，音频响应快、连点更跟手。
 * 全屏九宫格触发：按 X 分 3 列音节、按 Y 分 3 档音高。 */
void dog_ui_handle_touch(int32_t x, int32_t y)
{
    const uint8_t column = x >= 240 ? 2 : (x >= 120 ? 1 : 0);
    /* 同一音节按 Y 分 3 档音高：上高下低（九宫格，每格 120px） */
    const uint8_t tier = y >= 240 ? 2 : (y >= 120 ? 1 : 0);
    dog_ui_schedule_bark(bark_audio_enqueue((bark_syllable_t)column, tier));
}

/* LVGL 点击事件回调（Core 1 渲染上下文）：只负责全屏 FX 动效，
 * 发声由 touch_async 独立 task 按下即触发，两者互不依赖。 */
static void stage_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev != NULL) lv_indev_get_point(indev, &point);
    fx_effects_spawn(point.x, point.y);
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

    /* 大狗节拍律动（复刻网页版 main.js）：拍头向上跳 + 两拍一周期左右晃动 */
    const float t = (float)(now % 2000000) * 1e-6f; /* 限制在 2s 内保证 float 精度 */
    const float phase = fmodf(t, DOG_SPB_S) / DOG_SPB_S;
    const float beat_p = powf(1.0f - phase, 2.4f);
    const float sway = sinf(t * (float)M_PI / DOG_SPB_S);
    lv_obj_set_x(s_dog, (lv_coord_t)(sway * DOG_SWAY_PX));
    lv_obj_set_y(s_dog, (lv_coord_t)(-DOG_JUMP_PX * beat_p) + (s_open ? -4 : 0));
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
    /* 触摸读取解耦：移出 LVGL 渲染任务，提升点击灵敏度（须在显示锁内） */
    touch_async_init();
    lv_obj_t *stage = lv_screen_active();
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xf7f1df), 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    /* 固定屏幕：禁用滚动，避免大狗节拍压扁/晃动导致内容越界后手指能拖动画面 */
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(stage, LV_DIR_NONE);
    /* LVGL 点击事件：仅触发全屏 FX 特效（发声由 touch_async 独立驱动，不依赖此） */
    lv_obj_add_event_cb(stage, stage_event, LV_EVENT_CLICKED, NULL);

    /* 全屏动效层：叠在背景之上、大狗之下（大狗在前不被遮挡），不可点击 */
    fx_effects_init();

    /* 大狗即整个内容区域：满屏铺满、不可点击（触摸由 touch_async 驱动） */
    s_dog = lv_image_create(stage);
    lv_image_set_src(s_dog, &s_dog_closed);
    lv_image_set_scale(s_dog, DOG_BASE_SCALE);
    lv_image_set_pivot(s_dog, 180, 180);
    lv_obj_center(s_dog);
    lv_obj_remove_flag(s_dog, LV_OBJ_FLAG_CLICKABLE);

    lv_timer_create(animation_timer, 16, NULL);
    bsp_display_unlock();
}
