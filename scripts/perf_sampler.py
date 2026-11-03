#!/usr/bin/env python3
"""端到端性能采样（速度/实时性取证用）。

连上状态 socket（就是 UI 客户端的连接方式），按时间戳记录三类消息：

  status          -> fps / latency_ms 序列
  video           -> 视频帧到达间隔（界面是否掉帧）、每帧字节约
  notification    -> 忽略

输出两份东西：
  * 屏幕摘要（直接贴进 evidence 的表格就是它）
  * --json PATH 原始采样（每一条消息一行，便于复算）

用法：
  python3 scripts/perf_sampler.py --seconds 20
  python3 scripts/perf_sampler.py --seconds 20 --json /tmp/perf.json
  python3 scripts/perf_sampler.py --seconds 20 --no-client-…   # 见下
  python3 scripts/perf_sampler.py --seconds 20 --drop-after 3  # 采 3s 后断开（测无客户端）

注意：本工具自己就是一个 UI 客户端，连着的时候服务端一定会编码并推送视频。
要测「没有界面时的开销」，用 --drop-after 先连再断。
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import statistics
import sys
import time

DEFAULT_SOCKET = "/run/posture-guard/status.sock"


def pct(vals: list[float], p: float) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    idx = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[idx]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", default=DEFAULT_SOCKET)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--warmup", type=float, default=3.0,
                    help="前 N 秒丢弃（引擎/摄像头预热）")
    ap.add_argument("--drop-after", type=float, default=0.0,
                    help="连接 N 秒后主动断开（>0 时用来测「无 UI 客户端」）")
    ap.add_argument("--json", default="")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(a.socket)
    t_start = time.monotonic()
    drop_at = t_start + a.drop_after if a.drop_after > 0 else None
    end = t_start + a.seconds

    rows = []
    buf = b""
    s.settimeout(0.5)
    while time.monotonic() < end:
        now = time.monotonic()
        if drop_at is not None and now >= drop_at:
            try:
                s.close()
            except OSError:
                pass
            s = None
            drop_at = None
        if s is not None:
            try:
                chunk = s.recv(262144)
                if not chunk:
                    s.close()
                    s = None
                else:
                    buf += chunk
            except socket.timeout:
                pass
            except OSError:
                s = None
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                t = time.monotonic()
                kind = msg.get("type")
                if kind == "status":
                    rows.append({
                        "t": round(t - t_start, 4), "type": "status",
                        "fps": msg.get("fps"), "latency_ms": msg.get("latency_ms"),
                        "state": msg.get("posture_state"),
                        "present": msg.get("present"),
                    })
                elif kind == "video":
                    rows.append({
                        "t": round(t - t_start, 4), "type": "video",
                        "seq": msg.get("seq"), "w": msg.get("w"), "h": msg.get("h"),
                        "b64": len(msg.get("jpeg") or ""),
                    })
                else:
                    rows.append({"t": round(t - t_start, 4), "type": kind})
        else:
            time.sleep(0.05)

    st = [r for r in rows if r["type"] == "status" and r["t"] >= a.warmup]
    vi = [r for r in rows if r["type"] == "video" and r["t"] >= a.warmup]

    fps_vals = [r["fps"] for r in st if r.get("fps") is not None]
    lat_vals = [r["latency_ms"] for r in st if r.get("latency_ms") is not None]
    gaps = [(vi[i + 1]["t"] - vi[i]["t"]) * 1000.0 for i in range(len(vi) - 1)]
    span = (a.seconds - a.warmup) if a.seconds > a.warmup else a.seconds
    status_hz = len(st) / span if span > 0 else 0.0
    video_hz = len(vi) / span if span > 0 else 0.0

    out = {
        "label": a.label, "socket": a.socket, "seconds": a.seconds,
        "warmup": a.warmup, "drop_after": a.drop_after,
        "status_msgs": len(st), "video_msgs": len(vi),
        "status_hz": round(status_hz, 2), "video_hz": round(video_hz, 2),
        "fps_mean": round(statistics.fmean(fps_vals), 2) if fps_vals else None,
        "fps_min": round(min(fps_vals), 2) if fps_vals else None,
        "fps_max": round(max(fps_vals), 2) if fps_vals else None,
        "lat_mean_ms": round(statistics.fmean(lat_vals), 2) if lat_vals else None,
        "lat_p95_ms": round(pct(lat_vals, 95), 2) if lat_vals else None,
        "lat_min_ms": round(min(lat_vals), 2) if lat_vals else None,
        "lat_max_ms": round(max(lat_vals), 2) if lat_vals else None,
        "video_gap_mean_ms": round(statistics.fmean(gaps), 1) if gaps else None,
        "video_gap_p95_ms": round(pct(gaps, 95), 1) if gaps else None,
        "video_gap_max_ms": round(max(gaps), 1) if gaps else None,
        "video_kb_per_s": round(sum(r.get("b64", 0) for r in vi) / 1024.0 / span, 1) if span else None,
        "raw": rows,
    }

    print(f"== perf sample {a.label or a.socket} ==")
    print(f"   窗口 {a.seconds - a.warmup:.0f}s（丢弃前 {a.warmup:.0f}s）  "
          f"drop_after={a.drop_after}")
    print(f"   status 推送     {status_hz:5.2f} Hz ({len(st)} 条)")
    print(f"   有效推理 FPS    mean {out['fps_mean']}  min {out['fps_min']}  max {out['fps_max']}")
    print(f"   推理耗时        mean {out['lat_mean_ms']}ms  p95 {out['lat_p95_ms']}ms  "
          f"max {out['lat_max_ms']}ms")
    print(f"   视频帧          {video_hz:5.2f} fps ({len(vi)} 帧)  "
          f"间隔 mean {out['video_gap_mean_ms']}ms p95 {out['video_gap_p95_ms']}ms "
          f"max {out['video_gap_max_ms']}ms")
    print(f"   视频带宽        {out['video_kb_per_s']} KB/s (base64)")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
        print(f"   原始采样 -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
