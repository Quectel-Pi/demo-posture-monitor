#!/usr/bin/env python3
"""逐姿势采集 —— 一次只采一个姿势，采完立刻告诉你「这一项能不能识别」

设计原则（针对过去"一次测 6 个姿势结果互相污染"的问题）：
    · 一次只采 1 个姿势，采完立即评估、立即报告
    · 每个姿势存成独立样本文件，可单独重采、单独删除
    · 采完立刻算「这个姿势和 normal 的分得开吗」，分不开就直接说分不开，
      不会让你白采下一轮
    · 全部采完之后用最近原型分类做离线交叉验证，给出混淆矩阵

用法（板端，服务保持运行）：
    # 1) 先采 normal（其它姿势都相对它比较，必须先有）
    python3 scripts/teach_posture.py --label normal

    # 2) 再逐个采要识别的姿势（可重复采，会覆盖旧样本）
    python3 scripts/teach_posture.py --label head_down
    python3 scripts/teach_posture.py --label head_tilt
    python3 scripts/teach_posture.py --label too_close
    python3 scripts/teach_posture.py --label hand_face

    # 3) 看分离度与混淆矩阵，再生成画像
    python3 scripts/teach_posture.py --report
    python3 scripts/teach_posture.py --build      # 生成 postures.txt 并重启判定

参数：
    --seconds 12      采集时长（默认 12s，够稳）
    --sample-dir DIR  样本目录（默认 /var/lib/posture-guard/posture-samples）
    --report          只分析，不采集
    --build           由已采样本生成画像文件
    --delete LABEL    删除某个姿势的样本
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import statistics
import sys
import time

SOCKET = "/run/posture-guard/status.sock"
SAMPLE_DIR = "/var/lib/posture-guard/posture-samples"
PROFILE_FILE = "/var/lib/posture-guard/postures.txt"

NF = 6
FEATURES = ["head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise",
            "wrist_near"]
FEATURE_ZH = {
    "head_drop": "头低量", "ear_tilt": "耳线歪", "shoulder_tilt": "肩线歪",
    "size": "靠近倍数", "wrist_rise": "抬手量", "wrist_near": "手离脸",
}
# 每个特征「低于这个变化量就没意义」的下限（与 C++ DefaultFloors 一致）
FLOORS = [0.05, 3.0, 2.0, 0.04, 0.10, 0.15]
# 每个姿势的「定义性特征」候选（数据会再筛一遍，只保留真的动了的）
# 顺序对应 FEATURES: head_drop, ear_tilt, shoulder_tilt, size, wrist_rise
LABELS_ORDER = ["normal", "head_down", "head_tilt", "too_close", "hand_face"]
LABEL_ZH = {"normal": "正常", "head_down": "低头/趴桌", "head_tilt": "歪头",
            "too_close": "离屏太近", "hand_face": "托腮", "away": "无人"}

#   head_drop, ear_tilt, shoulder_tilt, size, wrist_rise, wrist_near
CANDS = {
    "normal":     [True, True, True, True, True, True],
    "head_down":  [True, False, False, False, False, False],
    "head_tilt":  [False, True, False, False, False, False],
    "too_close":  [False, False, False, True, False, False],
    # 托腮要两个都动才算：手抬起来 + 手靠近脸（否则前倾时手跟着抬会误判）
    "hand_face":  [False, False, False, False, True, True],
}


def median(v):
    return statistics.median(v) if v else 0.0


def mad(vals, med):
    return median([abs(v - med) for v in vals]) if vals else 0.0


# 判定优先级：数字越小越先试。
# 顺序不是随便定的，是被数据逼出来的：
#   · 歪头必须在低头之前 —— 歪头会让一只耳朵下沉，head_drop 也涨到 0.18，
#     先判低头就会把头歪吃掉
#   · 凑近必须在托腮之前 —— 身体前倾时手一定跟着靠近脸（实测 wrist_near
#     从 3.0 掉到 0.56，和托腮的 0.41 几乎一样），先判托腮就会把凑近吃掉
PRIORITY = {
    "head_tilt": 1,
    "head_down": 2,
    "too_close": 3,
    "hand_face": 4,
    "away": 5,
}
# 每个姿势「可以拿来判定」的特征候选（顺序对应 FEATURES）
#   head_drop, ear_tilt, shoulder_tilt, size, wrist_rise, wrist_near
CAND_FEATURES = {
    "head_down":  ["head_drop"],
    "head_tilt":  ["ear_tilt"],
    "too_close":  ["size"],
    "hand_face":  ["wrist_near", "wrist_rise"],
}


def build_rules(samples: dict) -> list[dict]:
    """由样本自动算出级联规则。

    每个姿势只用「一个」特征 + 一个阈值：
      · 特征：候选里 gap / 该特征有意义尺度 最大的那个
      · 阈值：(normal 中位数 + 该类中位数) / 2
    这样阈值不是手调的，换机位/换人重采一遍就自动跟着变。
    """
    if "normal" not in samples:
        return []
    nm = [median([v[i] for v in samples["normal"]]) for i in range(NF)]
    rules = []
    for label, feats in samples.items():
        if label == "normal":
            continue
        cands = CAND_FEATURES.get(label)
        if not cands:
            continue
        cur = [median([v[i] for v in feats]) for i in range(NF)]
        best = None
        for fname in cands:
            k = FEATURES.index(fname)
            gap = abs(cur[k] - nm[k])
            if gap < FLOORS[k] * 2.0:          # 差值太小，没有实际意义
                continue
            score = gap / FLOORS[k]
            if best is None or score > best[0]:
                best = (score, k, gap)
        if best is None:
            print(f"   !! {label}: 没有可用特征（和 normal 差得太少），不生成规则")
            continue
        _score, k, gap = best
        direction = 1 if cur[k] >= nm[k] else -1
        thr = best_threshold(samples, label, k, direction)
        # 互斥门控：歪头会压低一侧耳朵 → head_drop 跟着涨（实测 0.145），
        # 会把「歪头」抢成「低头」。耳线已经明显歪了就不许再判低头。
        guard = None
        if label == "head_down" and "head_tilt" in samples:
            k_ear = FEATURES.index("ear_tilt")
            ht_ear = median([v[k_ear] for v in samples["head_tilt"]])
            guard = ("ear_tilt", (nm[k_ear] + ht_ear) / 2.0)
        rules.append({
            "priority": PRIORITY.get(label, 50),
            "label": label,
            "feature": FEATURES[k],
            "feature_index": k,
            "direction": direction,
            "threshold": thr,
            "normal_median": nm[k],
            "class_median": cur[k],
            "gap": gap,
            "guard": guard,
        })
    rules.sort(key=lambda r: r["priority"])
    return rules


def best_threshold(samples: dict, label: str, k: int, direction: int) -> float:
    """在该特征上搜索最优分割阈值（最大化平衡准确率）。

    为什么不用中点：托腮时下巴压手，head_drop 会被动升到 0.17，
    而低头是 0.32 —— 中点 0.158 会把托腮吃掉（实测 4/9 帧判错）。
    搜出来的阈值会自己躲开这种重叠。

    只在「normal 中位数」和「该类中位数」之间搜，不许跑出这个区间：
    否则样本少的时候会过拟合到某个离群帧上。
    """
    pos = [v[k] for v in samples.get(label, [])]
    neg = [v[k] for lb, vs in samples.items() if lb != label for v in vs]
    if not pos or not neg:
        return 0.0
    lo = min(median([v[k] for v in samples["normal"]]), median(pos))
    hi = max(median([v[k] for v in samples["normal"]]), median(pos))
    cands = sorted({(a + b) / 2.0 for a in pos for b in neg})
    cands = [c for c in cands if lo <= c <= hi] or [(lo + hi) / 2.0]

    best, best_score = None, -1.0
    for c in cands:
        if direction > 0:
            tpr = sum(1 for x in pos if x >= c) / len(pos)
            tnr = sum(1 for x in neg if x < c) / len(neg)
        else:
            tpr = sum(1 for x in pos if x <= c) / len(pos)
            tnr = sum(1 for x in neg if x > c) / len(neg)
        score = tpr + tnr
        # 同分时优先取离两侧样本都更远（更安全）的阈值
        margin = min(min(abs(x - c) for x in pos), min(abs(x - c) for x in neg))
        key = (round(score, 6), margin)
        if best is None or key > best[0]:
            best = (key, c)
    return best[1] if best else (lo + hi) / 2.0


def classify(v, rules, samples):
    """与 C++ PostureProfile::Classify 完全一致：按优先级级联，第一个命中的就是结论。"""
    for r in rules:
        g = r.get("guard")
        if g and v[FEATURES.index(g[0])] >= g[1]:
            continue
        k = r["feature_index"]
        over = (v[k] - r["threshold"]) * r["direction"]
        if over > 0:
            return r["label"], over / FLOORS[k], r
    if "normal" in samples:
        return "normal", 0.0, None
    return None, 0.0, None


def recompute(sample_dir: str) -> int:
    """用已存的 raw_metrics 重算全部特征（改特征定义后不必让用户重采）。"""
    if not os.path.isdir(sample_dir):
        print(f"!! 没有样本目录 {sample_dir}", file=sys.stderr)
        return 1
    total = 0
    for fn in sorted(os.listdir(sample_dir)):
        if not fn.endswith(".json"):
            continue
        path = os.path.join(sample_dir, fn)
        with open(path) as f:
            data = json.load(f)
        raw = data.get("raw_metrics") or []
        baseline = data.get("baseline") or {}
        if not raw:
            print(f"   {fn[:-5]}: 没有 raw_metrics 快照，需重采")
            continue
        feats = []
        for r in raw:
            v = features_of(dict(r, valid=True), baseline)
            if v is not None:
                feats.append(v)
        data["features"] = feats
        data["count"] = len(feats)
        data["recomputed_at"] = time.strftime("%F %T")
        with open(path, "w") as f:
            json.dump(data, f, indent=1)
        print(f"   {fn[:-5]}: {len(feats)}/{len(raw)} 帧")
        total += len(feats)
    print(f"重算完成，共 {total} 帧")
    return 0


def rebaseline(sample_dir: str) -> int:
    """把全部样本「重锚」到当前校准基准上，并用 raw_metrics 重算特征。

    为什么需要：6 个特征里 5 个是「相对基准」的量。基准一旦重建（换人、
    挪机位、自动重校准），旧画像的特征就整体偏移了 —— 表现为「明明坐得对，
    却离正常原型偏 15σ」。样本里存了绝对原始指标，所以可以直接换算，
    不必让用户重采一遍。
    """
    cur = get_baseline()
    if not cur.get("valid"):
        print("!! 当前没有有效基准，无法重锚", file=sys.stderr)
        return 1
    if not os.path.isdir(sample_dir):
        print(f"!! 没有样本目录 {sample_dir}", file=sys.stderr)
        return 1
    total = 0
    for fn in sorted(os.listdir(sample_dir)):
        if not fn.endswith(".json"):
            continue
        path = os.path.join(sample_dir, fn)
        with open(path) as f:
            data = json.load(f)
        raw = data.get("raw_metrics") or []
        if not raw:
            print(f"   {fn[:-5]}: 没有 raw_metrics 快照，需重采")
            continue
        feats = []
        for r in raw:
            v = features_of(dict(r, valid=True), cur)
            if v is not None:
                feats.append(v)
        data["baseline"] = cur
        data["features"] = feats
        data["count"] = len(feats)
        data["rebased_at"] = time.strftime("%F %T")
        with open(path, "w") as f:
            json.dump(data, f, indent=1)
        print(f"   {fn[:-5]}: {len(feats)} 帧")
        total += len(feats)
    print(f"重锚完成：{total} 帧已对齐到当前基准")
    return 0


def load_samples(sample_dir: str) -> dict[str, list[list[float]]]:
    out: dict[str, list[list[float]]] = {}
    if not os.path.isdir(sample_dir):
        return out
    for fn in sorted(os.listdir(sample_dir)):
        if not fn.endswith(".json"):
            continue
        label = fn[:-5]
        try:
            with open(os.path.join(sample_dir, fn)) as f:
                data = json.load(f)
            feats = data.get("features") or []
            if feats:
                out[label] = feats
        except (OSError, ValueError):
            continue
    return out


def get_baseline() -> dict:
    for p in ("/var/lib/posture-guard/baseline.json",):
        try:
            with open(p) as f:
                return json.load(f)
        except (OSError, ValueError):
            continue
    return {}


# ---------------------------------------------------------------- 读状态
def read_status(timeout: float = 3.0):
    """从 status.sock 读一帧 status JSON（行分隔）。失败返回 None。"""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sk:
            sk.settimeout(timeout)
            sk.connect(SOCKET)
            buf = b""
            deadline = time.time() + timeout
            while time.time() < deadline:
                chunk = sk.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        continue
                    if obj.get("type") == "status":
                        return obj
    except (OSError, socket.timeout):
        return None
    return None


def read_frames(seconds: float) -> list[dict]:
    """在 seconds 秒内持续读状态，返回所有 metrics 有效的帧。"""
    out: list[dict] = []
    end = time.time() + seconds
    while time.time() < end:
        st = read_status()
        if st is None:
            time.sleep(0.15)
            continue
        # 状态字段是扁平的（不是嵌在 metrics 下）
        if not st.get("metrics_valid"):
            out.append({"valid": False,
                        "explanation": st.get("explanation")
                        or st.get("reject_reason") or "指标无效"})
            continue
        out.append({
            "valid": True,
            "ear_mid_offset_y": st.get("ear_mid_offset_y"),
            "head_offset_y": st.get("head_offset_y"),
            "ear_angle_deg": st.get("ear_angle_deg"),
            "shoulder_angle_deg": st.get("shoulder_angle_deg"),
            "width_ratio": st.get("width_ratio"),
            "wrist_y_rel": st.get("wrist_y_rel"),
            "wrist_to_nose_raw": st.get("wrist_to_nose"),
            "ear_mid_valid": bool(st.get("ear_mid_valid")),
            "ear_angle_valid": bool(st.get("ear_angle_valid")),
            "wrist_valid": bool(st.get("wrist_valid")),
        })
        time.sleep(0.05)
    return out


def features_of(frame: dict, baseline: dict):
    """把一帧原始指标换算成 6 维特征。

    ★ 必须与 C++ BuildFeatureVector 逐行等价 ★
    踩过：这里曾经把 head_drop 额外除以肩宽占比，而 ear_mid_offset_y /
    head_offset_y 本来就是按肩宽归一化的 —— 训练出的阈值量纲和运行期差 2 倍，
    画像是「训练时 100%、上线全不对」这种最难查的错。
    改这里必须同步改 src/PostureProfile.cpp:BuildFeatureVector()。
    """
    if not frame.get("valid") or not baseline.get("valid"):
        return None

    # head_drop：头比基准低多少（正 = 更低）。单位已经是肩宽，不要再除。
    if frame.get("ear_mid_valid") and baseline.get("ear_mid_valid"):
        ear_off, base_off = frame.get("ear_mid_offset_y"), baseline.get("ear_mid_offset_y")
    else:
        ear_off, base_off = frame.get("head_offset_y"), baseline.get("head_offset_y")
    if ear_off is None or base_off is None:
        return None
    head_drop = float(ear_off) - float(base_off)

    # 歪头：耳线「相对肩线」的夹角差（与 C++ 一致）。
    # 用绝对耳线角会跨会话漂 8°，实测同一人正常坐两次校准差 16°，规则常亮。
    if frame.get("ear_angle_valid") and baseline.get("ear_angle_valid"):
        rel = (float(frame.get("ear_angle_deg", 0.0))
               - float(frame.get("shoulder_angle_deg", 0.0)))
        base_rel = (float(baseline.get("ear_angle_deg", 0.0))
                    - float(baseline.get("shoulder_angle_deg", 0.0)))
        ear_tilt = abs(rel - base_rel)
    else:
        ear_tilt = 0.0

    shoulder_tilt = abs(float(frame.get("shoulder_angle_deg", 0.0))
                        - float(baseline.get("shoulder_angle_deg", 0.0)))

    bwr = float(baseline.get("width_ratio") or 0.0)
    size = (float(frame.get("width_ratio") or 0.0) / bwr) if bwr > 0.01 else 1.0

    if frame.get("wrist_valid") and baseline.get("wrist_valid"):
        wrist_rise = (float(baseline.get("wrist_y_rel", 0.0))
                      - float(frame.get("wrist_y_rel", 0.0)))
    else:
        wrist_rise = 0.0

    wrist_near = frame.get("wrist_to_nose_raw", 3.0)
    if not isinstance(wrist_near, (int, float)) or wrist_near > 3.0:
        wrist_near = 3.0

    return [head_drop, ear_tilt, shoulder_tilt, size, wrist_rise, float(wrist_near)]


# ---------------------------------------------------------------- 采集
def collect(label: str, seconds: float, sample_dir: str, settle: float = 6.0) -> int:
    baseline = get_baseline()
    if not baseline.get("valid"):
        print("!! 还没有校准基准。请先坐正保持 5 秒让程序自动校准，再采样本。",
              file=sys.stderr)
        return 1

    print(f"== 采集「{LABEL_ZH.get(label, label)}」 {seconds:.0f} 秒 ==")
    if label == "normal":
        print("   要领：坐直、双手放桌面、别歪头别低头。")
        print("   ★ 但要「像平时一样自然」：轻微转头看屏幕不同位置、手在键盘上小动。")
        print("     全程僵住不动反而不好 —— 转头会改变耳线投影角，僵着采到的正态")
        print("     范围太窄，实际用起来一点点转头就被判成歪头（实测踩过）。")
    elif label == "head_down":
        print("   要领：下巴朝胸口明显低头（像看键盘/手机），保持不动")
    elif label == "head_tilt":
        print("   要领：头向一侧明显歪 20~30°（耳朵靠向同侧肩膀），保持不动")
    elif label == "too_close":
        print("   要领：身体前倾、脸向屏幕靠近（别真贴上），保持不动")
    elif label == "hand_face":
        print("   要领：单手托住下巴/脸颊，肘撑桌面，保持不动")
    print()
    for i in range(5, 0, -1):
        print(f"   请摆好姿势… {i}   ", end="\r", flush=True)
        time.sleep(1)
    # ★ 稳定期：先丢弃前 settle 秒。
    #   踩过：14 秒采集里前 14 帧还是上一个姿势（换姿势需要时间），
    #   中位数被污染，结论变成「区分不开」。
    if settle > 0:
        print(f"   稳定期 {settle:.0f} 秒（换姿势用，不计入样本）…     ")
        read_frames(settle)
    print(f"   采集中（{seconds:.0f} 秒），保持别动…          ")

    frames = read_frames(seconds)
    feats = []
    skipped = []
    for f in frames:
        v = features_of(f, baseline)
        if v is None:
            skipped.append(f.get("explanation") or "指标无效")
            continue
        feats.append(v)

    # ---- 轻量剔离群：只去掉极端帧，保留自然变化 ----
    # 踩过两个反向的坑：
    #   ① 完全不剔：换姿势时的残留帧会污染中位数
    #   ② 剔太狠（2.5×MAD 迭代 3 次）：把「自然打字、轻微转头」也当噪声删了，
    #      27 帧只剩 9 帧，正态范围被压窄，实际用起来一动就误判
    # 所以：单次、4×MAD、且最多只能删 25%，否则整批不动。
    if len(feats) >= 10:
        meds = [median([v[i] for v in feats]) for i in range(NF)]
        mads = [max(mad([v[i] for v in feats], meds[i]), FLOORS[i]) for i in range(NF)]
        kept = [v for v in feats
                if all(abs(v[i] - meds[i]) <= 4.0 * mads[i] for i in range(NF))]
        if len(kept) >= len(feats) * 0.75 and len(kept) >= 8:
            if len(kept) < len(feats):
                print(f"   剔除 {len(feats) - len(kept)} 个极端帧，保留 {len(kept)} 个")
            feats = kept

    if len(feats) < 5:
        print(f"\n!! 只拿到 {len(feats)} 个有效样本（共 {len(frames)} 帧），不够。")
        if skipped:
            from collections import Counter
            for reason, n in Counter(skipped).most_common(3):
                print(f"   {n:3d} 帧被跳过：{reason[:70]}")
        print("   先解决上面的问题（多半是取景/机位），再重采。")
        return 2

    os.makedirs(sample_dir, exist_ok=True)
    path = os.path.join(sample_dir, f"{label}.json")
    # 同时存原始指标快照：以后改特征定义（例如加第 6 个特征）可以直接重算，
    # 不需要让用户重采。
    RAW_KEYS = ("ear_mid_offset_y", "head_offset_y", "ear_angle_deg",
                "shoulder_angle_deg", "width_ratio", "wrist_y_rel",
                "wrist_to_nose_raw", "ear_mid_valid", "ear_angle_valid",
                "wrist_valid")
    raw = [{k: fr.get(k) for k in RAW_KEYS}
           for fr in frames if features_of(fr, baseline) is not None]
    with open(path, "w") as f:
        json.dump({"label": label, "count": len(feats), "features": feats,
                   "raw_metrics": raw,
                   "baseline": baseline, "collected_at": time.strftime("%F %T")},
                  f, indent=1)

    print(f"\n   有效样本 {len(feats)}/{len(frames)} 帧，已存 {path}")
    print("   各特征中位数：")
    for i, name in enumerate(FEATURES):
        vals = [v[i] for v in feats]
        print(f"     {name:15s}({FEATURE_ZH[name]})  中位 {median(vals):+8.3f}   "
              f"范围 [{min(vals):+7.3f}, {max(vals):+7.3f}]")

    # ---- 立刻评估：这一项和 normal 分得开吗 ----
    if label != "normal":
        samples = load_samples(sample_dir)
        if "normal" not in samples:
            print("\n   （还没有 normal 样本，先采 normal 才能判断能不能分开）")
        else:
            print(f"\n   == 和「正常」的可分性 ==")
            ok = True
            for i, name in enumerate(FEATURES):
                a = [v[i] for v in samples["normal"]]
                b = [v[i] for v in feats]
                ma, mb = median(a), median(b)
                gap = abs(mb - ma)
                within = max(1e-9, (mad(a, ma) + mad(b, mb)) / 2.0)
                ratio = gap / within
                mark = "✅" if ratio >= 2.0 else ("⚠️" if ratio >= 1.0 else "  ")
                print(f"     {mark} {name:15s} normal {ma:+7.3f}  vs  {label} {mb:+7.3f}"
                      f"   差 {gap:6.3f}  分离比 {ratio:5.2f}")
            # 只要有一个特征分离比 >= 2，这一项就可判
            # 选「主力特征」：比值要够大，同时绝对差也要够大 ——
            # 光看比值会被 MAD 极小的特征骗（例如 size 差 0.008 也能算出比值 60）
            best = 0.0
            best_name = ""
            for i, name in enumerate(FEATURES):
                a = [v[i] for v in samples["normal"]]
                b = [v[i] for v in feats]
                gap = abs(median(b) - median(a))
                within = max(1e-9, (mad(a, median(a)) + mad(b, median(b))) / 2.0)
                ratio = gap / within
                # 绝对差必须超过该特征下限的 2 倍，否则没有实际意义
                if gap < FLOORS[i] * 2.0:
                    continue
                if ratio > best:
                    best = ratio
                    best_name = name
            if best == 0.0:
                best_name = FEATURES[0]
            if best >= 2.0:
                print(f"\n   ✅ 可判：靠「{FEATURE_ZH[best_name]}」(分离比 {best:.2f}) 能把这个姿势"
                      f"和正常分开")
            else:
                print(f"\n   ❌ 分不开：最好的特征「{FEATURE_ZH[best_name]}」分离比只有 {best:.2f}"
                      f"（需要 ≥2）")
                print("      这说明摄像头在这个机位下拿不到能区分该姿势的关键点。")
                print("      不是阈值问题，调阈值只会让它随机误报。")
                ok = False
            return 0 if ok else 3
    return 0


# ---------------------------------------------------------------- 报告
def report(sample_dir: str) -> int:
    samples = load_samples(sample_dir)
    if not samples:
        print(f"没有任何样本（目录 {sample_dir}）")
        return 1
    print("== 已采样本 ==")
    for label in LABELS_ORDER + [l for l in samples if l not in LABELS_ORDER]:
        if label in samples:
            print(f"   {label:12s} {LABEL_ZH.get(label, '?'):8s} {len(samples[label]):4d} 帧"
                  f"   特征中位 " +
                  " ".join(f"{median([v[i] for v in samples[label]]):+.2f}"
                           for i in range(NF)))

    if len(samples) < 2:
        print("\n只采了一个姿势，无法交叉验证。")
        return 1

    # 类原型
    proto = {}
    for label, feats in samples.items():
        med = [median([v[i] for v in feats]) for i in range(NF)]
        sd = [mad([v[i] for v in feats], med[i]) for i in range(NF)]
        proto[label] = (med, sd, len(feats))
    labels = list(proto.keys())

    # 跨类尺度
    scales = []
    for i in range(NF):
        s = median([proto[l][1][i] for l in labels])
        scales.append(max(s, FLOORS[i]))

    print("\n== 特征分离度（中位差 / 类内离散，≥2 才能可靠区分）==")
    print(f"   {'特征':16s}" + "".join(f"{l[:9]:>11s}" for l in labels))
    weak = []
    for i, name in enumerate(FEATURES):
        row = f"   {name:16s}"
        for l in labels:
            row += f"{proto[l][0][i]:+11.2f}"
        # 算该特征的最大两两分离比
        best = 0.0
        for a in range(len(labels)):
            for b in range(a + 1, len(labels)):
                gap = abs(proto[labels[a]][0][i] - proto[labels[b]][0][i])
                within = max(1e-9, (proto[labels[a]][1][i] + proto[labels[b]][1][i]) / 2.0)
                best = max(best, gap / within)
        mark = "✅" if best >= 2.0 else ("⚠️" if best >= 1.0 else "❌")
        print(row + f"   最佳分离比 {best:5.2f} {mark}")
        if best < 2.0:
            weak.append(name)

    # 留一交叉验证（最近原型）
    rules = build_rules(samples)
    print("\n== 判定规则（按优先级，数据自动算出）==")
    if not rules:
        print("   !! 只有 normal，没有其它姿势的样本，生成不了规则")
    for r in rules:
        zh = FEATURE_ZH.get(r["feature"], r["feature"])
        g = r.get("guard")
        extra = f"   [门控 {g[0]} < {g[1]:.2f}]" if g else ""
        print("   %d. %-12s %s %s %.4f   （normal %.4f → %s %.4f，差 %.4f）%s" % (
            r["priority"], r["label"], r["feature"],
            ">=" if r["direction"] > 0 else "<=", r["threshold"],
            r["normal_median"], r["label"], r["class_median"], r["gap"], extra))

    print("\n== 混淆矩阵（级联判定 + 优先级）==")
    head = "真\\判".ljust(12)
    print("   " + head + "".join(f"{l[:8]:>10s}" for l in labels))
    correct = 0
    for true_label in labels:
        row = f"   {true_label:12s}"
        for pred_label in labels:
            hit = 0
            for v in samples[true_label]:
                bl, _bd, _br = classify(v, rules, samples)
                if bl == pred_label:
                    hit += 1
            row += f"{hit:10d}"
            if pred_label == true_label:
                correct += hit
        print(row)
    n_all = sum(len(samples[l]) for l in labels)
    print(f"\n   总体准确率 {correct}/{n_all} = {correct / max(1, n_all) * 100:.1f}%")
    if weak:
        print(f"\n   ⚠️ 这些特征区分不开已采的姿势：{', '.join(weak)}")
        print("      如果准确率低，多半是摄像头机位拿不到相应关键点，"
              "不是分类算法的问题。")
    return 0


# ---------------------------------------------------------------- 生成画像
def build(sample_dir: str, profile_file: str) -> int:
    samples = load_samples(sample_dir)
    if "normal" not in samples:
        print("!! 必须先采 normal（其它姿势都是相对它算的）", file=sys.stderr)
        return 1
    if len(samples) < 2:
        print("!! 至少要有 normal + 1 个姿势", file=sys.stderr)
        return 1

    proto = {}
    for label, feats in samples.items():
        med = [median([v[i] for v in feats]) for i in range(NF)]
        sd = [mad([v[i] for v in feats], med[i]) for i in range(NF)]
        proto[label] = (med, sd, len(feats))
    # 保持固定顺序（normal 在最前，便于阅读）
    labels = [l for l in LABELS_ORDER if l in proto] + \
             [l for l in proto if l not in LABELS_ORDER]
    scales = []
    for i in range(NF):
        s = median([proto[l][1][i] for l in labels])
        scales.append(max(s, FLOORS[i]))

    rules = build_rules(samples)
    if not rules:
        print("!! 没有可用的判定规则（先采 normal + 至少一个异常姿势）",
              file=sys.stderr)
        return 2
    lines = ["# posture-profile v2 —— 逐姿势校准画像（有序级联判定）",
             "# rule 行：判定用。按 priority 从小到大依次试，第一个命中的就是结论。",
             "#   rule <优先级> <标签> <特征> <方向> <阈值> <normal中位> <该类中位>",
             "#   方向 + 表示特征 >= 阈值命中，- 表示特征 <= 阈值命中",
             "#   阈值由采集数据搜索得出（最大化平衡准确率），不是手调的",
             "# class 行：原始统计（诊断用，不参与判定）",
             "#   class <标签> <样本数> <特征中位数> <特征MAD>",
             "# 特征顺序：" + " ".join(FEATURES),
             "scale" + "".join(f"\t{x:.4f}" for x in scales)]
    for l in labels:
        med, sd, n = proto[l]
        lines.append(f"class\t{l}\t{n}" + "".join(f"\t{x:.4f}" for x in med)
                     + "".join(f"\t{x:.4f}" for x in sd))
    for r in rules:
        row = "rule\t%d\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f" % (
            r["priority"], r["label"], r["feature"],
            "-" if r["direction"] < 0 else "+",
            r["threshold"], r["normal_median"], r["class_median"])
        if r.get("guard"):
            row += "\t%s\t%.4f" % (r["guard"][0], r["guard"][1])
        lines.append(row)
    os.makedirs(os.path.dirname(profile_file), exist_ok=True)
    with open(profile_file, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"已生成画像 {profile_file}（{len(labels)} 个姿势，"
          f"{sum(proto[l][2] for l in labels)} 个样本）")
    print("\n生效方式：")
    print("  systemctl restart posture-guard")
    print("生效后 status 里 profile_active=True，判定改由画像分类给出。")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="逐姿势采集与画像生成")
    ap.add_argument("--label", help="要采的姿势：normal/head_down/head_tilt/too_close/hand_face")
    ap.add_argument("--seconds", type=float, default=14.0,
                    help="稳定期之后真正采集的时长")
    ap.add_argument("--settle", type=float, default=6.0,
                    help="倒计时后先丢弃多少秒（换姿势用）")
    ap.add_argument("--sample-dir", default=SAMPLE_DIR)
    ap.add_argument("--profile-file", default=PROFILE_FILE)
    ap.add_argument("--rebaseline", action="store_true",
                    help="把样本重锚到当前校准基准并重算特征（重新校准后一定要跑）")
    ap.add_argument("--recompute", action="store_true",
                    help="用已存 raw_metrics 重算特征（改了特征定义后用，不必重采）")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--delete")
    args = ap.parse_args()

    if not os.path.exists(SOCKET):
        print(f"!! 状态 socket 不存在: {SOCKET}", file=sys.stderr)
        return 1
    if args.delete:
        p = os.path.join(args.sample_dir, f"{args.delete}.json")
        if os.path.exists(p):
            os.remove(p)
            print(f"已删除 {p}")
        else:
            print(f"没有 {p}")
        return 0
    if args.rebaseline:
        rc = rebaseline(args.sample_dir)
        if rc != 0:
            return rc
        return 0
    if args.recompute:
        return recompute(args.sample_dir)
    if args.report:
        return report(args.sample_dir)
    if args.build:
        return build(args.sample_dir, args.profile_file)
    if not args.label:
        print("需要 --label 或 --report / --build", file=sys.stderr)
        return 1
    return collect(args.label, args.seconds, args.sample_dir, args.settle)


if __name__ == "__main__":
    sys.exit(main())
