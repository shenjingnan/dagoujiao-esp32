#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 全屏动效背景层（仿网页版「大狗大狗叫叫叫」的 Mikutap 风格特效）
 *
 * 实现方式：在 LVGL 上用一个全屏 lv_canvas(ARGB8888, PSRAM) 叠在大狗图之上。
 * 每次触摸触发一个随机特效，直接往 canvas 裸 buffer 写像素，
 * LVGL 软件渲染器负责把带 alpha 的特效合成到大狗之上。
 * 单层显示：新特效触发时，旧特效在 0.4s 内淡出+缩到 78%。
 */

/* 初始化：创建全屏特效画布与 33ms 更新定时器。
 * 必须在 bsp_display_start() 之后、bsp_display_lock() 临界区内调用。 */
void fx_effects_init(void);

/* 触发生效：随机挑选一种全屏特效；已有特效进入退场。
 * x/y 为触摸坐标（当前固定以屏幕中心为原点，参数预留）。 */
void fx_effects_spawn(int32_t x, int32_t y);

/* 每帧推进：由内部 33ms 定时器调用；对外保留便于外部周期驱动。 */
void fx_effects_update(int64_t now_us);

/* 是否有活跃特效（空闲时外部可据此跳过无效重绘）。 */
bool fx_effects_is_active(void);
