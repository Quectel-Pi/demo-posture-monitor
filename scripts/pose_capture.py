#!/usr/bin/env python3
"""姿势样本采集器 —— 现场标定/排障用

为什么需要它：判定不生效时，「阈值不对」还是「关键点本身不可信」是完全不同的
两件事，只看最终状态分不出来。这个工具把每个姿势下的原始关键点、派生指标、
每条规则的「当前值 vs 阈值 vs 已持续」一起记下来，事后可以做数值对比。

用法（板端）：
  python3 scripts/pose_capture.py --label normal        # 正常坐姿
  python3 scripts/pose_capture.py --label head_down     # 低头
  python3 scripts/pose_capture.py --label head_tilt     # 歪头
  python3 scripts/pose_capture.py --label lean_close    # 凑近屏幕
  python3 scripts/pose_capture.py --label hand_face     # 托腮
  python3 scripts/pose_capture.py --label away          # 离开座位

  --seconds 6   采集时长（默认 6s，够覆盖 hold_s=5s）
  --shot        同时抓一张屏幕截图（默认开）
  --out DIR     输出目录（默认 /var/lib/posture-guard/poses）

输出：
  每个 label 一个目录，含
    sample.json   逐帧完整 status 快照 + 汇总统计
    screen.png    当时的屏幕截图（人能看，用于对照）
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time

DEFAULT_SOCKET = "/run/posture-guard/status.sock"
DEFAULT_OUT = "/var/lib/posture-guard/poses"

KEYPOINT_NAMES = [
    "nose", "eye_l", "eye_r", "ear_l", "ear_r",
    "sh_l", "sh_r", "elbow_l", "elbow_r", "wrist_l", "wrist_r",
    "hip_l", "hip_r", "knee_l", "knee_r", "ankle_l", "ankle_r",
]


def read_frames(socket_path: str, seconds: float, timeout: float = 8.0) -> list[dict]:
    """连续采集 seconds 秒内的所有 status 消息。"""
    import socket as sock_mod

    frames: list[dict] = []
    s = sock_mod.socket(sock_mod.AF_UNIX, sock_mod.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(socket_path)
        buf = b""
        deadline = time.time() + seconds
        s.settimeout(1.0)
        while time.time() < deadline:
            try:
                chunk = s.recv(65536)
            except sock_mod.timeout:
                continue
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
                    frames.append(msg)
    finally:
        s.close()
    return frames


def pixels(frame: dict) -> list[tuple[float, float]]:
    """归一化关键点 -> 原图像素坐标"""
    kp = frame.get("keypoints") or []
    w = frame.get("frame_width") or 640
    h = frame.get("frame_height") or 480
    return [(kp[2 * i] * w, kp[2 * i + 1] * h) for i in range(min(17, len(kp) // 2))]


def summarize(label: str, frames: list[dict]) -> dict:
    valid = [f for f in frames if f.get("metrics_valid")]
    out: dict = {
        "label": label,
        "frames": len(frames),
        "valid_frames": len(valid),
        "states": {},
        "rules_fired": {},
    }
    for f in frames:
        st = f.get("posture_state", "?")
        out["states"][st] = out["states"].get(st, 0) + 1
        for r in f.get("active_rules") or []:
            out["rules_fired"][r] = out["rules_fired"].get(r, 0) + 1
    if not valid:
        return out

    def stat(key):
        vals = [f.get(key) for f in valid if isinstance(f.get(key), (int, float))]
        if not vals:
            return {}
        return {
            "median": round(statistics.median(vals), 4),
            "min": round(min(vals), 4),
            "max": round(max(vals), 4),
            "stdev": round(statistics.pstdev(vals), 4) if len(vals) > 1 else 0.0,
        }

    for k in ("shoulder_angle_deg", "shoulder_tilt_dev", "head_offset_y",
              "ear_mid_offset_y", "ear_angle_deg", "width_ratio",
              "wrist_to_nose"):
        v = stat(k)
        if v:
            out[k] = v

    # 每条规则的「值 vs 阈值」：这是判断"阈值不对"还是"数据不对"的关键
    rule_stats: dict[str, dict] = {}
    for f in valid:
        for r in f.get("rules") or []:
            key = r.get("key")
            if not key:
                continue
            slot = rule_stats.setdefault(key, {"values": [], "trigger": r.get("trigger"),
                                               "hold_s": r.get("hold_s"),
                                               "latched_frames": 0})
            if r.get("has_value"):
                slot["values"].append(r.get("value"))
            if r.get("latched"):
                slot["latched_frames"] += 1
    for key, slot in rule_stats.items():
        vals = slot.pop("values")
        if vals:
            slot["value_median"] = round(statistics.median(vals), 4)
            slot["value_min"] = round(min(vals), 4)
            slot["value_max"] = round(max(vals), 4)
        slot["ratio_vs_trigger"] = (
            round(statistics.median(vals) / slot["trigger"], 3)
            if vals and slot.get("trigger") else None
        )
    out["rule_diag"] = rule_stats

    # 关键点稳定性：同一姿势内不该乱跳。抖得厉害说明这个点不可信。
    kp_std = {}
    for i, name in enumerate(KEYPOINT_NAMES):
        xs, ys = [], []
        for f in valid:
            p = pixels(f)
            if i < len(p):
                xs.append(p[i][0])
                ys.append(p[i][1])
        if len(xs) > 1:
            kp_std[name] = {
                "x_std": round(statistics.pstdev(xs), 1),
                "y_std": round(statistics.pstdev(ys), 1),
                "x_med": round(statistics.median(xs), 1),
                "y_med": round(statistics.median(ys), 1),
            }
    out["keypoint_std"] = kp_std
    return out


def take_screenshot(path: str) -> bool:
    for cmd in (["bash", "/opt/posture-guard/scripts/screenshot.sh", path],
                ["bash", "scripts/screenshot.sh", path]):
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=30)
            if r.returncode == 0 and os.path.exists(path):
                return True
        except (OSError, subprocess.TimeoutExpired):
            continue
    return False


GUIDED_POSES = [
    ("normal", "正常坐姿：坐直、双手放桌面、目视屏幕",
     "这是基准姿势，后面对比都靠它"),
    ("head_down", "低头：像看键盘/手机那样明显低头，下巴朝胸口",
     "预期 head_down 触发"),
    ("head_tilt", "歪头：把头向一侧明显歪 20-30 度（耳朵朝肩膀）",
     "预期 head_tilt 触发"),
    ("lean_close", "凑近：身体前倾、脸向屏幕靠近（别真贴上）",
     "预期 too_close 触发"),
    ("hand_face", "托腮：单手托住下巴/脸颊，肘撑桌面",
     "预期 hand_on_face 触发"),
    ("away", "离开：起身离开座位，画面里没有人",
     "预期 USER_AWAY、用眼会话结束"),
]


def guided(args) -> int:
    dur = max(4.0, args.seconds)
    total = len(GUIDED_POSES) * dur * 2
    print("=" * 62)
    print(f"引导式采集：每个姿势 倒计时{dur:.0f}s + 采集{dur:.0f}s，"
          f"全程约 {total/60:.1f} 分钟")
    print("=" * 62)
    rc = 0
    for label, pose, expect in GUIDED_POSES:
        print()
        print("-" * 62)
        print(f"【{label}】{pose}")
        print(f"          {expect}")
        print("-" * 62)
        for i in range(int(dur), 0, -1):
            print(f"   请摆好姿势… {i}    ", end="\r", flush=True)
            time.sleep(1)
        print(f"   开始采集（保持 {dur:.0f} 秒）        ")
        frames = read_frames(args.socket, dur)
        if not frames:
            print("   !! 没收到数据，跳过")
            rc = 1
            continue
        d = os.path.join(args.out, label)
        os.makedirs(d, exist_ok=True)
        if not args.no_shot:
            take_screenshot(os.path.join(d, "screen.png"))
        summary = summarize(label, frames)
        with open(os.path.join(d, "sample.json"), "w") as f:
            json.dump({"summary": summary, "raw": frames}, f, ensure_ascii=False, indent=1)
        fired = summary.get("rules_fired") or {}
        print(f"   帧 {summary['valid_frames']}/{summary['frames']}  "
              f"状态 {summary['states']}")
        print(f"   触发 {fired or '（无）'}")
    print()
    print("=" * 62)
    print(f"全部完成，数据在 {args.out}")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description="坐姿样本采集")
    ap.add_argument("--label", help="姿势标签，如 normal/head_down")
    ap.add_argument("--guided", action="store_true", help="引导式：自动走完 6 个姿势")
    ap.add_argument("--socket", default=os.environ.get("POSTURE_SOCKET", DEFAULT_SOCKET))
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--no-shot", action="store_true", help="不抓屏")
    args = ap.parse_args()

    if not os.path.exists(args.socket):
        print(f"!! 状态 socket 不存在: {args.socket}", file=sys.stderr)
        return 1

    if args.guided:
        return guided(args)
    if not args.label:
        print("!! 需要 --label 或 --guided", file=sys.stderr)
        return 1

    print(f"== 采集 {args.seconds:.0f} 秒（标签 {args.label}）==")
    for i in range(3, 0, -1):
        print(f"   {i}…", end="", flush=True)
        time.sleep(1)
    print(" 开始，请保持姿势")

    frames = read_frames(args.socket, args.seconds)
    if not frames:
        print("!! 没收到任何 status 消息", file=sys.stderr)
        return 1

    d = os.path.join(args.out, args.label)
    os.makedirs(d, exist_ok=True)

    shot = None
    if not args.no_shot:
        p = os.path.join(d, "screen.png")
        shot = take_screenshot(p)
        print(f"   截图: {'成功 ' + p if shot else '失败（不影响数值分析）'}")

    summary = summarize(args.label, frames)
    with open(os.path.join(d, "sample.json"), "w") as f:
        json.dump({"summary": summary, "raw": frames}, f, ensure_ascii=False, indent=1)

    print(f"\n== {args.label} ==")
    print(f"   帧数 {summary['frames']}（有效 {summary['valid_frames']}）")
    print(f"   状态分布 {summary['states']}")
    print(f"   触发规则 {summary['rules_fired'] or '（无）'}")
    for k in ("shoulder_angle_deg", "shoulder_tilt_dev", "head_offset_y",
              "ear_mid_offset_y", "ear_angle_deg", "width_ratio", "wrist_to_nose"):
        if k in summary:
            s = summary[k]
            print(f"   {k:20s} 中位 {s['median']:8.3f}  [{s['min']:.3f}, {s['max']:.3f}]  σ={s['stdev']:.3f}")
    print("\n   规则诊断（值 vs 阈值）：")
    for key, v in (summary.get("rule_diag") or {}).items():
        ratio = v.get("ratio_vs_trigger")
        flag = ""
        if key == "hand_on_face" and (v.get("value_median") or 0) > 10:
            flag = "  ← 门控未通过（手腕没抬到肩线之上，值=99 是哨兵）"
        elif ratio is not None:
            flag = "  ← 未达阈值" if ratio < 1.0 else "  ✔ 超阈值"
        print(f"     {key:15s} 值中位 {v.get('value_median')}  阈值 {v.get('trigger')}"
              f"  比值 {ratio}{flag}  持续门限 {v.get('hold_s')}s  激活帧 {v.get('latched_frames')}")
    print(f"\n   已写入 {d}/sample.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
