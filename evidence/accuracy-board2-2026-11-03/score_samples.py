#!/usr/bin/env python3
"""用板上真实采集的特征向量（同基准、已标注）评分当前生效规则。

数据来源 /var/lib/posture-guard/posture-samples/*.json，
5 个类共用同一校准基准（2026-10-29T14:35:29），所以可以横向比较。
"""
import json, glob, sys
from collections import Counter, defaultdict

FEAT = ["head_drop","ear_tilt","shoulder_tilt","size","wrist_rise","wrist_near"]

def rules_active():
    """板端 /var/lib/posture-guard/postures.txt 当前生效的 5 条规则。"""
    return [
        ("head_tilt",  lambda f: f[1] >= 13.35),
        ("head_down",  lambda f: f[0] >= 0.1035),
        ("too_close",  lambda f: f[3] >= 1.20),
        ("hand_face",  lambda f: f[4] >= 0.286 and f[5] < 0.55),
        ("shoulder_tilt", lambda f: f[2] >= 12.0),
    ]

def classify(f, rs):
    for lab, fn in rs:
        if fn(f): return lab
    return "normal"

def run(paths, tag):
    rs = rules_active()
    conf = defaultdict(Counter)
    for f in sorted(glob.glob(paths)):
        d = json.load(open(f))
        truth = d["label"]
        for row in d["features"]:
            conf[truth][classify(row, rs)] += 1
    preds = ["normal","head_down","head_tilt","too_close","hand_face","shoulder_tilt"]
    print(f"\n=== {tag} ===")
    print(f"{'真值':<14}" + "".join(f"{p[:10]:>11}" for p in preds) + "   召回    精确")
    ok = tot = 0
    per = {}
    for truth in ["normal","head_down","head_tilt","too_close","hand_face"]:
        if truth not in conf: continue
        c = conf[truth]; n = sum(c.values()); hit = c.get(truth, 0)
        ok += hit; tot += n
        tp = hit; fp = sum(conf[t].get(truth,0) for t in conf if t != truth)
        prec = tp/(tp+fp) if tp+fp else 0
        per[truth] = (hit, n, tp, fp)
        print(f"{truth:<14}" + "".join(f"{c.get(p,0):>11}" for p in preds)
              + f"  {hit}/{n}={100*hit/n:5.1f}%  {100*prec:5.1f}%")
    print(f"总体 {ok}/{tot} = {100*ok/tot:.1f}%")
    return conf, per

if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv)>1 else "/tmp/ps2/*.json", "当前生效规则（板端现场）")
