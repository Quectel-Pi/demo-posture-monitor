#!/usr/bin/env python3
"""取景助手 —— 实时告诉你摄像头摆得对不对

为什么需要：坐姿判定靠的是「头 - 肩 - 手」的几何关系。如果人离镜头太近，
画面里只剩一个巨大的头（实测过 box 高度占 95% 画面、头顶被裁掉一截），
模型就拿不到身体上下文，关键点会变成一堆噪声（肩宽 0~22px），
再好的阈值也救不回来。这是物理问题，必须先把机位调对。

用法（板端，另开一个终端跑，服务继续运行）：
    python3 scripts/frame_check.py

输出每 0.5 秒刷新一次，带 ✅/⚠️/❌ 判定和具体调整建议。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

DEFAULT_SOCKET = "/run/posture-guard/status.sock"

# 取景判据（来自本项目的实测标定）
#   box 高度占画面比例：太大人会出框、太小关键点精度不够
BOX_H_MIN = 0.45
BOX_H_MAX = 0.85
#   头顶到画面上沿至少留这么多（否则头被裁、鼻子/耳朵不可靠）
HEAD_TOP_MARGIN_MIN = 0.06
#   肩宽像素：低于 120 时关键点噪声会盖过 12° 的高低肩阈值
SHOULDER_MIN_PX = 120.0


def read_status(path: str, timeout: float = 5.0) -> dict | None:
    import socket

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get("type") == "status":
                    return msg
    finally:
        s.close()
    return None


def evaluate(d: dict) -> tuple[str, list[str], list[str]]:
    """返回 (总判定, 问题列表, 建议列表)"""
    problems: list[str] = []
    advice: list[str] = []

    if not d.get("present") and not d.get("box_valid"):
        return "❌ 画面里没有检测到人", ["没人在画面里"], [
            "请坐到摄像头前；或检查摄像头是否对着座位"]

    fw = d.get("frame_width") or 640
    fh = d.get("frame_height") or 480
    bw = d.get("box_w") or 0
    bh = d.get("box_h") or 0
    bx = d.get("box_x") or 0
    by = d.get("box_y") or 0
    wr = d.get("width_ratio") or 0
    sw_px = wr * fw

    box_h_ratio = bh / fh if fh else 0
    head_top = by / fh if fh else 0

    if box_h_ratio > BOX_H_MAX:
        problems.append(f"人太大（占画面高 {box_h_ratio * 100:.0f}%）")
        advice.append("把摄像头往后移，或让人坐远一点")
    elif box_h_ratio < BOX_H_MIN:
        problems.append(f"人太小（占画面高 {box_h_ratio * 100:.0f}%）")
        advice.append("把摄像头挪近一点，或让人坐近一点")

    if head_top < HEAD_TOP_MARGIN_MIN:
        problems.append(f"头顶贴到画面上沿（余量 {head_top * 100:.0f}%，需 ≥{HEAD_TOP_MARGIN_MIN * 100:.0f}%）")
        advice.append("把摄像头抬高，或把镜头往上仰一点")

    if sw_px < SHOULDER_MIN_PX:
        problems.append(f"肩宽只有 {sw_px:.0f}px（需 ≥{SHOULDER_MIN_PX:.0f}px）")
        advice.append("人离镜头太远，关键点精度不够；挪近一点")

    if d.get("box_x", 0) < 0 or d.get("box_y", 0) < 0:
        problems.append("人体框超出画面左上角")
        advice.append("人往画面右下方向移，或把摄像头往左上方向调")

    if not d.get("metrics_valid") and not problems:
        problems.append(f"关键点不可用：{d.get('explanation') or '未知'}")

    if not problems:
        return "✅ 取景 OK", [], []
    if len(problems) == 1 and "太小" in problems[0]:
        return "⚠️ 可以但偏小", problems, advice
    return "❌ 需要调整", problems, advice


def main() -> int:
    ap = argparse.ArgumentParser(description="摄像头取景助手")
    ap.add_argument("--socket", default=os.environ.get("POSTURE_SOCKET", DEFAULT_SOCKET))
    ap.add_argument("--once", action="store_true", help="只检查一次")
    args = ap.parse_args()

    if not os.path.exists(args.socket):
        print(f"状态 socket 不存在: {args.socket}", file=sys.stderr)
        return 1

    print("取景助手：每 0.5 秒刷新。Ctrl+C 退出。")
    print(f"判据：人体占画面高 {BOX_H_MIN*100:.0f}~{BOX_H_MAX*100:.0f}%，"
          f"头顶余量 ≥{HEAD_TOP_MARGIN_MIN*100:.0f}%，肩宽 ≥{SHOULDER_MIN_PX:.0f}px")
    print()
    try:
        while True:
            d = read_status(args.socket)
            if d is None:
                print("读不到状态…")
                time.sleep(1)
                continue
            verdict, problems, advice = evaluate(d)
            fw = d.get("frame_width") or 640
            fh = d.get("frame_height") or 480
            lines = [
                f"人体框 {d.get('box_w', 0):.0f}x{d.get('box_h', 0):.0f}px  "
                f"占画面高 {(d.get('box_h', 0) / fh * 100 if fh else 0):.0f}%  "
                f"肩宽 {(d.get('width_ratio', 0) or 0) * fw:.0f}px  "
                f"头顶余量 {(d.get('box_y', 0) / fh * 100 if fh else 0):.0f}%",
                f"判定：{verdict}",
            ]
            for p in problems:
                lines.append(f"   · {p}")
            for a in advice:
                lines.append(f"   → {a}")
            text = "\n".join(lines)
            if sys.stdout.isatty():
                os.system("clear")
            print(text)
            if args.once:
                return 0 if verdict.startswith("✅") else 2
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n退出")
        return 0


if __name__ == "__main__":
    sys.exit(main())
