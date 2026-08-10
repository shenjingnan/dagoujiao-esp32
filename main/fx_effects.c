#include "fx_effects.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#define TAG "fx"

/* ---------------- 画布 / 时序常量 ---------------- */

#define FX_CANVAS_W 240
#define FX_CANVAS_H 240
#define FX_CX       120.0f /* 特效原点（画布中心，居中显示） */
#define FX_CY       120.0f
#define FX_MIN_D    240.0f /* 屏幕短边 */
#define FX_MAX_D    339.4113f /* hypot(240,240)，confetti 最大散布参考 */
#define FX_IN_S     0.55f  /* 入场 0.55s */
#define FX_OUT_S    0.40f  /* 退场 0.4s */
#define FX_AUTO_LIFE_S 1.6f /* 不点击时自动消失：入场+短暂常驻后转退场渐隐 */
#define FX_POOL     2      /* 当前特效 + 正在淡出的旧特效 */
#define FX_MAX_SHAPES 40
#define FX_UPDATE_MS 16    /* ~60fps，与 LVGL 刷新周期对齐 */
#define FX_PI       3.14159265358979323846f

/* ---------------- 调色板（与网页版 1:1） ---------------- */
/* 0x00RRGGBB；写像素时拼 (A<<24)|RGB */
#define FX_COL_AMBER 0xFFB400u /* 主色黄（62%） */
#define FX_COL_GRAY  0x87837Eu /* 次要灰（28%） */
#define FX_COL_CORAL 0xFF5A5Fu /* 点缀 */
#define FX_COL_TEAL  0x16C2A3u /* 点缀 */
#define FX_COL_BLUE  0x3E7BFAu /* 点缀 */

/* ---------------- 类型 / 状态 ---------------- */

typedef enum {
    FX_TYPE_RINGS = 0,   /* 同心环爆发 */
    FX_TYPE_CONFETTI,    /* 几何纸屑 */
    FX_TYPE_STARS,       /* 星星弹跳 */
    FX_TYPE_WAVE,        /* 波浪丝带 */
    FX_TYPE_RAYS,        /* 放射光芒 */
    FX_TYPE_SPIRAL,      /* 螺旋弹珠 */
    FX_TYPE_COUNT,
} fx_type_t;

typedef enum {
    FX_ST_IN = 0,   /* 播放中（直到被新特效顶替） */
    FX_ST_OUT,      /* 退场中（淡出+缩小） */
    FX_ST_DEAD,     /* 空闲槽位 */
} fx_state_t;

typedef enum {
    FX_PRIM_CIRCLE = 0,
    FX_PRIM_RING,
    FX_PRIM_SQUARE,
    FX_PRIM_TRIANGLE,
    FX_PRIM_DIAMOND,
    FX_PRIM_HEXAGON,
    FX_PRIM_STAR,
    FX_PRIM_CROSS,
} fx_prim_t;

/* 通用粒子参数：不同效果按需使用（字段较多，静态池开销可忽略） */
typedef struct {
    float ang, dist;      /* 极坐标放置（confetti/spiral/rays） */
    float x, y;           /* 绝对位置（stars） */
    float y0;             /* wave 基准线 y */
    float size, r;        /* 尺寸 / 半径 */
    float rot;            /* 基础旋转 */
    float delay;          /* 入场延迟 s */
    float spin, speed;    /* 自旋 / 运动速度 */
    float rEnd, w, len;   /* rings rEnd/w；rays w=半角/len */
    float amp, wl, th, rad; /* wave amp/wl/th；spiral rad */
    int8_t side;          /* wave 滑入方向 ±1 */
    uint8_t prim;         /* 原语类型 */
    uint32_t rgb;         /* 0x00RRGGBB */
} fx_shape_t;

typedef struct {
    uint8_t type, state, n;
    int8_t dir;
    float t0_s;           /* 出生时刻（秒） */
    float out_t0_s;       /* 退场起点（秒） */
    float rot0;
    float dot_r;          /* rings 中心点半径 / rays 起点半径 */
    fx_shape_t sh[FX_MAX_SHAPES];
} fx_effect_t;

/* ---------------- 静态状态 ---------------- */

static lv_obj_t *s_canvas;
static uint32_t *s_buf;               /* PSRAM 里的 ARGB8888 裸 buffer */
static lv_timer_t *s_timer;
static fx_effect_t s_fx[FX_POOL];
static uint8_t s_head;                /* 最新特效所在槽位 */
static bool s_active;
static uint32_t s_rng_state;          /* mulberry32 状态（与网页算法一致） */
static float s_beat_p;                /* 节拍脉冲 0..1（BGM128 固定基准） */
static uint32_t s_inv[256];           /* source-over 除法倒数表 */

/* ---------------- rng / easing ---------------- */

static float rng_next(void)
{
    s_rng_state = s_rng_state + 0x6D2B79F5u;
    uint32_t t = s_rng_state;
    t = (uint32_t)((t ^ (t >> 15)) * (1u | t));
    t = (t + (uint32_t)((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14)) & 0xFFFFFFFFu) / 4294967296.0f;
}

static inline float fx_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline float fx_prog(float t, float delay, float dur) { return fx_clamp01((t - delay) / dur); }
static inline float fx_smooth(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float fx_ease_out_cubic(float t) { return 1.0f - powf(1.0f - t, 3.0f); }
static inline float fx_ease_out_back(float t)
{
    const float c = 1.70158f, u = t - 1.0f;
    return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
}
static inline float fx_ease_out_elastic(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * (2.0f * FX_PI / 3.0f)) + 1.0f;
}

/* 节拍脉冲：BPM128 一拍 468.75ms，按 16 分音符线性衰减。
 * 预留：之后可换成 bgm_beat_pulse() 读真实 BGM 拍点。 */
static float fx_beat_pulse(int64_t now_us)
{
    const float beat = 60.0f / 128.0f;
    const float sub = beat / 4.0f;
    float ph = fmodf((float)(now_us % 1000000) / 1e6f, sub);
    return 1.0f - ph / sub;
}

/* ---------------- 像素光栅化 ---------------- */

static inline void fx_blend_px(uint32_t *p, uint32_t rgb, uint8_t a)
{
    uint32_t d = *p;
    uint8_t da = (uint8_t)(d >> 24);
    if (a >= 255) { *p = 0xFF000000u | rgb; return; }
    if (da == 0) { *p = ((uint32_t)a << 24) | rgb; return; }
    /* 标准 source-over（非预乘）：
       outA = a + da*(255-a)/255
       outC = (srcC*a*255 + dstC*da*(255-a)) / (outA*255)   */
    uint32_t wa = 255 - a;
    uint32_t daTerm = da * wa;
    uint32_t oa = a + daTerm / 255;
    uint32_t inv = s_inv[oa]; /* ≈ 2^32/(oa*255) */
    uint32_t srcR = (rgb >> 16) & 0xFF, srcG = (rgb >> 8) & 0xFF, srcB = rgb & 0xFF;
    uint32_t dstR = (d >> 16) & 0xFF, dstG = (d >> 8) & 0xFF, dstB = d & 0xFF;
    uint8_t oR = (uint8_t)(((uint64_t)(srcR * a * 255 + dstR * daTerm) * inv) >> 32);
    uint8_t oG = (uint8_t)(((uint64_t)(srcG * a * 255 + dstG * daTerm) * inv) >> 32);
    uint8_t oB = (uint8_t)(((uint64_t)(srcB * a * 255 + dstB * daTerm) * inv) >> 32);
    *p = ((uint32_t)oa << 24) | ((uint32_t)oR << 16) | ((uint32_t)oG << 8) | oB;
}

/* 水平 span 填充 */
static void fx_fill_span(uint32_t *buf, int x0, int x1, int y, uint32_t rgb, uint8_t a)
{
    if (a == 0) return;
    if (y < 0 || y >= FX_CANVAS_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= FX_CANVAS_W) x1 = FX_CANVAS_W - 1;
    if (x0 > x1) return;
    uint32_t *p = buf + (uint32_t)y * FX_CANVAS_W + (uint32_t)x0;
    for (int x = x0; x <= x1; x++, p++) fx_blend_px(p, rgb, a);
}

/* 垂直 span 填充（wave 每列用） */
static void fx_fill_vspan(uint32_t *buf, int x, int y0, int y1, uint32_t rgb, uint8_t a)
{
    if (a == 0) return;
    if (x < 0 || x >= FX_CANVAS_W) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= FX_CANVAS_H) y1 = FX_CANVAS_H - 1;
    if (y0 > y1) return;
    uint32_t *p = buf + (uint32_t)y0 * FX_CANVAS_W + (uint32_t)x;
    for (int y = y0; y <= y1; y++, p += FX_CANVAS_W) fx_blend_px(p, rgb, a);
}

static void fx_fill_circle(uint32_t *buf, float cx, float cy, float r, uint32_t rgb, uint8_t a)
{
    if (r <= 0.0f || a == 0) return;
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    if (y0 < 0) y0 = 0;
    if (y1 >= FX_CANVAS_H) y1 = FX_CANVAS_H - 1;
    float r2 = r * r;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float d2 = r2 - dy * dy;
        if (d2 < 0) continue;
        float half = sqrtf(d2);
        fx_fill_span(buf, (int)floorf(cx - half), (int)ceilf(cx + half), y, rgb, a);
    }
}

/* 圆环：圆心 rC、环宽 w（外圈半径 rC+w/2） */
static void fx_fill_ring(uint32_t *buf, float cx, float cy, float rC, float w, uint32_t rgb, uint8_t a)
{
    float rOut = rC + w * 0.5f;
    float rIn = rC - w * 0.5f;
    if (rIn < 0.0f) rIn = 0.0f;
    if (rOut <= 0.0f || a == 0) return;
    int y0 = (int)floorf(cy - rOut), y1 = (int)ceilf(cy + rOut);
    if (y0 < 0) y0 = 0;
    if (y1 >= FX_CANVAS_H) y1 = FX_CANVAS_H - 1;
    float ro2 = rOut * rOut, ri2 = rIn * rIn;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        float o2 = ro2 - dy2;
        if (o2 < 0) continue;
        float outHalf = sqrtf(o2);
        int xr0 = (int)floorf(cx - outHalf);
        int xr1 = (int)ceilf(cx + outHalf);
        if (rIn > 0.0f && dy2 < ri2) {
            float inHalf = sqrtf(ri2 - dy2);
            int xi0 = (int)ceilf(cx - inHalf);
            int xi1 = (int)floorf(cx + inHalf);
            fx_fill_span(buf, xr0, xi0 - 1, y, rgb, a);
            fx_fill_span(buf, xi1 + 1, xr1, y, rgb, a);
        } else {
            fx_fill_span(buf, xr0, xr1, y, rgb, a);
        }
    }
}

/* 通用多边形：扫描线 + 奇偶填充 */
static void fx_fill_poly(uint32_t *buf, const float v[][2], int n, uint32_t rgb, uint8_t a)
{
    if (n < 3 || a == 0) return;
    float y0f = v[0][1], y1f = v[0][1];
    for (int i = 1; i < n; i++) {
        if (v[i][1] < y0f) y0f = v[i][1];
        if (v[i][1] > y1f) y1f = v[i][1];
    }
    int iy0 = (int)floorf(y0f), iy1 = (int)ceilf(y1f);
    if (iy0 < 0) iy0 = 0;
    if (iy1 >= FX_CANVAS_H) iy1 = FX_CANVAS_H - 1;
    float xs[16];
    for (int y = iy0; y <= iy1; y++) {
        float ys = (float)y + 0.5f;
        int m = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float yA = v[i][1], yB = v[j][1];
            if ((yA <= ys && yB > ys) || (yB <= ys && yA > ys)) {
                float x = v[i][0] + (ys - yA) / (yB - yA) * (v[j][0] - v[i][0]);
                if (m < 16) xs[m++] = x;
            }
        }
        for (int i2 = 1; i2 < m; i2++) { /* 插入排序，m<=10 */
            float t = xs[i2];
            int j2 = i2 - 1;
            while (j2 >= 0 && xs[j2] > t) { xs[j2 + 1] = xs[j2]; j2--; }
            xs[j2 + 1] = t;
        }
        for (int j2 = 0; j2 + 1 < m; j2 += 2) {
            fx_fill_span(buf, (int)ceilf(xs[j2]), (int)floorf(xs[j2 + 1]), y, rgb, a);
        }
    }
}

/* 正多边形顶点（circumradius r，起始角 rot） */
static void fx_poly_verts(float v[][2], int n, float cx, float cy, float r, float rot)
{
    for (int i = 0; i < n; i++) {
        float a = rot + (float)i * 2.0f * FX_PI / (float)n;
        v[i][0] = cx + cosf(a) * r;
        v[i][1] = cy + sinf(a) * r;
    }
}

/* 五角星顶点（外 r、内 0.46r 交替） */
static void fx_star_verts(float v[][2], float cx, float cy, float r, float rot, int points)
{
    for (int i = 0; i < points * 2; i++) {
        float rr = (i % 2) ? r * 0.46f : r;
        float a = rot + (float)i * FX_PI / (float)points;
        v[i][0] = cx + cosf(a) * rr;
        v[i][1] = cy + sinf(a) * rr;
    }
}

/* 8 种原语统一入口（与网页 drawPiece 参数一致） */
static void fx_draw_piece(uint32_t *buf, uint8_t prim, float x, float y, float r, float rot,
                          uint32_t rgb, uint8_t a)
{
    if (r <= 0.0f || a == 0) return;
    switch (prim) {
    case FX_PRIM_CIRCLE:
        fx_fill_circle(buf, x, y, r, rgb, a);
        break;
    case FX_PRIM_RING: {
        float lw = fmaxf(2.0f, r * 0.3f);
        fx_fill_ring(buf, x, y, r, lw, rgb, a);
        break;
    }
    case FX_PRIM_SQUARE: {
        float c = cosf(rot), s = sinf(rot);
        float v[4][2];
        int idx = 0;
        for (int iy = -1; iy <= 1; iy += 2)
            for (int ix = -1; ix <= 1; ix += 2) {
                float lx = ix * r, ly = iy * r;
                v[idx][0] = x + lx * c - ly * s;
                v[idx][1] = y + lx * s + ly * c;
                idx++;
            }
        fx_fill_poly(buf, v, 4, rgb, a);
        break;
    }
    case FX_PRIM_TRIANGLE: {
        float v[3][2];
        fx_poly_verts(v, 3, x, y, r * 1.2f, rot - FX_PI * 0.5f);
        fx_fill_poly(buf, v, 3, rgb, a);
        break;
    }
    case FX_PRIM_DIAMOND: {
        float v[4][2];
        fx_poly_verts(v, 4, x, y, r * 1.15f, rot);
        fx_fill_poly(buf, v, 4, rgb, a);
        break;
    }
    case FX_PRIM_HEXAGON: {
        float v[6][2];
        fx_poly_verts(v, 6, x, y, r * 1.1f, rot);
        fx_fill_poly(buf, v, 6, rgb, a);
        break;
    }
    case FX_PRIM_STAR: {
        float v[10][2];
        fx_star_verts(v, x, y, r * 1.25f, rot - FX_PI * 0.5f, 5);
        fx_fill_poly(buf, v, 10, rgb, a);
        break;
    }
    case FX_PRIM_CROSS: {
        float c = cosf(rot), s = sinf(rot);
        float hw = r * 0.62f * 0.5f; /* 十字臂宽一半 */
        for (int arm = 0; arm < 2; arm++) {
            float ah = (arm == 0) ? r : hw;    /* 臂半长 */
            float av = (arm == 0) ? hw : r;    /* 臂半宽 */
            float v[4][2];
            int idx = 0;
            for (int iy = -1; iy <= 1; iy += 2)
                for (int ix = -1; ix <= 1; ix += 2) {
                    float lx = ix * ah, ly = iy * av;
                    v[idx][0] = x + lx * c - ly * s;
                    v[idx][1] = y + lx * s + ly * c;
                    idx++;
                }
            fx_fill_poly(buf, v, 4, rgb, a);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- 取色 ---------------- */

static uint32_t fx_pick_accent(void)
{
    const uint32_t accents[3] = { FX_COL_CORAL, FX_COL_TEAL, FX_COL_BLUE };
    return accents[(uint32_t)(rng_next() * 3.0f)];
}

static uint32_t fx_pick_color(void)
{
    float r = rng_next();
    if (r < 0.62f) return FX_COL_AMBER;
    if (r < 0.9f) return FX_COL_GRAY;
    return fx_pick_accent();
}

/* ---------------- BUILD：随机参数预生成（与网页 1:1） ---------------- */

static void fx_build(fx_effect_t *fx, uint8_t type)
{
    fx->type = type;
    fx->state = FX_ST_IN;
    fx->n = 0;
    fx->dot_r = 0.0f;
    fx->rot0 = rng_next() * FX_PI * 2.0f;
    fx->dir = rng_next() < 0.5f ? -1 : 1;

    switch (type) {
    case FX_TYPE_RINGS:
        for (int i = 0; i < 7; i++) {
            fx_shape_t *s = &fx->sh[fx->n++];
            s->delay = i * 0.05f;
            s->rEnd = FX_MIN_D * (0.13f + rng_next() * 0.29f);
            s->w = 5.0f + rng_next() * 9.0f;
            s->rgb = fx_pick_color();
        }
        fx->dot_r = FX_MIN_D * 0.07f;
        break;
    case FX_TYPE_CONFETTI:
        for (int i = 0; i < 30; i++) {
            const uint8_t kinds[4] = { FX_PRIM_SQUARE, FX_PRIM_CIRCLE, FX_PRIM_TRIANGLE, FX_PRIM_DIAMOND };
            fx_shape_t *s = &fx->sh[fx->n++];
            s->ang = rng_next() * FX_PI * 2.0f;
            s->dist = FX_MAX_D * (0.12f + rng_next() * 0.46f);
            s->size = FX_MIN_D * (0.026f + rng_next() * 0.05f);
            s->spin = fx->dir * (1.0f + rng_next() * 2.0f) * 2.2f;
            s->delay = rng_next() * 0.18f;
            s->prim = kinds[(uint32_t)(rng_next() * 4.0f)];
            s->rgb = fx_pick_color();
        }
        break;
    case FX_TYPE_STARS:
        for (int i = 0; i < 12; i++) {
            fx_shape_t *s = &fx->sh[fx->n++];
            s->x = FX_CANVAS_W * (0.07f + rng_next() * 0.86f);
            s->y = FX_CANVAS_H * (0.07f + rng_next() * 0.86f);
            s->r = FX_MIN_D * (0.034f + rng_next() * 0.055f);
            s->delay = rng_next() * 0.25f;
            s->rot = rng_next() * FX_PI;
            s->prim = FX_PRIM_STAR;
            s->rgb = fx_pick_color();
        }
        break;
    case FX_TYPE_WAVE:
        for (int i = 0; i < 4; i++) {
            fx_shape_t *s = &fx->sh[fx->n++];
            s->y0 = FX_CANVAS_H * (0.14f + i * 0.24f) + (rng_next() - 0.5f) * FX_CANVAS_H * 0.08f;
            s->amp = FX_MIN_D * (0.03f + rng_next() * 0.05f);
            s->wl = FX_CANVAS_W * (0.45f + rng_next() * 0.4f);
            s->speed = fx->dir * (1.0f + rng_next() * 1.2f);
            s->th = FX_MIN_D * (0.07f + rng_next() * 0.06f);
            s->side = (i % 2) ? 1 : -1;
            s->delay = i * 0.08f;
            s->rgb = (rng_next() < 0.12f) ? fx_pick_accent() : ((i % 2) ? FX_COL_GRAY : FX_COL_AMBER);
        }
        break;
    case FX_TYPE_RAYS: {
        int n = 13 + (int)(rng_next() * 4.0f);
        fx->dot_r = FX_MIN_D * 0.06f;
        for (int i = 0; i < n; i++) {
            fx_shape_t *s = &fx->sh[fx->n++];
            s->ang = ((float)i / (float)n) * FX_PI * 2.0f + rng_next() * 0.15f;
            s->w = 0.09f + rng_next() * 0.13f;
            s->len = FX_MIN_D * (0.36f + rng_next() * 0.1f);
            s->delay = rng_next() * 0.12f;
            s->rgb = (rng_next() < 0.12f) ? fx_pick_accent() : ((i % 2) ? FX_COL_GRAY : FX_COL_AMBER);
        }
        break;
    }
    case FX_TYPE_SPIRAL:
        for (int i = 0; i < 36; i++) {
            fx_shape_t *s = &fx->sh[fx->n++];
            s->ang = i * 0.55f;
            s->rad = 6.0f + i * FX_MIN_D * 0.0125f;
            s->size = FX_MIN_D * (0.009f + i * 0.0008f);
            s->delay = i * 0.018f;
            s->prim = (i % 6 == 5) ? FX_PRIM_SQUARE : FX_PRIM_CIRCLE;
            s->rgb = fx_pick_color();
        }
        break;
    default:
        break;
    }
}

/* ---------------- DRAW：每帧绘制（t = 出生至今秒数） ---------------- */

static void fx_draw_rings(fx_effect_t *fx, float t, float fade, float sc)
{
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_cubic(fx_prog(t, s->delay, FX_IN_S));
        if (k <= 0.0f) continue;
        float r = (k * s->rEnd * (1.0f + 0.04f * sinf(t * 1.4f + i)) + s_beat_p * FX_MIN_D * 0.012f) * sc;
        float w = s->w * (1.0f + s_beat_p * 0.5f);
        uint8_t a = (uint8_t)((1.0f - k * 0.5f) * fade * 255.0f);
        fx_fill_ring(s_buf, FX_CX, FX_CY, r, w, s->rgb, a);
    }
    float dk = fx_ease_out_back(fx_prog(t, 0.0f, FX_IN_S));
    if (dk > 0.0f) {
        float dr = fx->dot_r * dk * (1.0f + s_beat_p * 0.2f) * sc;
        uint8_t a = (uint8_t)(fade * 255.0f);
        fx_fill_circle(s_buf, FX_CX, FX_CY, dr, FX_COL_AMBER, a);
    }
}

static void fx_draw_confetti(fx_effect_t *fx, float t, float fade, float sc)
{
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_back(fx_prog(t, s->delay, FX_IN_S));
        if (k <= 0.0f) continue;
        float rr = s->dist * k * (1.0f + s_beat_p * 0.025f) * sc;
        float x = FX_CX + cosf(s->ang) * rr;
        float y = FX_CY + sinf(s->ang) * rr + sinf(t * 2.2f + i * 1.3f) * 6.0f;
        float sz = s->size * k * (1.0f + s_beat_p * 0.18f) * sc;
        float rot = s->spin * k + t * 0.6f * fx->dir;
        uint8_t a = (uint8_t)(fade * 255.0f);
        fx_draw_piece(s_buf, s->prim, x, y, sz, rot, s->rgb, a);
    }
}

static void fx_draw_stars(fx_effect_t *fx, float t, float fade, float sc)
{
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_elastic(fx_prog(t, s->delay, FX_IN_S));
        if (k <= 0.0f) continue;
        float tw = 1.0f + 0.15f * sinf(t * 3.2f + i * 2.1f) + s_beat_p * 0.18f;
        float x = FX_CX + (s->x - FX_CX) * sc;
        float y = FX_CY + (s->y - FX_CY) * sc;
        float r = s->r * k * tw * sc;
        float rot = s->rot + t * 0.7f * fx->dir;
        uint8_t a = (uint8_t)(0.97f * fade * 255.0f);
        fx_draw_piece(s_buf, FX_PRIM_STAR, x, y, r, rot, s->rgb, a);
    }
}

static void fx_draw_wave(fx_effect_t *fx, float t, float fade, float sc)
{
    const float step = 14.0f;
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_cubic(fx_prog(t, s->delay, 0.6f));
        if (k <= 0.0f) continue;
        float off = (1.0f - k) * (FX_CANVAS_W + 120.0f) * s->side;
        float amp = s->amp * (0.6f + 0.4f * k) * (1.0f + s_beat_p * 0.3f) * sc;
        float th = s->th * (1.0f + s_beat_p * 0.12f) * sc;
        uint8_t a = (uint8_t)(0.9f * fade * 255.0f);
        for (float x = -60.0f; x <= FX_CANVAS_W + 60.0f; x += step) {
            float xx = x + off;
            if (xx < -4.0f || xx >= FX_CANVAS_W + 4.0f) continue;
            float ph = (x / s->wl) * FX_PI * 2.0f + t * s->speed;
            float yTop = s->y0 + sinf(ph) * amp;
            float yBot = s->y0 + th + sinf(ph + 0.9f) * amp;
            int yy0 = (int)floorf(fminf(yTop, yBot));
            int yy1 = (int)ceilf(fmaxf(yTop, yBot));
            fx_fill_vspan(s_buf, (int)xx, yy0, yy1, s->rgb, a);
        }
    }
}

static void fx_draw_rays(fx_effect_t *fx, float t, float fade, float sc)
{
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_cubic(fx_prog(t, s->delay, 0.5f));
        if (k <= 0.0f) continue;
        float rot = fx->rot0 + fx->dir * (1.0f - k) * 0.8f + t * 0.14f * fx->dir;
        float len = s->len * k * (1.0f + s_beat_p * 0.09f) * sc;
        float a = s->ang + rot;
        float r1 = fx->dot_r * sc + len;
        float w = s->w;
        /* 楔形：中心 + 外弧上 3 个点（a-w, a, a+w） */
        float v[4][2];
        v[0][0] = FX_CX;
        v[0][1] = FX_CY;
        for (int j = 1; j < 4; j++) {
            float aa = a - w + (2.0f * w) * ((float)(j - 1) * 0.5f);
            v[j][0] = FX_CX + cosf(aa) * r1;
            v[j][1] = FX_CY + sinf(aa) * r1;
        }
        uint8_t aa = (uint8_t)(0.88f * fade * 255.0f);
        fx_fill_poly(s_buf, v, 4, s->rgb, aa);
    }
}

static void fx_draw_spiral(fx_effect_t *fx, float t, float fade, float sc)
{
    float rot = fx->rot0 + t * 0.45f * fx->dir + s_beat_p * 0.05f * fx->dir;
    for (int i = 0; i < fx->n; i++) {
        fx_shape_t *s = &fx->sh[i];
        float k = fx_ease_out_back(fx_prog(t, s->delay, FX_IN_S));
        if (k <= 0.0f) continue;
        float a = s->ang + rot;
        float r = (s->rad * k * (1.0f + s_beat_p * 0.04f) + sinf(t * 1.5f + i * 0.5f) * 4.0f) * sc;
        float x = FX_CX + cosf(a) * r;
        float y = FX_CY + sinf(a) * r;
        float sz = s->size * k * (1.0f + s_beat_p * 0.25f) * sc;
        uint8_t aa = (uint8_t)(fade * 255.0f);
        fx_draw_piece(s_buf, s->prim, x, y, sz, a, s->rgb, aa);
    }
}

/* ---------------- 定时器 ---------------- */

static void fx_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    fx_effects_update(esp_timer_get_time());
}

/* ---------------- 对外接口 ---------------- */

void fx_effects_init(void)
{
    s_buf = heap_caps_malloc(FX_CANVAS_W * FX_CANVAS_H * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        /* PSRAM 不足时退回内部堆（8MB 板子几乎不可能失败） */
        s_buf = heap_caps_malloc(FX_CANVAS_W * FX_CANVAS_H * 4, MALLOC_CAP_8BIT);
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "fx canvas buffer alloc failed, FX disabled");
        return;
    }
    memset(s_buf, 0, FX_CANVAS_W * FX_CANVAS_H * 4);

    /* source-over 除法的倒数表：s_inv[oa] ≈ 2^32/(oa*255) */
    for (int i = 1; i < 256; i++) {
        s_inv[i] = (uint32_t)(4294967296.0 / (double)i / 255.0);
    }

    s_canvas = lv_canvas_create(lv_screen_active());
    lv_obj_set_size(s_canvas, FX_CANVAS_W, FX_CANVAS_H);
    lv_canvas_set_buffer(s_canvas, s_buf, FX_CANVAS_W, FX_CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_canvas);

    for (int i = 0; i < FX_POOL; i++) s_fx[i].state = FX_ST_DEAD;
    s_head = 0;
    s_active = false;

    s_timer = lv_timer_create(fx_timer_cb, FX_UPDATE_MS, NULL);
}

void fx_effects_spawn(int32_t x, int32_t y)
{
    (void)x;
    (void)y;
    if (s_buf == NULL) return;

    const float now_s = (float)esp_timer_get_time() / 1e6f;

    /* 随机选一种特效，并用硬件随机数种子重置 mulberry32 */
    uint8_t type = (uint8_t)(esp_random() % FX_TYPE_COUNT);
    s_rng_state = esp_random();

    /* 当前最新特效进入退场 */
    fx_effect_t *cur = &s_fx[s_head];
    if (cur->state != FX_ST_DEAD && cur->state != FX_ST_OUT) {
        cur->state = FX_ST_OUT;
        cur->out_t0_s = now_s;
    }

    /* 换到另一个槽位生成新特效（快速连点时覆盖最旧） */
    s_head = (uint8_t)((s_head + 1) % FX_POOL);
    fx_effect_t *fx = &s_fx[s_head];
    fx_build(fx, type);
    fx->t0_s = now_s;
}

void fx_effects_update(int64_t now_us)
{
    if (s_buf == NULL) return;

    const float now_s = (float)now_us / 1e6f;
    s_beat_p = fx_beat_pulse(now_us);

    /* 清空透明，然后按「旧→新」顺序画进同一 buffer */
    memset(s_buf, 0, FX_CANVAS_W * FX_CANVAS_H * 4);

    bool active = false;
    for (int step = 1; step <= FX_POOL; step++) {
        fx_effect_t *fx = &s_fx[(s_head + step) % FX_POOL];
        if (fx->state == FX_ST_DEAD) continue;

        float out_k = 0.0f;
        if (fx->state == FX_ST_OUT) {
            out_k = fx_clamp01((now_s - fx->out_t0_s) / FX_OUT_S);
            if (out_k >= 1.0f) {
                fx->state = FX_ST_DEAD;
                continue;
            }
        }
        float t = now_s - fx->t0_s;
        if (t < 0.0f) continue;

        /* 不点击时超时自动消失：入场 + 短暂常驻后转退场渐隐 */
        if (fx->state == FX_ST_IN && t >= FX_AUTO_LIFE_S) {
            fx->state = FX_ST_OUT;
            fx->out_t0_s = now_s;
        }

        active = true;
        float fade = 1.0f - fx_smooth(out_k);
        float sc = (fx->state == FX_ST_OUT) ? (1.0f - 0.22f * out_k) : (1.0f + s_beat_p * 0.02f);

        switch (fx->type) {
        case FX_TYPE_RINGS:   fx_draw_rings(fx, t, fade, sc); break;
        case FX_TYPE_CONFETTI: fx_draw_confetti(fx, t, fade, sc); break;
        case FX_TYPE_STARS:   fx_draw_stars(fx, t, fade, sc); break;
        case FX_TYPE_WAVE:    fx_draw_wave(fx, t, fade, sc); break;
        case FX_TYPE_RAYS:    fx_draw_rays(fx, t, fade, sc); break;
        case FX_TYPE_SPIRAL:  fx_draw_spiral(fx, t, fade, sc); break;
        default: break;
        }
    }

    /* 空闲时隐藏 canvas，彻底省掉混合成本 */
    if (active) {
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_canvas);
    } else {
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    s_active = active;
}

bool fx_effects_is_active(void)
{
    return s_active;
}
