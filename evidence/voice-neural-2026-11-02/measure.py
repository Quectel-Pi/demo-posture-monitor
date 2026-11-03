#!/usr/bin/env python3
"""量测 assets/voice/*.wav 的时长/峰值/RMS/F0（浊音中位基频），证明是女声。"""
import math, statistics, struct, sys, wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEYS = ('shoulder_tilt', 'head_down', 'head_tilt', 'too_close', 'hand_on_face', 'rest')

def load(p):
    with wave.open(str(p), 'rb') as w:
        n, r = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw)), r, n

def f0_median(v, r, win=1024, hop=512, lo=70, hi=400):
    est = []
    for s in range(0, len(v) - win, hop):
        fr = v[s:s + win]
        if sum(x * x for x in fr) < (0.02 * 32767) ** 2 * win:
            continue
        m = sum(fr) / win
        fr = [x - m for x in fr]
        best, bl = 0.0, 0
        for lag in range(r // hi, r // lo):
            num = sum(fr[i] * fr[i + lag] for i in range(win - lag))
            den = math.sqrt(sum(x * x for x in fr[:win - lag]) *
                            sum(x * x for x in fr[lag:])) + 1e-9
            if num / den > best:
                best, bl = num / den, lag
        if best > 0.5 and bl:
            est.append(r / bl)
    return statistics.median(est) if est else 0.0

for key in KEYS:
    p = ROOT / 'assets' / 'voice' / f'{key}.wav'
    v, r, n = load(p)
    peak = max((abs(x) for x in v), default=0)
    rms = math.sqrt(sum(x * x for x in v) / len(v)) if v else 0
    print(f'{key:14s} {n/r:5.2f}s {r}Hz mono16 peak={peak/327.67:5.1f}% '
          f'rms={rms/327.67:5.1f}% F0={f0_median(v, r):6.1f}Hz')
