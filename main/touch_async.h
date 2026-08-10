#pragma once

/* 触摸读取解耦：把 CST816S 的 I2C 读取从 LVGL worker task 移到独立 task，
 * LVGL 的 indev read_cb 只读共享缓存（O(1)，无 I2C）。
 * 这样触摸读取不再被 LVGL 渲染（大狗节律 + FX 全屏动效）阻塞，点击响应更灵敏。
 *
 * 必须在 bsp_display_start() 之后、bsp_display_lock() 临界区内调用。 */
void touch_async_init(void);
