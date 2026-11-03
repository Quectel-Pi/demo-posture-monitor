#!/usr/bin/env python3
"""用板上真实关键点复算 PostureAnalyzer 的判定，量化准确率。

几何公式严格照抄 src/PostureAnalyzer.cpp (ComputeMetrics) 与
src/PostureProfile.cpp (BuildFeatureVector)，规则照抄
/var/lib/posture-guard/postures.txt。
"""
import json, math, sys

# ---- 板端当前校准基准（/var/lib/posture-guard/baseline.json, 2026-11-03T10:08:05）----
BASELINE = {
    "valid": True, "ear_angle_valid": True, "ear_angle_deg": -3.576,
    "wrist_valid": True, "wrist_y_rel": 0.648,
    "ear_mid_valid": True, "ear_mid_offset_y": -0.559,
    "shoulder_angle_deg": -4.399, "width_ratio": 0.4092,
    "head_offset_y": -0.3821, "samples": 107,
}
# 规则文件里的 scale（决定腕高手势门限要多大）
SCALE = [0.05, 3.0, 2.0, 0.04, 0.10, 0.15, 0.15]
HAND_FACE_WRIST_RISE = 0.20      # config: posture.hand_face_wrist_rise

KP = ["nose","eye_l","eye_r","ear_l","ear_r","shoulder_l","shoulder_r",
      "elbow_l","elbow_r","wrist_l","wrist_r","hip_l","hip_r",
      "knee_l","knee_r","ankle_l","ankle_r"]


def norm_line_angle(deg):
    while deg >= 90:  deg -= 180
    while deg < -90:  deg += 180
    return deg


def dist(a, b):
    return math.hypot(a[0]-b[0], a[1]-b[1])


def metrics_of(person, W, H):
    kp = {k["name"]: (k["x"], k["y"]) for k in person["keypoints"]}
    sh_l, sh_r = kp["shoulder_l"], kp["shoulder_r"]
    nose = kp["nose"]
    sw = dist(sh_l, sh_r)
    if sw < 20:
        return None, "肩宽过小"
    sx, sy = (sh_l[0]+sh_r[0])/2, (sh_l[1]+sh_r[1])/2
    sh_ang = norm_line_angle(math.degrees(math.atan2(sh_r[1]-sh_l[1], sh_r[0]-sh_l[0])))
    m = {"sw": sw, "sx": sx, "sy": sy, "shoulder_angle_deg": sh_ang,
         "width_ratio": sw / W, "head_offset_y": (nose[1]-sy)/sw}
    # 耳中点
    def inside(p):
        return 0 <= p[0] <= W and 0 <= p[1] <= H
    # 鼻子/肩在框内检查（简化：板端还有更严格的取景门）
    ear_l, ear_r = kp["ear_l"], kp["ear_r"]
    if inside(ear_l) and inside(ear_r) and dist(ear_l, ear_r) > sw*0.35:
        m["ear_mid_offset_y"] = ((ear_l[1]+ear_r[1])/2 - sy)/sw
        m["ear_angle_deg"] = norm_line_angle(
            math.degrees(math.atan2(ear_r[1]-ear_l[1], ear_r[0]-ear_l[0])))
        m["ear_mid_valid"] = True
        m["ear_angle_valid"] = True
    else:
        m["ear_mid_valid"] = False
        m["ear_angle_valid"] = False
    # 手腕
    best, best_y, best_x = 1e9, None, None
    for w in ("wrist_l", "wrist_r"):
        p = kp[w]
        d = dist(p, nose)/sw
        if d < best:
            best, best_x, best_y = d, p[0], p[1]
    m["wrist_to_nose_raw"] = best
    m["wrist_y_rel"] = (best_y - sy)/sw
    gate = m["wrist_y_rel"] < BASELINE["wrist_y_rel"] - HAND_FACE_WRIST_RISE
    m["wrist_to_nose"] = best if gate else 99.0
    m["wrist_gate"] = gate
    # wrist_face / below_ear
    emx = (ear_l[0]+ear_r[0])/2 if m["ear_mid_valid"] else nose[0]
    emy = (ear_l[1]+ear_r[1])/2 if m["ear_mid_valid"] else nose[1]
    m["wrist_face"] = dist((best_x, best_y), (emx, emy))/sw
    m["below_ear"] = (best_y - emy)/sw
    return m, None


def features_of(m):
    b = BASELINE
    head_drop = (m["ear_mid_offset_y"] - b["ear_mid_offset_y"]) if m["ear_mid_valid"] \
                else (m["head_offset_y"] - b["head_offset_y"])
    ear_tilt = abs(math.remainder(
        (m["ear_angle_deg"] - m["shoulder_angle_deg"]) -
        (b["ear_angle_deg"] - b["shoulder_angle_deg"]), 180)) if m["ear_angle_valid"] else 0.0
    shoulder_tilt = abs(math.remainder(m["shoulder_angle_deg"] - b["shoulder_angle_deg"], 180))
    size = m["width_ratio"]/b["width_ratio"] if b["width_ratio"] > 0.01 else 1.0
    wrist_rise = b["wrist_y_rel"] - m["wrist_y_rel"]
    wrist_near = min(m["wrist_to_nose_raw"], 3.0)
    wrist_face = min(m["wrist_face"], 3.0)
    return [head_drop, ear_tilt, shoulder_tilt, size, wrist_rise, wrist_near, wrist_face]


def classify(f, m):
    """照抄 postures.txt 的 5 条级联规则（当前生效版）。"""
    hd, et, st, sz, wr, wn, wf = f
    if et >= 13.35:                      return "head_tilt"
    if hd >= 0.1035:                     return "head_down"
    if sz >= 1.20:                       return "too_close"
    if wr >= 0.286 and wn < 0.55:        return "hand_face"
    if st >= 12.0:                       return "shoulder_tilt"
    return "normal"


LABEL_MAP = {"lean_close": "too_close", "away": "away"}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "evidence/accuracy-board2-2026-11-03/kp_export.jsonl"
    rows = [json.loads(l) for l in open(path)]
    from collections import Counter, defaultdict
    conf = defaultdict(Counter)
    for r in rows:
        truth = LABEL_MAP.get(r["pose"], r["pose"])
        d = r["data"]
        if not d or not d.get("persons"):
            pred = "UNKNOWN(无检出)"
        else:
            p = max(d["persons"], key=lambda x: x["score"])
            m, why = metrics_of(p, d["width"], d["height"])
            if m is None:
                pred = "UNKNOWN(" + why + ")"
            else:
                pred = classify(features_of(m), m)
        conf[truth][pred] += 1
    # 打印混淆矩阵
    preds = sorted({p for c in conf.values() for p in c})
    print(f"{'真值':<12}", "  ".join(f"{p[:10]:>10}" for p in preds), "   召回")
    total_ok = total = 0
    for truth in ["normal","head_down","head_tilt","too_close","hand_face","away"]:
        if truth not in conf: continue
        c = conf[truth]
        n = sum(c.values()); ok = c.get(truth, 0)
        total_ok += ok; total += n
        print(f"{truth:<12}", "  ".join(f"{c.get(p,0):>10}" for p in preds),
              f"  {ok}/{n} = {100*ok/n:5.1f}%")
    print(f"\n总体准确率 {total_ok}/{total} = {100*total_ok/total:.1f}%")
    return conf


if __name__ == "__main__":
    main()
