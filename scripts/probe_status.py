#!/usr/bin/env python3
"""从 posture-guard 的状态 socket 读一帧，人类可读或原始 JSON 输出。

为什么单独做成 .py 而不是塞进 shell heredoc：
  * socket 上混着 status / video / notification 三类消息，必须挑出 status；
  * 要在 shell 里内嵌带 f-string 和引号的 Python，转义会变成灾难
    （踩过：`python3 - <<'PY'` 还会把 stdin 占掉，管道数据根本进不来）。

用法：
  python3 probe_status.py                 # 人类可读
  python3 probe_status.py --raw           # 原始 JSON 一行
  python3 probe_status.py --watch         # 每秒刷新
  python3 probe_status.py --socket PATH
  python3 probe_status.py --timeout 5
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

DEFAULT_SOCKET = "/run/posture-guard/status.sock"


def read_status(path: str, timeout: float = 6.0) -> dict:
    """读一条 type == status 的消息。跳过 video / notification。"""
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
    raise RuntimeError(f"{path} 上没收到 status 消息（服务在跑吗？）")


def seconds(total: float) -> str:
    total = int(total or 0)
    if total < 60:
        return f"{total}s"
    return f"{total // 60}m{total % 60:02d}s"


def render(d: dict) -> str:
    out = []
    dev = os.path.basename(d.get("device_id") or "")
    out.append(f"后端      : {d.get('backend')} / {d.get('runtime_version')}")
    out.append(f"窗口      : {d.get('frame_width')}x{d.get('frame_height')}   设备: {dev}")
    out.append(f"姿态      : {d.get('posture_state')}   {d.get('posture_state_zh')}"
               f"   {d.get('explanation')}")
    out.append(f"在座/人数 : {d.get('present')} / {d.get('persons_primary', 0)}"
               f"（原始检出 {d.get('persons')}）"
               f"   fps={d.get('fps')}  frame_id={d.get('frame_id')}")

    metrics = " ".join(
        f"{k}={d.get(k)}"
        for k in ("shoulder_angle_deg", "shoulder_tilt_dev", "head_offset_y",
                  "ear_angle_deg", "width_ratio", "wrist_to_nose")
    )
    out.append(f"指标      : {metrics}  (valid={d.get('metrics_valid')})")

    out.append(f"校准      : baseline={d.get('has_baseline')} "
               f"calibrating={d.get('calibrating')} "
               f"{d.get('calibration_progress', 0):.0f}%  "
               f"({d.get('baseline_created_at') or '无'})")
    if d.get("baseline_hint"):
        out.append(f"⚠ 基准提示 : {d.get('baseline_hint')}")
    out.append(f"用眼      : {d.get('session_state')}  已用 {seconds(d.get('use_s'))}"
               f"  剩余 {seconds(d.get('remaining_s'))}")
    out.append(f"今日      : 在座 {int(d.get('today_use_s') or 0)}s"
               f"  不良 {int(d.get('today_bad_s') or 0)}s"
               f"  提醒 {d.get('today_alerts')} 次")

    rules = ", ".join(d.get("active_rules") or []) or "（无）"
    out.append(f"规则      : {rules}" + ("   免打扰中" if d.get("snoozed") else ""))
    if d.get("last_notification"):
        out.append(f"最近提醒  : {d.get('last_notification')}"
                   f" ({int(d.get('last_notification_ago_s') or 0)}s 前)")

    kps = d.get("keypoints") or []
    if d.get("keypoints_valid") and len(kps) >= 34:
        named = ["nose", "eye_l", "eye_r", "ear_l", "ear_r", "sh_l", "sh_r"]
        pairs = " ".join(f"{named[i]}({kps[2*i]:.2f},{kps[2*i+1]:.2f})" for i in range(7))
        out.append(f"关键点    : {pairs} …")
    else:
        out.append("关键点    : 本帧无可用骨架")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="读取 posture-guard 状态")
    ap.add_argument("--socket", default=os.environ.get("POSTURE_SOCKET", DEFAULT_SOCKET))
    ap.add_argument("--raw", action="store_true", help="输出原始 JSON 一行")
    ap.add_argument("--watch", action="store_true", help="每秒刷新")
    ap.add_argument("--timeout", type=float, default=6.0)
    args = ap.parse_args()

    if not os.path.exists(args.socket):
        print(f"状态 socket 不存在: {args.socket}\n"
              f"后台服务没跑？ systemctl status posture-guard", file=sys.stderr)
        return 1

    try:
        if args.watch:
            while True:
                d = read_status(args.socket, args.timeout)
                if sys.stdout.isatty():
                    os.system("clear")
                print(f"===== {time.strftime('%F %T')} =====")
                print(json.dumps(d, ensure_ascii=False) if args.raw else render(d))
                print("（Ctrl+C 退出）", file=sys.stderr)
                time.sleep(1.0)
        d = read_status(args.socket, args.timeout)
    except (RuntimeError, OSError) as exc:
        print(f"读取失败: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(d, ensure_ascii=False) if args.raw else render(d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
