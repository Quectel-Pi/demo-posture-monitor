"""无界面模式：跑 N 秒，周期性打印判定结果。用于部署自检与回归。

    python3 -m posturemon.headless --seconds 20
    python3 -m posturemon.headless --source /opt/posture-monitor/samples/sit.mp4 --seconds 60

另外提供 --selftest-rules：用合成关键点直接验证判定规则（不需要人、不需要摄像头），
部署脚本把它当冒烟测试用。
"""
from __future__ import annotations

import argparse
import json
import sys
import time

import numpy as np

from . import config as cfgmod
from .rules import PostureEngine, VIOLATION_LABEL, measure

# 一组合成的“正常坐姿”关键点（640 空间，画面中央的人）
BASE = np.array([
    [320, 150, 1], [312, 140, 1], [328, 140, 1], [300, 145, 1], [340, 145, 1],
    [270, 220, 1], [370, 220, 1],          # 肩
    [250, 300, 1], [390, 300, 1],          # 肘
    [235, 370, 1], [405, 370, 1],          # 手腕（放在桌面上）
    [285, 350, 1], [355, 350, 1],          # 髋
    [285, 470, 1], [355, 470, 1],          # 膝
    [285, 600, 1], [355, 600, 1],          # 踝
], dtype=np.float32)


def _shift(**kw):
    k = BASE.copy()
    for name, (dx, dy) in kw.items():
        idx = {"nose": 0, "sh_l": 5, "sh_r": 6, "wr_l": 9, "wr_r": 10,
               "hip_l": 11, "hip_r": 12}[name]
        k[idx] = [k[idx][0] + dx, k[idx][1] + dy, 1]
    return k


def selftest_rules(cfg) -> int:
    """用合成关键点验证 6 条规则都能独立触发。返回失败数。"""
    cases = {
        "upright": (BASE, None,
                    "正常坐姿应无违规"),
        "head_down": (_shift(nose=(0, 130)), "head_down", "鼻子下移 130px 应判低头"),
        "hand_on_face": (_shift(wr_r=(-50, -185)), "hand_on_face", "右手抬到脸旁应判托腮"),
        "uneven_shoulder": (_shift(sh_l=(0, 40)), "uneven_shoulder", "左肩下移 40px 应判高低肩"),
        "head_tilt": (_tilt_eyes(28), "head_tilt", "双眼连线 28° 应判歪头"),
        "lean_forward": (_shift(nose=(120, 0)), "lean_forward", "鼻子右移 120px 应判前倾"),
    }
    fails = 0
    for name, (kpts, expect, desc) in cases.items():
        eng = PostureEngine(cfg)
        t = 1000.0
        # 推进 3 秒，跨过 hold 阈值
        for i in range(0, 31):
            eng.update(kpts, 640.0, box=(200, 120, 200, 500), now=t + i * 0.1)
        act = list(eng.st.active)
        bad = act if act else None
        ok = (expect is None and not act) or (expect is not None and expect in act)
        fails += 0 if ok else 1
        exp_s = str(expect or "无违规")
        got_s = "、".join(bad) if bad else "无违规"
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:16s} 期望={exp_s:14s} "
              f"实得={got_s:14s} {desc}")
        m = measure(kpts, 640.0)
        print(f"         指标 head_drop={m.head_drop:.2f} shoulder_tilt={m.shoulder_tilt_deg:.1f}° "
              f"head_tilt={m.head_tilt_deg:.1f}° hand_face={m.hand_to_face:.2f} "
              f"lean={m.lean:.2f} width_ratio={m.width_ratio:.2f}")
    return fails


def _tilt_eyes(deg):
    import math
    k = BASE.copy()
    rad = math.radians(deg)
    for i, sign in ((1, -1), (2, 1)):
        dx = sign * 8 * math.cos(rad)
        dy = sign * 8 * math.sin(rad)
        k[i] = [320 + dx, 140 + dy, 1]
    return k


def run_pipeline(cfg, source, seconds, quiet=False) -> int:
    from .pipeline import Pipeline
    pipe = Pipeline(cfg, source=source, use_camera=(source is None)).start()
    t0 = time.time()
    last = 0.0
    try:
        while time.time() - t0 < seconds:
            time.sleep(0.2)
            if time.time() - last < 1.0:
                continue
            last = time.time()
            s = pipe.snapshot()
            if not quiet:
                print(f"  t={time.time()-t0:5.1f}s {s['backend']} {s['latency_ms']}ms "
                      f"persons={s['persons']} present={s['present']} "
                      f"bad={s['bad']} main={s['label'] or '-'} "
                      f"bad_dur={s['bad_duration_s']:.1f}s "
                      f"head_drop={s['metrics']['head_drop']:.2f} "
                      f"shoulder={s['metrics']['shoulder_tilt_deg']:.1f}")
    finally:
        snap = pipe.snapshot()
        pipe.stop()
    print(json.dumps(snap, ensure_ascii=False, indent=1, default=str))
    return 0 if snap["infer_count"] > 0 else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--source", default=None, help="视频文件回放，默认用摄像头")
    ap.add_argument("--selftest-rules", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    cfg = cfgmod.Config.load()
    if a.selftest_rules:
        fails = selftest_rules(cfg)
        print(f"  规则自检：{'全部通过' if not fails else f'{fails} 项失败'}")
        return 1 if fails else 0
    return run_pipeline(cfg, a.source, a.seconds, a.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
