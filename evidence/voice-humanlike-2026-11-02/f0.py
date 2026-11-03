#!/usr/bin/env python3
"""F0 统计：逐帧自相关基频 → 中位数/均值/标准差/相对抖动，用来量化「平不平」。

F0 方差小 = 语调平 = 机器味重；真人说话逐字有升降 + 轻微微扰（jitter）。
"""
import math, statistics, struct, wave


def load(path):
    with wave.open(str(path), 'rb') as w:
        n, r = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw)), r, n


def f0_track(v, r, win=1024, hop=256, lo=70, hi=420, thr=0.45):
    track = []
    for s in range(0, max(0, len(v) - win), hop):
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
        if best > thr and bl:
            track.append(r / bl)
    return track


def stats(path):
    v, r, n = load(path)
    peak = max((abs(x) for x in v), default=0)
    rms = math.sqrt(sum(x * x for x in v) / len(v)) if v else 0
    t = f0_track(v, r)
    out = {
        'seconds': n / r, 'rate': r, 'peak_pct': peak / 327.67,
        'rms_pct': rms / 327.67, 'voiced_frames': len(t),
    }
    if t:
        med = statistics.median(t)
        out.update({
            'f0_med': med, 'f0_mean': statistics.fmean(t),
            'f0_std': statistics.pstdev(t) if len(t) > 1 else 0.0,
            'f0_p10': sorted(t)[max(0, int(len(t) * 0.10))],
            'f0_p90': sorted(t)[min(len(t) - 1, int(len(t) * 0.90))],
        })
        out['f0_range_st'] = 12 * math.log2(out['f0_p90'] / out['f0_p10']) if out['f0_p10'] > 0 else 0.0
        out['f0_cv_pct'] = out['f0_std'] / out['f0_mean'] * 100 if out['f0_mean'] else 0.0
        # 帧间抖动（相对值）：真人连读 <3%，拼接式/机械音要么极大（跳八度）要么极小（死平）
        d = [abs(t[i] - t[i - 1]) / t[i - 1] * 100 for i in range(1, len(t))]
        out['f0_jitter_pct'] = statistics.median(d) if d else 0.0
    else:
        out.update({'f0_med': 0, 'f0_mean': 0, 'f0_std': 0, 'f0_p10': 0,
                    'f0_p90': 0, 'f0_range_st': 0, 'f0_cv_pct': 0,
                    'f0_jitter_pct': 0})
    return out


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        s = stats(p)
        print(f"{p.split('/')[-1]:34s} {s['seconds']:5.2f}s peak={s['peak_pct']:5.1f}% "
              f"rms={s['rms_pct']:5.1f}% F0med={s['f0_med']:6.1f} F0std={s['f0_std']:5.1f} "
              f"cv={s['f0_cv_pct']:4.1f}% range={s['f0_range_st']:4.1f}st jit={s['f0_jitter_pct']:4.1f}%")
