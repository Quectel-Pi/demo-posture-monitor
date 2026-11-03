#!/usr/bin/env python3
"""画像级联「迟滞」离线验证。

复刻 PostureProfile::ClassifyHysteretic + PostureAnalyzer::ProfileUpdate 的
标签保持逻辑，在 114 帧真标注数据上对比「有迟滞 / 无迟滞」的抖动次数与分类结果。

为什么需要专门的抖动指标：单帧分类准确率看不出抖动问题 —— 一段连续样本里
每帧都对，但只要两帧在阈值附近来回跨，界面就会闪、语音就会反复播。
所以这里两个指标一起看：
  · accuracy       单帧判定是否等于真标注（不能因为加迟滞而下降）
  · flip_frames    每秒发生的标签切换次数（越低越稳，这是用户能感知的「稳不稳」）

用法：
  python3 scripts/verify_hysteresis.py            # 用 /tmp/pm-samples
  python3 scripts/verify_hysteresis.py <样本目录>
"""
import glob
import json
import os
import statistics
import sys

NF = 6
FEATURES = ["head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise",
            "wrist_near"]
FLOORS = [0.05, 3.0, 2.0, 0.04, 0.10, 0.15]
# 与 PostureProfile::ReleaseScale() 必须完全一致
RELEASE_SCALE = [0.08, 2.0, 2.0, 0.10, 0.10, 0.15]

SAMPLE_DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pm-samples"
FPS = 8.0          # camera.infer_fps 实测有效帧率
HOLD_S = 0.5       # posture.profile_hold_s


def load():
    """返回 {label: [FeatureVec, ...]}"""
    out = {}
    for p in sorted(glob.glob(os.path.join(SAMPLE_DIR, "*.json"))):
        d = json.load(open(p))
        out[d["label"]] = [list(map(float, f)) for f in d["features"]]
    return out


def build_rules(samples):
    """复刻 teach_posture.py::build_rules 的核心（阈值 = normal 与该类中位中点）。

    这里不追求和生产完全一致（生产阈值是搜出来的），目的是让迟滞效果可以在
    不依赖板端 postures.txt 的情况下离线复现。规则本身取自板端已部署版本。
    """
    nm = [statistics.median([v[i] for v in samples["normal"]]) for i in range(NF)]
    cand = {
        "head_down": "head_drop",
        "head_tilt": "ear_tilt",
        "too_close": "size",
        "hand_face": "wrist_rise",
    }
    prio = {"head_tilt": 1, "head_down": 2, "too_close": 3, "hand_face": 4}
    rules = []
    for label, fname in cand.items():
        k = FEATURES.index(fname)
        cur = statistics.median([v[k] for v in samples[label]])
        thr = (nm[k] + cur) / 2.0
        guard = None
        if label == "head_down" and "head_tilt" in samples:
            ke = FEATURES.index("ear_tilt")
            ht = statistics.median([v[ke] for v in samples["head_tilt"]])
            guard = (ke, (nm[ke] + ht) / 2.0)
        rules.append({"priority": prio[label], "label": label, "k": k, "thr": thr,
                      "normal_median": nm[k], "guard": guard})
    rules.sort(key=lambda r: r["priority"])
    return rules


def release_band(rule, ratio):
    if ratio <= 0:
        return 0.0
    k = rule["k"]
    band = max(RELEASE_SCALE[k], FLOORS[k]) * ratio
    span = abs(rule["thr"] - rule["normal_median"])
    return min(band, span) if span > 1e-9 else band


def classify(v, rules, active, ratio):
    """复刻 ClassifyHysteretic。active=None 时等价于纯 Classify。"""
    for r in rules:
        g = r["guard"]
        if g and v[g[0]] >= g[1]:
            continue
        thr = r["thr"]
        if active is not None and r["label"] == active:
            thr = thr - release_band(r, ratio)      # 只放宽「退出」
        if v[r["k"]] >= thr:
            return r["label"]
    return "normal"


def replay(vs, rules, ratio, hold_frames):
    """复刻 ProfileUpdate 的标签保持：同一标签连续 hold_frames 帧才采纳。"""
    active = None
    pending = None
    cnt = 0
    seq = []
    for v in vs:
        lab = classify(v, rules, active, ratio)
        if lab == pending:
            cnt += 1
        else:
            pending, cnt = lab, 1
        if cnt >= hold_frames and lab != active:
            active = lab
        seq.append(active)
    return seq


def flips(seq):
    return sum(1 for i in range(1, len(seq))
               if seq[i] is not None and seq[i - 1] is not None
               and seq[i] != seq[i - 1])


def main():
    samples = load()
    rules = build_rules(samples)
    hold_frames = max(1, round(HOLD_S * FPS))

    print(f"样本目录 {SAMPLE_DIR}  共 {sum(len(v) for v in samples.values())} 帧")
    print(f"保持门限 {HOLD_S}s -> {hold_frames} 帧 @ {FPS:g}fps")
    print()
    print("规则（阈值 = normal 与该类中位的中间点，guard 为互斥门控）:")
    for r in rules:
        g = f"  guard {FEATURES[r['guard'][0]]} < {r['guard'][1]:.3f}" if r["guard"] else ""
        print(f"  P{r['priority']} {r['label']:<10} {FEATURES[r['k']]:<12} "
              f">= {r['thr']:.4f}{g}")
        print(f"      释放带 @0.5 = {release_band(r, 0.5):.4f}")
    print()

    results = {}
    for ratio in (0.0, 0.25, 0.5, 0.75, 1.0):
        correct = total = 0
        flip_total = 0
        per_label = {}
        for label, vs in samples.items():
            seq = replay(vs, rules, ratio, hold_frames)
            for truth, got in zip([label] * len(vs), seq):
                total += 1
                if got == truth:
                    correct += 1
            f = flips(seq)
            flip_total += f
            per_label[label] = f
        results[ratio] = (correct, total, flip_total, per_label)

    print(f"{'迟滞比例':<10}{'单帧准确率':>12}{'抖动次数':>10}   每类抖动")
    for ratio, (c, t, fl, pl) in results.items():
        detail = " ".join(f"{k}={v}" for k, v in pl.items())
        print(f"{ratio:<10}{c / t:>11.1%}{fl:>10}   {detail}")

    base_fl = results[0.0][2]
    best = min(results.items(), key=lambda kv: (kv[1][2], -kv[1][0]))
    print()
    print(f"无迟滞抖动 {base_fl} 次；迟滞比例 {best[0]} 抖动 {best[1][2]} 次，"
          f"单帧准确率 {best[1][0] / best[1][1]:.1%}")
    if best[1][2] == 0 and base_fl == 0:
        print("注意：本数据集是「单姿势连续段」，段内特征几乎不变，"
              "本身就不产生抖动 —— 抖动只在段间过渡帧出现。"
              "所以本脚本的抖动指标只能验证「迟滞没把准确率弄坏」，"
              "不能证明现场抖动会下降；现场抖动证据见 journal 统计。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
