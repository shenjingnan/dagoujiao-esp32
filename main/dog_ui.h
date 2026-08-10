#pragma once

#include <stdint.h>

void dog_ui_init(void);
void dog_ui_schedule_bark(int64_t when_us);
/* 由 touch_async 独立触摸 task 调用：按下即触发狗叫（绕过 LVGL 渲染，音频响应快） */
void dog_ui_handle_touch(int32_t x, int32_t y);
