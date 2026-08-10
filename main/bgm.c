#include "bgm.h"

#include <math.h>
#include <string.h>

/* =====================================================================
 * 网页版 BGM 规格（@32kHz 采样率的样本换算）：
 *   BPM 128 → S16 = 60/128/4 * 32000 = 3750 样本；S8 = 7500；循环 = 64*3750 = 240000
 *   每个 16 分步触发一次 trigger_step，四小节 C-G-Am-F 无限循环。
 *   所有包络为指数斜坡（每样本 amp *= k，k 在触发时用 powf 一次算好）。
 * ===================================================================== */

#define BGM_SR           32000u
#define BGM_S16          3750u
#define BGM_S8           7500u
#define BGM_LOOP_STEPS   64u
#define BGM_POOL         16
#define BGM_LUT_SIZE     4096u
#define BGM_STAB_OSC     8

#define TWO_PI_F         6.283185307179586f

typedef enum {
    BGM_V_KICK,
    BGM_V_SNARE_NOISE,
    BGM_V_SNARE_TONE,
    BGM_V_HAT,
    BGM_V_CRASH,
    BGM_V_STAB,
    BGM_V_BASS,
} bgm_voice_type_t;

typedef struct {
    bgm_voice_type_t type;
    bool             active;
    uint32_t         age;  /* 已渲染样本数 */
    uint32_t         dur;  /* 总寿命 */

    /* 振荡器 */
    float   phase, phase_inc;
    float   phases[BGM_STAB_OSC], incs[BGM_STAB_OSC]; /* 仅 stab 用：4 音 × 2 detune 层 */
    uint8_t n_osc;

    /* 包络（指数斜坡） */
    float   amp;
    float   attack_k, decay_k;
    uint32_t attack_n;

    /* 频率扫频（kick: 160→45Hz） */
    float   freq, freq_k;
    uint32_t freq_ramp_n;

    /* 一阶滤波器 */
    float   lp_a, lp_y;   /* 低通 */
    float   hp_a, hp_y, hp_x; /* 高通（snare 噪声层用 hp→lp 级联近似带通） */

    uint32_t rng;         /* 本声部独立白噪声 PRNG（xorshift32） */
    float   extra;        /* 备用：stab 低通系数扫频因子 */
} bgm_voice_t;

typedef struct {
    float bass;
    float notes[4];
} bgm_chord_t;

static const bgm_chord_t k_chords[4] = {
    {65.41f, {261.63f, 329.63f, 392.00f, 523.25f}}, /* C  */
    {49.00f, {196.00f, 246.94f, 293.66f, 392.00f}}, /* G  */
    {55.00f, {220.00f, 261.63f, 329.63f, 440.00f}}, /* Am */
    {43.65f, {174.61f, 220.00f, 261.63f, 349.23f}}, /* F  */
};
static const float k_hat_vel[4] = {0.34f, 0.16f, 0.42f, 0.16f};

static float s_sine[BGM_LUT_SIZE];
static bgm_voice_t s_voices[BGM_POOL];
static bool s_running;
static uint32_t s_step;      /* 0..63 */
static uint32_t s_countdown; /* 距下一次步进触发还剩多少样本 */
static float s_gain = BGM_DEFAULT_GAIN;
static uint32_t s_noise_seed = 0x9E3779B9u;

/* ---------- 基础工具 ---------- */

static float lut_sine(float phase)
{
    const float x = phase * (float)BGM_LUT_SIZE;
    const uint32_t i = (uint32_t)x & (BGM_LUT_SIZE - 1);
    const float frac = x - (float)i;
    const uint32_t j = (i + 1) & (BGM_LUT_SIZE - 1);
    return s_sine[i] + (s_sine[j] - s_sine[i]) * frac;
}

static uint32_t next_noise(uint32_t *rng)
{
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x;
    return x;
}

static void seed_rng(bgm_voice_t *v)
{
    /* 避免 0 种子的简单 LCG 播种，使每个噪声声部相互独立 */
    s_noise_seed = s_noise_seed * 0x9E3779B9u + 1u;
    if (s_noise_seed == 0) s_noise_seed = 0x9E3779B9u;
    v->rng = s_noise_seed;
}

static bgm_voice_t *alloc_voice(void)
{
    bgm_voice_t *oldest = &s_voices[0];
    uint32_t max_age = s_voices[0].age;
    for (size_t i = 0; i < BGM_POOL; ++i) {
        if (!s_voices[i].active) {
            bgm_voice_t *v = &s_voices[i];
            memset(v, 0, sizeof(*v));
            return v;
        }
        if (s_voices[i].age > max_age) { /* 满池时 steal 年龄最大的槽（正常不会发生） */
            max_age = s_voices[i].age;
            oldest = &s_voices[i];
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

/* ---------- 乐器触发（@32kHz 样本数） ---------- */

static void start_kick(void)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_KICK;
    v->active = true;
    v->dur = 8320;            /* 0.26s */
    v->freq = 160.0f;
    v->phase_inc = 160.0f / (float)BGM_SR;
    v->freq_k = powf(45.0f / 160.0f, 1.0f / 3520.0f);   /* 160→45 指数扫频，0.11s */
    v->freq_ramp_n = 3520;
    v->amp = 0.95f;
    v->decay_k = powf(0.001f / 0.95f, 1.0f / 7680.0f);  /* 0.95→0.001，0.24s */
}

static void start_snare_noise(float vol)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_SNARE_NOISE;
    v->active = true;
    v->dur = 5760;            /* 0.18s */
    v->amp = vol;
    v->decay_k = powf(0.001f / vol, 1.0f / 5120.0f);     /* →0.001，0.16s */
    v->hp_a = expf(-TWO_PI_F * 1800.0f / (float)BGM_SR); /* 带通≈HP1800 + LP1800 级联 */
    v->lp_a = 1.0f - expf(-TWO_PI_F * 1800.0f / (float)BGM_SR);
    seed_rng(v);
}

static void start_snare_tone(float vol)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_SNARE_TONE;
    v->active = true;
    v->dur = 3200;            /* 0.1s */
    v->phase_inc = 240.0f / (float)BGM_SR;
    v->amp = vol * 0.5f;      /* 鼓腔音量 = 噪声层 × 0.5 */
    v->decay_k = powf(0.001f / (vol * 0.5f), 1.0f / 2880.0f); /* →0.001，0.09s */
}

static void start_hat(float vel, float decay_sec)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_HAT;
    v->active = true;
    const uint32_t decay_n = (uint32_t)(decay_sec * (float)BGM_SR + 0.5f); /* 0.04s→1280 / 0.12s→3840 */
    v->dur = decay_n + 640;   /* +0.02s */
    v->amp = vel;
    v->decay_k = powf(0.001f / vel, 1.0f / (float)decay_n);
    v->hp_a = expf(-TWO_PI_F * 7500.0f / (float)BGM_SR);
    seed_rng(v);
}

static void start_crash(void)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_CRASH;
    v->active = true;
    v->dur = 41600;           /* 1.3s */
    v->amp = 0.32f;
    v->decay_k = powf(0.001f / 0.32f, 1.0f / 38400.0f);  /* →0.001，1.2s */
    v->hp_a = expf(-TWO_PI_F * 5000.0f / (float)BGM_SR);
    seed_rng(v);
}

static void start_stab(const float notes[4])
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_STAB;
    v->active = true;
    v->dur = 9600;            /* 0.3s */
    v->n_osc = 0;
    for (size_t k = 0; k < 4; ++k) {
        static const float det[2] = {-6.0f, 5.0f}; /* cents，每音叠 ±6/+5 两层 */
        for (size_t d = 0; d < 2; ++d) {
            const float fr = notes[k] * powf(2.0f, det[d] / 1200.0f);
            v->phases[v->n_osc] = 0.0f;
            v->incs[v->n_osc] = fr / (float)BGM_SR;
            v->n_osc++;
        }
    }
    v->amp = 0.0001f;
    v->attack_n = 320;        /* 0.01s */
    v->attack_k = powf(0.14f / 0.0001f, 1.0f / 320.0f);
    v->decay_k = powf(0.001f / 0.14f, 1.0f / 8640.0f);  /* 从峰值起衰减 0.27s */
    /* 低通 2600→600 指数扫频：扫系数而非频率，避免热循环 expf */
    const float lp_a_init = 1.0f - expf(-TWO_PI_F * 2600.0f / (float)BGM_SR);
    const float lp_a_end  = 1.0f - expf(-TWO_PI_F * 600.0f / (float)BGM_SR);
    v->lp_a = lp_a_init;
    v->extra = powf(lp_a_end / lp_a_init, 1.0f / 8960.0f);
    v->freq_ramp_n = 8960;    /* 扫频时长 0.28s，之后保持 */
}

static void start_bass(float root, float vol)
{
    bgm_voice_t *v = alloc_voice();
    v->type = BGM_V_BASS;
    v->active = true;
    v->dur = BGM_S8;          /* 0.234s */
    v->phase_inc = (root * 2.0f) / (float)BGM_SR; /* 根音 ×2 方波 */
    v->amp = 0.0001f;
    v->attack_n = 320;        /* 0.01s */
    v->attack_k = powf(vol / 0.0001f, 1.0f / 320.0f);
    v->decay_k = powf(0.001f / vol, 1.0f / 6430.0f); /* S8*0.9 - 320 = 6430 */
    v->lp_a = 1.0f - expf(-TWO_PI_F * 300.0f / (float)BGM_SR);
}

/* ---------- 步进调度（复刻网页 scheduleStep） ---------- */

static void trigger_step(uint32_t s)
{
    const uint32_t bar = s >> 4;
    const uint32_t pos = s & 15u;
    const bgm_chord_t *ch = &k_chords[bar];

    if (bar == 0 && pos == 0) start_crash();
    if ((pos & 3u) == 0) start_kick();
    if (pos == 4 || pos == 12) {
        start_snare_noise(0.5f);
        start_snare_tone(0.5f);
    }
    if (bar == 3 && pos == 14) { /* 末尾加花 */
        start_snare_noise(0.3f);
        start_snare_tone(0.3f);
    }
    start_hat(k_hat_vel[pos & 3u], pos == 14 ? 0.12f : 0.04f);
    if ((pos & 3u) == 2) start_stab(ch->notes);
    if ((pos & 1u) == 0) start_bass(ch->bass, (pos & 3u) == 0 ? 0.4f : 0.26f);
}

/* ---------- 每样本声部渲染 ---------- */

static float render_voice(bgm_voice_t *v)
{
    if (v->age >= v->dur) {
        v->active = false;
        return 0.0f;
    }
    float out = 0.0f;
    switch (v->type) {
    case BGM_V_KICK: {
        if (v->age < v->freq_ramp_n) { /* 频率指数扫频期间逐样本更新 */
            v->freq *= v->freq_k;
            v->phase_inc = v->freq / (float)BGM_SR;
        }
        v->phase += v->phase_inc;
        if (v->phase >= 1.0f) v->phase -= 1.0f;
        const float s = lut_sine(v->phase);
        v->amp *= v->decay_k;
        out = s * v->amp;
        break;
    }
    case BGM_V_SNARE_NOISE: {
        const float n = (int32_t)next_noise(&v->rng) * (1.0f / 2147483648.0f);
        v->hp_y = v->hp_a * (v->hp_y + n - v->hp_x); /* HP 1800 */
        v->hp_x = n;
        v->lp_y += v->lp_a * (v->hp_y - v->lp_y);    /* LP 1800 */
        v->amp *= v->decay_k;
        out = v->lp_y * v->amp;
        break;
    }
    case BGM_V_SNARE_TONE: {
        v->phase += v->phase_inc;
        if (v->phase >= 1.0f) v->phase -= 1.0f;
        const float tri = 2.0f * fabsf(2.0f * v->phase - 1.0f) - 1.0f;
        v->amp *= v->decay_k;
        out = tri * v->amp;
        break;
    }
    case BGM_V_HAT:
    case BGM_V_CRASH: {
        const float n = (int32_t)next_noise(&v->rng) * (1.0f / 2147483648.0f);
        v->hp_y = v->hp_a * (v->hp_y + n - v->hp_x);
        v->hp_x = n;
        v->amp *= v->decay_k;
        out = v->hp_y * v->amp;
        break;
    }
    case BGM_V_STAB: {
        float x = 0.0f;
        for (uint8_t k = 0; k < v->n_osc; ++k) {
            v->phases[k] += v->incs[k];
            if (v->phases[k] >= 1.0f) v->phases[k] -= 1.0f;
            x += 1.0f - 2.0f * v->phases[k]; /* 锯齿波 */
        }
        if (v->age < v->freq_ramp_n) v->lp_a *= v->extra; /* 低通扫频 */
        v->lp_y += v->lp_a * (x - v->lp_y);
        if (v->age < v->attack_n) v->amp *= v->attack_k;
        else v->amp *= v->decay_k;
        out = v->lp_y * v->amp;
        break;
    }
    case BGM_V_BASS: {
        v->phase += v->phase_inc;
        if (v->phase >= 1.0f) v->phase -= 1.0f;
        const float sq = v->phase < 0.5f ? 1.0f : -1.0f;
        v->lp_y += v->lp_a * (sq - v->lp_y);
        if (v->age < v->attack_n) v->amp *= v->attack_k;
        else v->amp *= v->decay_k;
        out = v->lp_y * v->amp;
        break;
    }
    }
    v->age++;
    return out;
}

/* ---------- 对外接口 ---------- */

void bgm_init(void)
{
    for (size_t i = 0; i < BGM_LUT_SIZE; ++i) {
        s_sine[i] = sinf(TWO_PI_F * (float)i / (float)BGM_LUT_SIZE);
    }
}

void bgm_start(void)
{
    s_running = true;
    s_step = 0;
    s_countdown = 0; /* 首个渲染样本立即触发 step 0，不从 mid-step 切入 */
    for (size_t i = 0; i < BGM_POOL; ++i) s_voices[i].active = false;
}

int32_t bgm_render_sample(bool muted)
{
    if (!s_running) return 0;
    if (s_countdown == 0) {
        trigger_step(s_step);
        s_step = (s_step + 1) & (BGM_LOOP_STEPS - 1);
        s_countdown = BGM_S16;
    }
    s_countdown--;
    float acc = 0.0f;
    for (size_t i = 0; i < BGM_POOL; ++i) {
        if (s_voices[i].active) acc += render_voice(&s_voices[i]);
    }
    if (muted) return 0; /* muted 仍推进音序器，恢复时从原位置继续 */
    return (int32_t)(acc * s_gain * 32767.0f);
}

void bgm_set_gain(float gain)
{
    s_gain = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
}

float bgm_gain(void)
{
    return s_gain;
}
