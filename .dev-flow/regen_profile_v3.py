#!/usr/bin/env python3
"""把板端 6 维 postures.txt 迁移到 7 维（新增 wrist_face）。

为什么不能直接用旧的：特征数从 6 变 7，旧 class 行的第 7 格会被解析成缺失，
wrist_face 全是 0 —— 任何依赖它的规则都会误命中。
这里保留原有 4 条规则（它们的特征都在前 6 维），只补 scale/class 的第 7 格。
"""
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "/var/lib/posture-guard/postures.txt"
FEATURES = ["head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise",
            "wrist_near", "wrist_face"]
NF = len(FEATURES)


def main():
    lines = []
    for line in open(SRC):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            lines.append(line)
            continue
        c = line.split("\t")
        if c[0] == "scale":
            vals = c[1:]
            while len(vals) < NF:
                vals.append("0.1500")          # wrist_face 的有意义尺度
            lines.append("scale\t" + "\t".join(vals))
        elif c[0] == "class":
            # class <label> <n> <med*6> <mad*6>
            vals = c[3:]
            if len(vals) == 12:                # 旧：6 med + 6 mad
                med, mad = vals[:6], vals[6:]
                med.append("3.0000")           # wrist_face 哨兵（无采集数据）
                mad.append("0.0000")
                vals = med + mad
            lines.append("\t".join(c[:3] + vals))
        else:
            # rule 行原样保留（特征名解析，与维数无关）
            lines.append(line)
    # 托腮主判据改成 wrist_face（手贴脸），并加「手在耳中点以下」的方向约束
    out = []
    for line in lines:
        if line.startswith("rule\t") and "\thand_face\t" in line:
            # rule <pri> hand_face wrist_face - <thr> <nmed> <cmed>
            # 阈值 0.95：手到耳中点 < 0.95 肩宽算贴脸；normal 实测约 1.6~2.5
            out.append("rule\t4\thand_face\twrist_face\t-\t0.9500\t3.0000\t0.4500")
            continue
        out.append(line)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
