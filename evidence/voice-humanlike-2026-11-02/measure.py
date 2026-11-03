#!/usr/bin/env python3
"""客观指标：客观指标表（时长/峰值/RMS/削顶/F0/'AI 味'代理量）。

"AI 味" 没有单一指标，这里用 5 个可算、可复现的代理量：
  F0 std / cv        语调起伏。机器味重的合成音往往 cv < 8%（死平）；
                     真人提醒句 cv 在 15~30%。
  F0 range (st)      p10..p90 的基频跨度（半音）。真人 5~12st，播报腔常 >14st。
  F0 jitter (%)      相邻帧基频相对变化中位数。真人有微扰 0.5~3%；
                     拼接式（espeak）常落在 <0.3% 或 >5%（跳八度）。
  centroid (Hz)      谱质心。神经 TTS 常见 3~5kHz 偏亮 = "电子味"；
                     越低越接近胸腔共鸣的人声。
  hf_ratio (%)       5kHz 以上能量占比。旧的 espeak 24k 素材 HF 能量异常高
                     （见 REPORT 对比），听感刺耳。
  clip               满刻度样本数，必须为 0（旧素材有削顶）。

用法：
    python3 measure.py                      # 量 assets/voice/*.wav
    python3 measure.py path/a.wav ...       # 量指定文件
    python3 measure.py --json out.json      # 同时存机器可读结果
"""
from __future__ import annotations

import json
import math
import struct
import sys
import statistics
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEYS = ('shoulder_tilt', 'head_down', 'head_tilt', 'too_close', 'hand_on_face', 'rest')
FULL_SCALE = 32767


def load(path: Path):
    with wave.open(str(path), 'rb') as w:
        n, r = w.getnframes(), w.getframerate()
        ch, wd = w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if wd != 2:
        raise SystemExit(f'{path}: 只支持 16bit PCM')
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw)), r, n, ch, wd * 8


def f0_track(v, r, win=1024, hop=256, lo=70, hi=420, thr=0.45):
    """逐帧归一化自相关 → 基频轨迹（只保留浊音帧）。"""
    track = []
    for s in range(0, max(0, len(v) - win), hop):
        fr = v[s:s + win]
        if sum(x * x for x in fr) < (0.02 * FULL_SCALE) ** 2 * win:
            continue
        m = sum(fr) / win
        fr = [x - m for x in fr]
        best, bl = 0.0, 0
        for lag in range(max(2, r // hi), r // lo):
            num = sum(fr[i] * fr[i + lag] for i in range(win - lag))
            den = math.sqrt(sum(x * x for x in fr[:win - lag]) *
                            sum(x * x for x in fr[lag:])) + 1e-9
            if num / den > best:
                best, bl = num / den, lag
        if best > thr and bl:
            track.append(r / bl)
    return track


def spectral(v, r, win=1024, hop=512):
    """用 numpy 算谱质心 / 5kHz 以上能量占比（没有 numpy 则跳过）。"""
    try:
        import numpy as np
    except ImportError:
        return None, None
    x = np.asarray(v, dtype=np.float64) / FULL_SCALE
    if len(x) < win:
        return 0.0, 0.0
    freqs = np.fft.rfftfreq(win, 1 / r)
    winfn = np.hanning(win)
    cen_num = cen_den = hf = tot = 0.0
    for s in range(0, len(x) - win, hop):
        seg = x[s:s + win] * winfn
        if not np.any(seg):
            continue
        mag = np.abs(np.fft.rfft(seg))
        p = mag ** 2
        ssum = p.sum()
        if ssum <= 1e-12:
            continue
        cen_num += float((freqs * p).sum())
        cen_den += ssum
        hf += float(p[freqs >= 5000].sum())
        tot += ssum
    return (cen_num / cen_den if cen_den else 0.0,
            hf / tot * 100 if tot else 0.0)


def silence_ratio(v, r, thr=0.01, hop=240):
    """10ms 帧里低于 -40dBFS 的比例，反映句内停顿/拖沓。"""
    frames = [v[s:s + hop] for s in range(0, len(v), hop)]
    quiet = 0
    for fr in frames:
        if not fr:
            continue
        rms = math.sqrt(sum(x * x for x in fr) / len(fr))
        if rms < thr * FULL_SCALE:
            quiet += 1
    return quiet / len(frames) * 100 if frames else 0.0


def measure(path: Path) -> dict:
    v, r, n, ch, bits = load(path)
    peak = max((abs(x) for x in v), default=0)
    rms = math.sqrt(sum(x * x for x in v) / len(v)) if v else 0.0
    clip = sum(1 for x in v if abs(x) >= FULL_SCALE)
    near_clip = sum(1 for x in v if abs(x) >= FULL_SCALE * 0.999)
    t = f0_track(v, r)
    cen, hf = spectral(v, r)
    m = {
        'file': path.name,
        'seconds': n / r, 'rate': r, 'channels': ch, 'bits': bits,
        'peak_pct': peak / FULL_SCALE * 100,
        'rms_pct': rms / FULL_SCALE * 100,
        'clip_samples': clip, 'near_clip_samples': near_clip,
        'silence_pct': silence_ratio(v, r),
        'centroid_hz': cen, 'hf_ratio_pct': hf,
    }
    if t:
        s = sorted(t)
        p10, p90 = s[int(len(s) * .10)], s[min(len(s) - 1, int(len(s) * .90))]
        d = [abs(t[i] - t[i - 1]) / t[i - 1] * 100 for i in range(1, len(t))]
        m.update({
            'voiced_frames': len(t),
            'f0_med': statistics.median(t),
            'f0_std': statistics.pstdev(t) if len(t) > 1 else 0.0,
            'f0_cv_pct': (statistics.pstdev(t) / statistics.fmean(t) * 100) if len(t) > 1 else 0.0,
            'f0_range_st': 12 * math.log2(p90 / p10) if p10 > 0 else 0.0,
            'f0_jitter_pct': statistics.median(d) if d else 0.0,
            'truncated': False,
        })
    else:
        m.update({'voiced_frames': 0, 'f0_med': 0.0, 'f0_std': 0.0, 'f0_cv_pct': 0.0,
                  'f0_range_st': 0.0, 'f0_jitter_pct': 0.0, 'truncated': True})
    # 截断判据：结尾 30ms 还有 >2% 满刻度的能量、或者首 5ms 就起音 → TTS 被切断
    tail = v[-int(0.03 * r):] if len(v) > int(0.03 * r) else v
    tail_rms = math.sqrt(sum(x * x for x in tail) / len(tail)) / FULL_SCALE * 100 if tail else 0
    m['tail_rms_pct'] = tail_rms
    m['truncated'] = tail_rms > 2.0
    return m


def fmt(m: dict) -> str:
    return (f"{m['file']:28s} {m['seconds']:5.2f}s {m['rate']:6d}Hz {m['channels']}ch {m['bits']}b "
            f"peak={m['peak_pct']:5.1f}% rms={m['rms_pct']:5.1f}% clip={m['clip_samples']:2d} "
            f"F0={m['f0_med']:6.1f} std={m['f0_std']:5.1f} cv={m['f0_cv_pct']:4.1f}% "
            f"rng={m['f0_range_st']:4.1f}st jit={m['f0_jitter_pct']:3.1f}% "
            f"cen={m['centroid_hz']:6.0f}Hz hf={m['hf_ratio_pct']:4.1f}% sil={m['silence_pct']:4.1f}%")


def main() -> int:
    args = [a for a in sys.argv[1:]]
    out_json = None
    if '--json' in args:
        i = args.index('--json')
        out_json = Path(args[i + 1])
        del args[i:i + 2]
    if args:
        paths = [Path(a) for a in args]
    else:
        paths = [ROOT / 'assets' / 'voice' / f'{k}.wav' for k in KEYS]
        paths += sorted((ROOT / 'assets' / 'voice' / 'variants').glob('*.wav'))
    results = []
    for p in paths:
        if not p.is_file():
            print(f'{p}: 缺失', file=sys.stderr)
            continue
        m = measure(p)
        results.append(m)
        print(fmt(m))
        if m['truncated']:
            print(f'    ⚠ 结尾仍有 {m["tail_rms_pct"]:.1f}% 能量，疑似被截断')
    if out_json:
        out_json.write_text(json.dumps(results, ensure_ascii=False, indent=2))
        print(f'\n→ {out_json}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
