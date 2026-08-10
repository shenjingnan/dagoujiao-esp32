#!/usr/bin/env python3
"""BGM 参考渲染器：把 main/bgm.c 的软合成算法复刻成 32kHz 16bit 单声道 WAV，
用于离线试听对比与 BGM_DEFAULT_GAIN / 各声部比例定标。与 C 代码一一对应。

用法：
    python3 tools/bgm_ref.py [输出.wav]        # 默认输出 tools/bgm_ref.wav
"""
import numpy as np
import scipy.signal as sps
import wave
import sys
import os

SR = 32000
S16 = 3750          # 16 分音符样本数
S8 = 7500           # 8 分音符样本数
N = 64 * S16        # 64 步完整循环样本数

CHORDS = [
    (65.41, [261.63, 329.63, 392.00, 523.25]),  # C
    (49.00, [196.00, 246.94, 293.66, 392.00]),  # G
    (55.00, [220.00, 261.63, 329.63, 440.00]),  # Am
    (43.65, [174.61, 220.00, 261.63, 349.23]),  # F
]
HAT_VEL = [0.34, 0.16, 0.42, 0.16]
GAIN = 0.30          # BGM_DEFAULT_GAIN，改这里试听

track = np.zeros(N, dtype=np.float64)
rng = np.random.default_rng(2024)


def add(buf, t0):
    buf = buf[: max(0, N - t0)]
    track[t0:t0 + len(buf)] += buf


def env_exp(start, target, dur):
    """指数斜坡：从 start 到 target，共 dur 样本（与 C 的 amp *= k 一致）。"""
    return start * (target / start) ** (np.arange(dur) / dur)


def noise(dur):
    return rng.uniform(-1.0, 1.0, dur)


def onepole_lp(x, fc):
    a = 1.0 - np.exp(-2 * np.pi * fc / SR)
    return sps.lfilter([a], [1.0, -(1.0 - a)], x)


def onepole_hp(x, fc):
    a = np.exp(-2 * np.pi * fc / SR)
    return sps.lfilter([a], [1.0, -a], x)  # y[n]=a*y[n-1]+a*(x[n]-x[n-1])，初值0


# ---------- 乐器 ----------

def kick():
    dur = 8320
    t = np.arange(dur)
    freq = 160.0 * (45.0 / 160.0) ** (t / 3520.0)
    phase = np.cumsum(freq) / SR
    s = np.sin(2 * np.pi * phase)
    return s * env_exp(0.95, 0.001, dur)


def snare_noise(vol):
    dur = 5760
    n = noise(dur)
    n = onepole_hp(n, 1800.0)
    n = onepole_lp(n, 1800.0)
    return n * env_exp(vol, 0.001, dur)


def snare_tone(vol):
    dur = 3200
    phase = np.arange(dur) * (240.0 / SR)
    tri = 2.0 * np.abs(2.0 * (phase % 1.0) - 1.0) - 1.0
    return tri * env_exp(vol * 0.5, 0.001, dur)


def hat(vel, decay_sec):
    decay_n = int(round(decay_sec * SR))
    dur = decay_n + 640
    n = onepole_hp(noise(dur), 7500.0)
    env = env_exp(vel, 0.001, decay_n)
    env = np.pad(env, (0, dur - len(env)), mode="edge")  # 衰减后保持到 dur（对应 C 中尾段继续衰减，可听范围内无差别）
    return n * env


def crash():
    dur = 41600
    n = onepole_hp(noise(dur), 5000.0)
    return n * env_exp(0.32, 0.001, dur)


def stab(notes):
    dur = 9600
    x = np.zeros(dur)
    for fr in notes:
        for det in (-6.0, 5.0):
            f = fr * 2.0 ** (det / 1200.0)
            phase = (np.arange(dur) * (f / SR)) % 1.0
            x += 1.0 - 2.0 * phase
    # 低通扫频 2600->600，扫系数
    a0 = 1.0 - np.exp(-2 * np.pi * 2600 / SR)
    a1 = 1.0 - np.exp(-2 * np.pi * 600 / SR)
    a = a0 * (a1 / a0) ** (np.arange(dur) / 8960.0)
    a = np.minimum(a, a1)  # 0.28s 后保持
    y = np.empty(dur)
    acc = 0.0
    for i in range(dur):
        acc += a[i] * (x[i] - acc)
        y[i] = acc
    attack = env_exp(0.0001, 0.14, 320)
    decay = env_exp(0.14, 0.001, 8640)
    env = np.concatenate([attack, decay])[:dur]
    env = np.pad(env, (0, dur - len(env)), mode="edge")
    return y * env


def bass(root, vol):
    dur = S8
    phase = (np.arange(dur) * (root * 2.0 / SR)) % 1.0
    sq = np.where(phase < 0.5, 1.0, -1.0)
    y = onepole_lp(sq, 300.0)
    attack = env_exp(0.0001, vol, 320)
    decay = env_exp(vol, 0.001, 6430)
    env = np.concatenate([attack, decay])[:dur]
    env = np.pad(env, (0, dur - len(env)), mode="edge")
    return y * env


# ---------- 音序器 ----------

def trigger(s):
    bar, pos = s >> 4, s & 15
    ch = CHORDS[bar]
    t0 = s * S16
    if bar == 0 and pos == 0:
        add(crash(), t0)
    if pos % 4 == 0:
        add(kick(), t0)
    if pos in (4, 12):
        add(snare_noise(0.5), t0)
        add(snare_tone(0.5), t0)
    if bar == 3 and pos == 14:
        add(snare_noise(0.3), t0)
        add(snare_tone(0.3), t0)
    add(hat(HAT_VEL[pos % 4], 0.12 if pos == 14 else 0.04), t0)
    if pos % 4 == 2:
        add(stab(ch[1]), t0)
    if pos % 2 == 0:
        add(bass(ch[0], 0.4 if pos % 4 == 0 else 0.26), t0)


for s in range(64):
    trigger(s)

pcm = np.clip(track * GAIN * 32767.0, -32768, 32767).astype(np.int16)

peak = np.abs(pcm).max()
rms = np.sqrt((pcm.astype(np.float64) ** 2).mean())
print(f"peak={peak/32767.0:.3f} rms={rms/32767.0:.3f} (相对满量程)")

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "bgm_ref.wav")
with wave.open(out, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"written: {out} ({N/SR:.2f}s, {SR}Hz 16bit mono)")
