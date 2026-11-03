#!/usr/bin/env python3
"""把板端 6 维 postures.txt 升级为 7 维 + 给托腮加「手必须在脸旁」的硬约束。

## 为什么需要这一步

两个独立缺陷叠在一起，必须同批修：

1) **维度错配**：源码特征已是 7 维（新增 wrist_face），板上画像还是 6 维。
   换二进制而不同时换画像 → wrist_face 解析成 0 → 依赖它的规则行为不可预测。

2) **托腮规则只有半条**：生效画像里
       rule 4 hand_face wrist_rise + 0.286
   只判「手腕比基准抬高 0.286 肩宽」，**完全没有「手要靠近脸」的约束**。
   实测后果（生产 68，近 3h，223 条切换日志）：
     - 托腮占用 41.7% 的检测时间，比「正常」(32.3%) 还多
     - 77 次托腮触发里 16 次 wrist_near > 1.0（手离鼻子一个肩宽以上，物理上不可能托腮）
     - 短命误报(<5s) 的 wrist_rise 中位 0.522，反倒高于真托腮(<30s) 的 0.486
       → wrist_rise 测的不是「手在脸边」，而是「前倾/打字时肩线下降」

## 阈值 0.55 的依据（实测，不是拍的）

    真实托腮（教学样本里 wrist_near 有效的帧，n=9）:  0.360 ─ 0.450
    生产误报（77 次触发）          :  0.420 ─ 1.334，中位 0.835

逐档「压制率 / 误杀率」：
    thr=0.45  压掉 97%  误杀 0/9
    thr=0.50  压掉 94%  误杀 0/9
    thr=0.55  压掉 90%  误杀 0/9   ← 取这个，留安全边际
    thr=0.60  压掉 86%  误杀 0/9

取 0.55（不是 0.45）的理由：真托腮上界 0.450 只比 0.45 差一点，
样本只有 9 帧，留 0.10 肩宽的余量给「手稍远一点的托腮」，避免把真托腮全杀掉。

## 注意 wrist_near 的哨兵值

教学样本里 hand_face 有 13/22 帧 wrist_near = 3.0（哨兵 = 该帧被门控吃掉）。
原因：教学脚本读 `wrist_to_nose`（门控后），而 C++ 特征用 `wrist_to_nose_raw`。
**运行时生产日志里 wrist_near 全部有效**（77/77），所以本阈值在运行时是可靠的；
教学样本的哨兵帧只是采集工具的历史缺陷，不影响本次判定。
用 guard 时哨兵值 3.0 > 0.55 会被拒 —— 方向正确（无信息 → 不报警，宁漏不误）。

用法：
    python3 regen_profile_v4.py <板端 postures.txt> <输出路径>
"""
import sys

FEATURES = ["head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise",
            "wrist_near", "wrist_face"]
NF = len(FEATURES)

# 托腮硬约束：手腕到鼻子的距离必须 < 该值（单位：肩宽）
HAND_FACE_NEAR_MAX = 0.55

# 高低肩：肩线相对基准偏角必须 >= 该值（度）
#
# ★ 为什么之前完全没有这条：生效画像只有 5 类（normal/head_down/head_tilt/
#   too_close/hand_face），**没有 shoulder_tilt**。而 ProfileUpdate 一旦返回结论
#   就直接 return，传统规则路径（含 shoulder_rule_ 高低肩）根本不会执行
#   → 高低肩在画像模式下永远不会被报出来。这是真实功能缺失。
#
# 阈值 12° 的依据：
#   教学样本 shoulder_tilt 分布（度）：
#     normal      min 5.86  med  7.56  max 10.06
#     head_down   min 0.06  med  2.24  max  8.46
#     head_tilt   min 11.86 med 14.26  max 16.46
#     too_close   min 3.36  med  5.86  max  8.76
#     hand_face   min 9.46  med  9.56  max 12.96
#   12° 时：正常样本误报 0/29；同时沿用 config 的 shoulder_trigger_deg 默认值。
#
# ★ 关键设计：这条必须排在**最后**（priority 5）。因为「歪头会连带抬肩线」——
#   head_tilt 样本的 shoulder_tilt（med 14.26°）与高低肩高度重叠，若排在前面
#   会把歪头误报成高低肩。级联先命中 head_tilt 就 return，天然给高低肩做了
#   互斥门控。这是用级联顺序替代额外 guard 的取舍：少一个参数，行为可预测。
SHOULDER_TILT_TRIGGER_DEG = 12.0
WRIST_FACE_SCALE = "0.1500"
# wrist_face 没有采集数据 → 用哨兵中位 3.0，MAD 0
WRIST_FACE_MED = "3.0000"
WRIST_FACE_MAD = "0.0000"


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/var/lib/posture-guard/postures.txt"
    dst = sys.argv[2] if len(sys.argv) > 2 else "/tmp/postures-v4.txt"

    out_lines = []
    stats = {"scale": 0, "class": 0, "rule": 0, "rule_guarded": 0}

    for line in open(src):
        raw = line.rstrip("\n")
        if not raw or raw.startswith("#"):
            out_lines.append(raw)
            continue
        c = raw.split("\t")
        kind = c[0]

        if kind == "scale":
            vals = c[1:]
            while len(vals) < NF:
                vals.append(WRIST_FACE_SCALE)
            out_lines.append("scale\t" + "\t".join(vals))
            stats["scale"] += 1

        elif kind == "class":
            # class <label> <n> <med*NF> <mad*NF>
            label, n = c[1], c[2]
            vals = c[3:]
            if len(vals) == 2 * (NF - 1):          # 旧：6 med + 6 mad
                med, mad = vals[:NF - 1], vals[NF - 1:]
                med.append(WRIST_FACE_MED)
                mad.append(WRIST_FACE_MAD)
                vals = med + mad
            elif len(vals) != 2 * NF:
                raise SystemExit(f"class 行字段数异常（{len(vals)}，期望 {2*NF}）：{raw}")
            out_lines.append("\t".join([kind, label, n] + vals))
            stats["class"] += 1

        elif kind == "rule":
            # rule <pri> <label> <feat> <dir> <thr> <nmed> <cmed>
            #      [<g1> <max1>] [<g2> <dir2> <max2>]
            label = c[2]
            feat = c[3]
            # 先截断到「基础 8 列」，避免旧 guard 字段与新增字段冲突
            base = c[:8]
            while len(base) < 8:
                base.append("0")

            if label == "hand_face":
                # 硬约束：手腕必须真的在脸旁。
                # 用 wrist_near（到鼻子距离）而不是 wrist_face（到耳中点）：
                #   wrist_face 是新特征，板上尚无采集数据，无法定阈值；
                #   wrist_near 在生产日志里 100% 有效，且已实测出干净分离。
                if feat != "wrist_rise":
                    raise SystemExit(f"hand_face 规则的主特征变了（{feat}），"
                                     f"请重新核对 guard 设计：{raw}")
                out_lines.append("\t".join(base + ["wrist_near", f"{HAND_FACE_NEAR_MAX:.4f}"]))
                stats["rule_guarded"] += 1
            else:
                out_lines.append("\t".join(base))
            stats["rule"] += 1

        else:
            raise SystemExit(f"未知行类型：{raw}")

    # 追加高低肩规则（priority 5 = 最后）。
    # 顺序很重要：歪头会连带抬肩线，必须先由 head_tilt 命中并 return，
    # 级联天然把「歪头导致的高肩线」挡在外面，无需额外 guard。
    out_lines.append(
        "\t".join(["rule", "5", "shoulder_tilt", "shoulder_tilt", "+",
                   f"{SHOULDER_TILT_TRIGGER_DEG:.4f}",
                   "7.5570",     # normal 中位（教学样本实测）
                   "14.2570"]))  # 该类中位（取 head_tilt 作参考上界）
    stats["rule"] += 1

    with open(dst, "w") as f:
        f.write("\n".join(out_lines) + "\n")

    print(f"写入 {dst}")
    print(f"  scale 行 {stats['scale']}，class 行 {stats['class']}，"
          f"rule 行 {stats['rule']}（其中托腮加了 guard {stats['rule_guarded']}）")
    print(f"  托腮硬约束：wrist_near < {HAND_FACE_NEAR_MAX}")
    print(f"  已追加高低肩规则：shoulder_tilt >= {SHOULDER_TILT_TRIGGER_DEG}（priority 5，最后）")


if __name__ == "__main__":
    main()
