#include "touch_async.h"

#include "bsp/esp-bsp.h"
#include "dog_ui.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define TAG "touch_async"

#define TOUCH_POLL_MS 10        /* 触摸读取 task 轮询周期 */
#define TOUCH_READ_PERIOD_MS 16 /* LVGL indev 读取共享缓存的周期 */
#define TOUCH_INT_GPIO BSP_LCD_TOUCH_INT /* CST816S INT 引脚，低电平=有触摸 */
#define TOUCH_TRIGGER_GAP_US 200000 /* 抬起丢失时的兜底触发间隔（正常靠按下边沿） */

/* 共享缓存：touch task（Core 0）写，LVGL read_cb（Core 1）读。
 * lv_coord_t / lv_indev_state_t 对齐读写，在 Xtensa 上是原子的。 */
static volatile lv_coord_t s_touch_x;
static volatile lv_coord_t s_touch_y;
static volatile lv_indev_state_t s_touch_state = LV_INDEV_STATE_RELEASED;

static esp_lcd_touch_handle_t s_tp;
static bool s_touch_down; /* 正在触摸：保持 PRESSED 直到确认松开 */
static bool s_prev_pressed; /* 上一轮是否按下（按下边沿判定） */
static int64_t s_last_trigger_us; /* 上次触发狗叫的时刻（抬起丢失兜底） */

/* 独立触摸读取：轮询读 CST816S（I2C，~0.5ms），写共享缓存。
 * 与 LVGL 渲染解耦，触摸状态不依赖 LVGL 的 33ms 调度。
 * 注意：CST816S 无触摸时进入低功耗睡眠，I2C 读会失败（NACK），
 * 因此只在"INT 拉低(有触摸) 或 正在触摸中"时读 I2C；
 * "读失败/读到无点"视为触摸释放，"读到点"视为按下（保持到确认松开）。 */
static void touch_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        if (s_tp == NULL) continue;

        const bool int_low = (gpio_get_level(TOUCH_INT_GPIO) == 0);
        if (!int_low && !s_touch_down) {
            s_touch_state = LV_INDEV_STATE_RELEASED; /* 无触摸且非触摸中 */
            continue;
        }

        esp_err_t err = esp_lcd_touch_read_data(s_tp);
        if (err != ESP_OK) {
            s_touch_down = false; /* 芯片睡眠 = 无触摸 */
            s_touch_state = LV_INDEV_STATE_RELEASED;
            continue;
        }
        esp_lcd_touch_point_data_t pt[1];
        uint8_t n = 0;
        if (esp_lcd_touch_get_data(s_tp, pt, &n, 1) == ESP_OK && n > 0) {
            /* 按下边沿触发（上一轮非按下）；持续按住不重复；
             * 兜底：极快连点导致抬起被漏检时，间隔超阈值也触发。 */
            const int64_t now = esp_timer_get_time();
            if (!s_prev_pressed || (now - s_last_trigger_us >= TOUCH_TRIGGER_GAP_US)) {
                dog_ui_handle_touch((int32_t)pt[0].x, (int32_t)pt[0].y);
                s_last_trigger_us = now;
            }
            s_prev_pressed = true;
            s_touch_down = true;
            s_touch_x = (lv_coord_t)pt[0].x;
            s_touch_y = (lv_coord_t)pt[0].y;
            s_touch_state = LV_INDEV_STATE_PRESSED;
        } else {
            s_prev_pressed = false;
            s_touch_down = false; /* 读到无点 = 松开 */
            s_touch_state = LV_INDEV_STATE_RELEASED;
        }
    }
}

/* 轻量 read_cb：只读共享缓存，无 I2C。坐标已由 esp_lcd_touch 缩放到屏幕范围。 */
static void touch_async_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    data->state = s_touch_state;
}

void touch_async_init(void)
{
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev == NULL) {
        ESP_LOGE(TAG, "indev not ready, touch async disabled");
        return;
    }

    /* 复用 esp_lv_adapter 已创建的 esp_lcd_touch handle。
     * 从 indev driver data（esp_lv_adapter_touch_ctx_t）取首字段 handle；
     * 依赖该组件结构布局（组件版本已锁定于 managed_components，升级时需核对）。 */
    typedef struct {
        esp_lcd_touch_handle_t handle;
    } touch_ctx_probe_t;
    s_tp = ((touch_ctx_probe_t *)lv_indev_get_driver_data(indev))->handle;
    if (s_tp == NULL) {
        ESP_LOGE(TAG, "touch handle not found, touch async disabled");
        return;
    }

    /* 替换 read_cb：LVGL 不再直接读 I2C，改读共享缓存 */
    lv_indev_set_read_cb(indev, touch_async_read_cb);
    /* 缩短缓存读取周期，让事件处理更快 */
    lv_timer_t *rt = lv_indev_get_read_timer(indev);
    if (rt != NULL) lv_timer_set_period(rt, TOUCH_READ_PERIOD_MS);

    /* 独立触摸读取 task：固定 Core 0、优先级 8（低于音频 9，不打断实时音频） */
    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 8, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "touch task create failed, keep original read path");
        return;
    }
    ESP_LOGI(TAG, "touch async: I2C read moved to dedicated task, LVGL reads cache");
}
