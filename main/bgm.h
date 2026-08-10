#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 背景音乐（BGM）软合成引擎：样本驱动的 64 步音序器，复刻网页版「大狗大狗叫叫叫」
 * 的 Web Audio BGM（BPM 128，C-G-Am-F 循环）。所有状态为模块内部；
 * 音序器与音色池只允许在 audio_task（Core 0）上下文触碰。
 *
 * 音量模型：BGM 使用独立的固定增益（BGM_DEFAULT_GAIN），不跟随狗叫的 VOL 调节；
 * 全局静音由调用方通过 bgm_render_sample(muted) 传入，静音时仍推进音序器。 */

#define BGM_DEFAULT_GAIN 0.30f /* P5 调音常量：BGM 峰值约占满量程一半以下，狗叫能穿透 */

void    bgm_init(void);                /* 生成 sine LUT；在 bark_audio_init() 中调用一次 */
void    bgm_start(void);               /* 重置音序器到 step 0、清空 voice 池；仅 audio_task 上下文调用 */
int32_t bgm_render_sample(bool muted); /* 每样本推进 1 次，返回 int16 量级 BGM 混合值（已乘 BGM_DEFAULT_GAIN） */
void    bgm_set_gain(float gain);      /* 设置绝对增益 0..1，作用于合成输出；默认 BGM_DEFAULT_GAIN */
float   bgm_gain(void);
