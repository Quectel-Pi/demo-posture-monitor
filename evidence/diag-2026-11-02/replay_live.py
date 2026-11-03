#!/usr/bin/env python3
"""用生产板 68 的实时 status 帧（含 17 关键点）离线回放 7 维特征 + 托腮门控曲线。

为什么必须用实时帧而不是 /var/lib/posture-guard/posture-samples：
  那 114 帧是「单姿势连续段」，采集时被告诉过姿势，存在污染；
  status 帧是用户真实使用时的逐帧输出，没有任何人为姿势引导。

关键点顺序 = COCO-17；坐标已按 width/height 归一化，所以做距离前
必须乘回像素（640x480），否则 x/y 尺度不等会把距离算歪。
"""
import json, math, sys, statistics as st

W, H = 640.0, 480.0
KP = ["nose","eye_l","eye_r","ear_l","ear_r","sh_l","sh_r",
      "elbow_l","elbow_r","wrist_l","wrist_r","hip_l","hip_r",
      "knee_l","knee_r","ankle_l","ankle_r"]

def kp(row, name):
    i = KP.index(name)
    return row["kp"][2*i]*W, row["kp"][2*i+1]*H

def d(a, b): return math.hypot(a[0]-b[0], a[1]-b[1])

def features(row, bl):
    """与 PostureProfile::BuildFeatureVector 完全同一口径。"""
    sh_l, sh_r = kp(row,"sh_l"), kp(row,"sh_r")
    sw = d(sh_l, sh_r)
    nose, ear_l, ear_r = kp(row,"nose"), kp(row,"ear_l"), kp(row,"ear_r")
    wl, wr = kp(row,"wrist_l"), kp(row,"wrist_r")
    sy = (sh_l[1]+sh_r[1])/2
    ear_mid = ((ear_l[0]+ear_r[0])/2, (ear_l[1]+ear_r[1])/2)
    # 最靠近鼻子的手腕（与 C++ 一致）
    w = wl if d(wl,nose) <= d(wr,nose) else wr
    wrist_y_rel = (w[1]-sy)/sw
    head_drop = (ear_mid[1]-sy)/sw - bl["ear_mid_offset_y"]
    ear_tilt = abs(((row["ear_angle_deg"] - row["shoulder_angle_deg"]) -
                    (bl["ear_angle_deg"] - bl["shoulder_angle_deg"]) + 90) % 180 - 90)
    size = row["width_ratio_raw"]/bl["width_ratio"]
    wrist_rise = bl["wrist_y_rel"] - wrist_y_rel
    wrist_near = min(row["wrist_to_nose_raw"], 3.0)
    wrist_face = min(d(w, ear_mid)/sw, 3.0)          # ★ 生产还没用的第 7 维
    below_ear = (w[1]-ear_mid[1])/sw                 # ★ wrist 在耳中点下方多少
    return dict(sw=sw, head_drop=head_drop, ear_tilt=ear_tilt, size=size,
                wrist_rise=wrist_rise, wrist_near=wrist_near,
                wrist_face=wrist_face, below_ear=below_ear,
                wrist_y_rel=wrist_y_rel, ear_mid_y=ear_mid[1])

def classify(f, rules):
    """复刻生产 postures.txt 的级联（无迟滞）。"""
    for r in rules:
        if "guard_ear" in r and f["ear_tilt"] >= r["guard_ear"]: continue
        over = (f[r["feat"]] - r["thr"])*r["dir"]
        if over > 0: return r["label"], r
    return "normal", None

def main():
    path, blpath = sys.argv[1], sys.argv[2]
    bl = json.load(open(blpath))
    rows = [json.loads(l) for l in open(path)]
    rows = [r for r in rows if r.get("kp")]
    print(f"可用帧 {len(rows)}  跨度 {rows[-1]['t']-rows[0]['t']:.0f}s "
          f"({len(rows)/max(1e-9,rows[-1]['t']-rows[0]['t']):.1f} Hz)")
    print(f"baseline: wrist_y_rel={bl['wrist_y_rel']} ear_mid_offset_y={bl['ear_mid_offset_y']} "
          f"width_ratio={bl['width_ratio']} ear_angle={bl['ear_angle_deg']} "
          f"shoulder_angle={bl['shoulder_angle_deg']}")
    rules = [dict(label="head_tilt", feat="ear_tilt", dir=1, thr=13.35),
             dict(label="head_down", feat="head_drop", dir=1, thr=0.1035, guard_ear=11.375),
             dict(label="too_close", feat="size", dir=1, thr=1.3084),
             dict(label="hand_face", feat="wrist_rise", dir=1, thr=0.286)]
    feats = [features(r, bl) for r in rows]
    for r, f in zip(rows, feats):
        lab, _ = classify(f, rules)
        r["replay"] = lab; r["f"] = f
    # ---- 校验：回放结论是否与板端日志一致 ----
    agree = 0; tot = 0
    for r in rows:
        logged = (r.get("profile_rule") or "")
        if "wrist_rise >=" in logged and "释放阈" not in logged:
            tot += 1; agree += (r["replay"] == "hand_face")
        elif "ear_tilt >=" in logged and "释放阈" not in logged:
            tot += 1; agree += (r["replay"] == "head_tilt")
        elif "head_drop >=" in logged and "释放阈" not in logged:
            tot += 1; agree += (r["replay"] == "head_down")
    print(f"\n[校验] 回放 vs 板端日志（只比进入侧、非释放阈）: {agree}/{tot} 一致 "
          f"= {100*agree/max(1,tot):.1f}%")
    json.dump([{k:(v if k!="f" else v) for k,v in r.items()} for r in rows],
              open(sys.argv[3],"w"), ensure_ascii=False)
    # ---- 各特征分布 ----
    print(f"\n{'特征':12s} {'min':>8s} {'P10':>8s} {'中位':>8s} {'P90':>8s} {'max':>8s} {'MAD':>8s}")
    def P(xs,p):
        xs=sorted(xs); k=(len(xs)-1)*p; i=int(k)
        return xs[i] if i+1>=len(xs) else xs[i]+(xs[i+1]-xs[i])*(k-i)
    for k in ("head_drop","ear_tilt","size","wrist_rise","wrist_near","wrist_face","below_ear"):
        xs=[f[k] for f in feats]
        med=st.median(xs)
        print(f"{k:12s} {min(xs):8.3f} {P(xs,.1):8.3f} {med:8.3f} {P(xs,.9):8.3f} {max(xs):8.3f} "
              f"{st.median([abs(x-med) for x in xs]):8.3f}")

if __name__ == "__main__":
    main()
