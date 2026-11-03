#!/usr/bin/env python3
"""用生产 journal 的真实特征值，量化「迟滞能压掉多少抖动」。

为什么用 journal 而不是 114 帧采集样本：
  采集样本是「单姿势连续段」，段内特征几乎不动，本身不产生抖动。
  现场抖动只出现在**姿势过渡帧**上，只有 journal 里那份逐帧
  head_drop/ear_tilt/size/wrist_rise 序列能复现。

方法（不做整段回放 —— journal 只记录了发生切换的帧，不是连续帧，
把它们当连续帧回放是错的）：对每一条「正在离开标签 L」的切换记录，
取出离开瞬间的特征值 f，判断迟滞会不会把这次离开拦住：

  无迟滞：  f < thr            → 离开
  有迟滞：  f < thr - band     → 才离开；band <= f < thr 时留在 L

于是「被迟滞拦下的次数」= 落在 [thr - band, thr) 里的切换数，
直接就是抖动被压掉的次数。

另外单独统计「3 秒内 A->B->A 的来回跳」作为现场抖动的直接证据。
"""
import re
import sys
from collections import Counter

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/journal_switches.txt"

# 板端 postures.txt v2 的真实阈值
RULES = {
    "head_tilt": (1, 13.35, None, 0.0),
    "head_down": (0, 0.1035, 1, 11.375),
    "too_close": (3, 1.3084, None, 0.0),
    "hand_face": (4, 0.2860, None, 0.0),
}
FEATURE_NAME = ["head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise",
                "wrist_near"]
FLOORS = [0.05, 3.0, 2.0, 0.04, 0.10, 0.15]
RELEASE_SCALE = [0.08, 2.0, 2.0, 0.10, 0.10, 0.15]
NORMAL_MEDIAN = [-0.0330, 3.9570, 7.5570, 1.0562, -0.0440, 3.0000]

FWD = re.compile(
    r"(\d{2}):(\d{2}):(\d{2}).*?切换：(\S+) -> (\S+).*?"
    r"head_drop=([-\d.e]+)\s+ear_tilt=([-\d.e]+)\s+size=([-\d.e]+)\s+"
    r"wrist_rise=([-\d.e]+)\s+wrist_near=([-\d.e]+)")


def band(label, ratio):
    if ratio <= 0 or label not in RULES:
        return 0.0
    k = RULES[label][0]
    b = max(RELEASE_SCALE[k], FLOORS[k]) * ratio
    span = abs(RULES[label][1] - NORMAL_MEDIAN[k])
    return min(b, span) if span > 1e-9 else b


def parse(path):
    out = []
    for line in open(path, errors="replace"):
        m = FWD.search(line)
        if not m:
            continue
        h, mi, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
        out.append({
            "t": h * 3600 + mi * 60 + s,
            "frm": m.group(4), "to": m.group(5),
            "v": {"head_drop": float(m.group(6)), "ear_tilt": float(m.group(7)),
                  "size": float(m.group(8)), "wrist_rise": float(m.group(9)),
                  "wrist_near": float(m.group(10))},
        })
    return out


def main():
    ev = parse(LOG)
    if not ev:
        print(f"没从 {LOG} 解析到切换记录", file=sys.stderr)
        return 1
    span = ev[-1]["t"] - ev[0]["t"]
    print(f"{LOG}")
    print(f"{len(ev)} 次切换，跨 {span/3600:.1f} 小时 → "
          f"平均 {len(ev)/(span/3600):.1f} 次/小时\n")

    # ---- 现场抖动直接证据：A->B->A 在 3 秒内来回 ----
    rapid = 0
    for i in range(2, len(ev)):
        a, b, c = ev[i - 2], ev[i - 1], ev[i]
        if a["frm"] == c["to"] and a["frm"] != b["to"] and c["t"] - a["t"] <= 3:
            rapid += 1
    print(f"3 秒内 A->B->A 来回跳：{rapid} 次 "
          f"（占全部切换的 {rapid/max(1,len(ev)):.0%}）")

    # ---- 每个「离开 L」的切换，迟滞会不会拦下 ----
    zh2key = {"正常": "normal", "低头/趴桌": "head_down", "歪头": "head_tilt",
              "离屏太近": "too_close", "托腮": "hand_face"}
    print(f"\n{'离开的标签':<12}{'切换次数':>9}{'迟滞可拦下':>11}{'拦下比例':>10}")
    total_left = 0
    suppressed = 0
    for zh, label in zh2key.items():
        leaves = [e for e in ev if e["frm"] == zh]
        if not leaves:
            continue
        if label == "normal":
            # normal 没有规则，离开 normal = 某条规则命中；迟滞只放宽退出、
            # 不放宽进入，所以这里不可能被拦下。
            print(f"{zh:<12}{len(leaves):>9}{'—':>11}{'—':>10}")
            continue
        k, thr, gf, gm = RULES[label]
        fname = FEATURE_NAME[k]
        b = band(label, 0.5)
        n_sup = sum(1 for e in leaves
                    if thr - b <= e["v"][fname] < thr and b > 0)
        total_left += len(leaves)
        suppressed += n_sup
        print(f"{zh:<12}{len(leaves):>9}{n_sup:>11}{n_sup/len(leaves):>10.0%}")

    print(f"\n合计：迟滞比例 0.5 时，可拦下 {suppressed}/{total_left} 次离开"
          f"（{suppressed/max(1,total_left):.0%}）")
    print("含义：这部分切换是「特征还没真的退回去就判离开了」导致的抖动，"
          "迟滞会把它们留住，界面不再闪、语音不再重复播。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
