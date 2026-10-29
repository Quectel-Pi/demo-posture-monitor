#!/usr/bin/env python3
"""取景调优 —— 只调摄像头位置，实时反馈，直到 ✅

用法（板端终端，服务保持运行）：
    python3 scripts/frame_tune.py

它会每 0.4 秒打印当前取景状态 + 一句"下一步动作"。
"""
from __future__ import annotations
import json, os, socket, sys, time

SOCKET = "/run/posture-guard/status.sock"
# 目标（来自实测标定）
TOP_MARGIN_OK = 0.06      # 头顶留 ≥6%
TOP_MARGIN_MIN = 0.03
H_RATIO_MAX = 0.85        # 人占画面高 ≤85%
H_RATIO_MIN = 0.40
W_RATIO_MAX = 0.80
SW_PX_MIN = 150           # 肩宽 ≥150px 才能保证关键点精度


def read(timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCKET)
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                c = s.recv(65536)
            except socket.timeout:
                break
            if not c:
                break
            buf += c
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if m.get("type") == "status":
                    return m
    finally:
        s.close()
    return None


# 关键点下标（COCO 17）：0 鼻 1 左眼 2 右眼 3 左耳 4 右耳 5 左肩 6 右肩
#                      7 左肘 8 右肘 9 左腕 10 右腕
KP_NOSE, KP_EAR_L, KP_EAR_R, KP_SH_L, KP_SH_R = 0, 3, 4, 5, 6
SAFE = 0.03          # 关键点离画面边缘至少留 3%
WARN = 0.08          # 小于 8% 提醒「偏紧」


def kp_judge(d: dict):
    """按关键点是否入画来判断取景 —— 这才是系统真正需要的东西。

    之前按「人体框」判断会骗人：框是模型外推的，桌面机位人只露头肩时，
    框顶常常落在画面外 -100px 以上，但这完全不影响判定。
    反过来，框看着在画面里、鼻子却跑出去了，才是真问题。
    """
    kps = d.get("keypoints") or []
    if not d.get("keypoints_valid") or len(kps) < 14:
        return None
    def at(i):
        return (kps[2 * i], kps[2 * i + 1])
    probs, acts = [], []
    nose = at(KP_NOSE)
    if nose[1] < SAFE:
        probs.append(f"鼻子贴到上边缘了（y={nose[1]*100:.0f}%）")
        acts.append("★ 摄像头往上抬 / 镜头上仰，让鼻子离开上边缘至少 8%")
    elif nose[1] < WARN:
        probs.append(f"鼻子离上边缘偏近（y={nose[1]*100:.0f}%，建议 ≥8%）")
        acts.append("摄像头再上抬一点点 / 轻微上仰")
    if nose[0] < SAFE or nose[0] > 1 - SAFE:
        probs.append(f"鼻子贴到左右边缘（x={nose[0]*100:.0f}%）")
        acts.append("把人挪到画面正中间")
    for name, idx in (("左肩", KP_SH_L), ("右肩", KP_SH_R)):
        k = at(idx)
        if k[1] > 1 - SAFE or k[0] < SAFE or k[0] > 1 - SAFE:
            probs.append(f"{name}出画/贴边")
            acts.append("坐远一点，或把摄像头后退，让双肩完整入画")
    if d.get("ear_angle_valid"):
        for name, idx in (("左耳", KP_EAR_L), ("右耳", KP_EAR_R)):
            k = at(idx)
            if k[1] < SAFE or k[1] > 1 - SAFE:
                probs.append(f"{name}出画（歪头判定会失效）")
                acts.append("摄像头抬高/上仰，让双耳都露出来")
    status = "✅ 取景 OK —— 可以开始校准了" if not probs else "❌ 还需调整"
    return status, probs, acts


def judge(d: dict):
    if not d:
        return "❌ 读不到状态", ["服务没响应"], ["检查 systemctl status posture-guard"]
    # 优先用「合格框」，被门控拒了就用「最大检出框」——现场调机位必须有数看
    if d.get("box_valid"):
        bw = d.get("box_w", 0); bh = d.get("box_h", 0)
        by = d.get("box_y", 0); bx = d.get("box_x", 0)
    elif d.get("raw_box_valid"):
        bw = d.get("raw_box_w", 0); bh = d.get("raw_box_h", 0)
        by = d.get("raw_box_y", 0); bx = d.get("raw_box_x", 0)
    else:
        return ("❌ 画面里没有人",
                [d.get("explanation") or "没有任何检出"],
                ["坐正、正对摄像头，确认镜头没对着别的方向"])
    kp = kp_judge(d)
    if kp is not None:
        return kp
    fh = d.get("frame_height") or 480
    fw = d.get("frame_width") or 640
    wr = d.get("width_ratio") or 0
    if wr <= 0 and bw > 0:
        # 被拒时 width_ratio 为 0，用框宽估一个粗略值仅供参考
        wr = 0.0
    top = by / fh if fh else 0
    hr = bh / fh if fh else 0
    wratio = bw / fw if fw else 0
    sw = wr * fw

    probs, acts = [], []
    # 先判断「人太大」——它和「头顶被裁」通常是同一个原因（离太近）
    if hr > H_RATIO_MAX:
        probs.append(f"人太大（占高 {hr*100:.0f}%）")
        acts.append("★ 坐远一点，或把摄像头往后退 20~30cm  ← 最关键")
    if top < TOP_MARGIN_MIN:
        probs.append(f"头顶被裁（余量 {top*100:+.0f}%）")
        acts.append("摄像头往上抬/镜头往上仰，或坐远一点")
    elif top < TOP_MARGIN_OK:
        probs.append(f"头顶余量偏紧（{top*100:.0f}%，目标 ≥6%）")
        acts.append("再把摄像头抬一点点，或轻微上仰镜头")
    if hr < H_RATIO_MIN:
        probs.append(f"人太小（占高 {hr*100:.0f}%）")
        acts.append("坐近一点，或把摄像头往前挪")
    if wratio > W_RATIO_MAX:
        probs.append(f"人太宽（占宽 {wratio*100:.0f}%）")
        acts.append("坐远一点")
    if wr > 0 and sw < SW_PX_MIN:
        probs.append(f"肩宽仅 {sw:.0f}px（需 ≥{SW_PX_MIN}）")
        acts.append("坐近一点，让肩膀更清楚")

    status = "✅ 取景 OK —— 可以开始校准了" if not probs else "❌ 还需调整"
    return status, probs, acts


def main():
    print("取景调优：每 0.4s 刷新。只调摄像头位置，不要改变坐姿。Ctrl+C 退出。\n")
    try:
        while True:
            d = read()
            status, probs, acts = judge(d)
            if sys.stdout.isatty():
                os.system("clear")
            if d and (d.get("box_valid") or d.get("raw_box_valid")):
                fh = d.get("frame_height") or 480
                fw = d.get("frame_width") or 640
                ok = d.get("box_valid")
                if ok:
                    bw, bh, by = d.get("box_w", 0), d.get("box_h", 0), d.get("box_y", 0)
                else:
                    bw, bh, by = (d.get("raw_box_w", 0), d.get("raw_box_h", 0),
                                  d.get("raw_box_y", 0))
                tag = "" if ok else "（被门控拒）"
                print(f"人体框 {bw:.0f}x{bh:.0f}px{tag}   "
                      f"占高 {bh/fh*100:.0f}%   占宽 {bw/fw*100:.0f}%   "
                      f"头顶余量 {by/fh*100:+.0f}%")
            if d and d.get("reject_reason") and not d.get("box_valid"):
                print(f"系统判定：{d['reject_reason'][:90]}")
            print(f"\n{status}")
            for p in probs:
                print(f"   ✗ {p}")
            for a in acts:
                print(f"   → {a}")
            if not probs:
                print("\n   下一步：")
                print("     rm -f /var/lib/posture-guard/baseline.json")
                print("     systemctl restart posture-guard")
                print("     坐正保持 5 秒（自动校准）")
            time.sleep(0.4)
    except KeyboardInterrupt:
        print("\n退出")


if __name__ == "__main__":
    main()
