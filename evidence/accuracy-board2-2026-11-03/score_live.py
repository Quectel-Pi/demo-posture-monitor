#!/usr/bin/env python3
"""用板端【实际生效】的 postures.txt 给已标注样本打分。

★ 教训：不要在脚本里硬编码规则副本 —— 之前 score_samples.py 写死了旧规则
（含 wrist_near guard），规则改了以后脚本还在报旧分数，误导排查。
"""
import json, glob, sys, os
from collections import Counter, defaultdict

FEAT = ["head_drop","ear_tilt","shoulder_tilt","size","wrist_rise","wrist_near","wrist_face"]


def parse_rules(path):
    out = []
    for line in open(path):
        c = line.rstrip("\n").split("\t")
        if c[0] != "rule":
            continue
        pri, label, feat, direction = int(c[1]), c[2], c[3], c[4]
        thr = float(c[5])
        guard = None
        # rule ... <thr> <nmed> <cmed> [g_feat g_max [g2_feat g2_dir g2_max]]
        if len(c) >= 10:
            gi = FEAT.index(c[8])
            guard = (gi, float(c[9]))
        out.append({"label": label, "feature": FEAT.index(feat),
                    "dir": 1 if direction == "+" else -1,
                    "thr": thr, "guard": guard})
    out.sort(key=lambda r: 0)  # 文件里已按优先级排序
    return out


def classify(f, rules):
    for r in rules:
        if r["guard"] is not None:
            gi, gmax = r["guard"]
            if f[gi] >= gmax:
                continue
        if (f[r["feature"]] - r["thr"]) * r["dir"] > 0:
            return r["label"]
    return "normal"


def main():
    rules_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/postures_live.txt"
    samp = sys.argv[2] if len(sys.argv) > 2 else "/tmp/ps3/*.json"
    rules = parse_rules(rules_path)
    print("生效规则：")
    for r in rules:
        g = "" if r["guard"] is None else f"  guard {FEAT[r['guard'][0]]}<{r['guard'][1]}"
        print(f"  {r['label']:14} {FEAT[r['feature']]:13} {'>=' if r['dir']>0 else '<='} {r['thr']}{g}")
    print()
    conf = defaultdict(Counter)
    for p in sorted(glob.glob(samp)):
        d = json.load(open(p))
        for row in d["features"]:
            conf[d["label"]][classify(row, rules)] += 1
    preds = ["normal","head_down","head_tilt","too_close","hand_face","shoulder_tilt"]
    print(f"{'真值':<14}" + "".join(f"{x[:10]:>11}" for x in preds) + "   召回")
    ok = tot = 0
    for t in ["normal","head_down","head_tilt","too_close","hand_face"]:
        if t not in conf: continue
        c = conf[t]; n = sum(c.values()); hit = c.get(t, 0)
        ok += hit; tot += n
        print(f"{t:<14}" + "".join(f"{c.get(x,0):>11}" for x in preds) + f"  {hit}/{n}={100*hit/n:5.1f}%")
    print(f"\n总体 {ok}/{tot} = {100*ok/tot:.1f}%")


if __name__ == "__main__":
    main()
