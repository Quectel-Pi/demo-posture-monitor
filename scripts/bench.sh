#!/bin/bash
# bench.sh — 板端性能基线（可直接贴进汇报）
#
#   sudo bash scripts/bench.sh            # NPU + CPU 各 50 次
#   sudo bash scripts/bench.sh --npu-only
#
# 注意：若应用正在运行会独占摄像头，本脚本不需要摄像头（用离线样图）。
set -uo pipefail
APP=/opt/posture-monitor
NPU_ONLY=0
[ "${1:-}" = "--npu-only" ] && NPU_ONLY=1

echo "== 环境 =="
cat /etc/os-release | grep PRETTY_NAME
echo "内核: $(uname -r)  SoC: $(grep -m1 -o 'rockchip,rk[0-9]*' /proc/device-tree/compatible 2>/dev/null | head -1)"
echo "nproc: $(nproc)  mem: $(awk '/MemTotal/{printf "%.1f GB\n", $2/1048576}' /proc/meminfo)"
python3 -c "
import re
d=open('/usr/lib/librknnrt.so','rb').read()
m=re.search(rb'librknnrt version: ([0-9.]+)', d)
print('librknnrt:', m.group(1).decode() if m else 'unknown')"
cat /sys/kernel/debug/rknpu/version 2>/dev/null | sed 's/^/  /'
echo

PYTHONPATH=$APP NPU_ONLY=$NPU_ONLY python3 - <<'PY'
import os, sys, time, numpy as np, cv2
NPU_ONLY = os.environ.get("NPU_ONLY") == "1"
sys.path.insert(0, "/opt/posture-monitor")
sys.path.insert(0, "/opt/posture-monitor/lib")
from posturemon import yolopose

img = cv2.imread("/opt/posture-monitor/samples/zidane.jpg")
canvas, r, px, py = yolopose.letterbox(img, 640)
rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
N = 50


def run(label, fn):
    fn()
    ts = []
    for _ in range(N):
        t = time.perf_counter()
        fn()
        ts.append((time.perf_counter() - t) * 1000)
    ts.sort()
    print(f"  {label:26s} median {ts[N//2]:6.1f} ms   p90 {ts[int(N*0.9)]:6.1f} ms   "
          f"min {ts[0]:6.1f}   fps {1000/ts[N//2]:5.1f}")
    return ts[N // 2]


print(f"== 推理后端（{N} 次，640x640 输入） ==")
results = {}
try:
    from posturemon.backends import RknnBackend
    be = RknnBackend("/opt/posture-monitor/models/yolov8n-pose-rknn.rknn")
    results["rknn"] = run("RKNN w8a8 @ NPU", lambda: be.infer(rgb))
    be.close()
except Exception as e:
    print(f"  RKNN 不可用: {type(e).__name__}: {e}")

if not NPU_ONLY:
    for threads in (4, 6, 8):
        try:
            from posturemon.backends import OnnxBackend
            ob = OnnxBackend("/opt/posture-monitor/models/yolov8n-pose.onnx", threads=threads)
            results[f"onnx{threads}"] = run(f"ONNX fp32 @ CPU {threads} 线程", lambda: ob.infer(rgb))
        except Exception as e:
            print(f"  ONNX({threads}) 不可用: {type(e).__name__}: {e}")
            break
    try:
        from posturemon.backends import OnnxBackend
        qi = OnnxBackend("/opt/posture-monitor/models/yolov8n-pose-int8.onnx", threads=6)
        run("ONNX int8 @ CPU 6 线程", lambda: qi.infer(rgb))
    except Exception as e:
        print(f"  ONNX int8 不可用: {type(e).__name__}: {e}")

if results.get("rknn") and results.get("onnx6"):
    print(f"\n  NPU 相对 CPU(6 线程) 加速: {results['onnx6']/results['rknn']:.1f}x")
PY

echo
echo "== 摄像头 =="
PYTHONPATH=$APP python3 - <<'PY'
import sys, time
sys.path.insert(0, "/opt/posture-monitor")
import cv2
from posturemon.camera import list_cameras
devs = list_cameras()
print("  设备:", devs or "无")
if devs:
    cap = cv2.VideoCapture(devs[0], cv2.CAP_V4L2)
    if not cap.isOpened():
        print("  打不开（可能被运行中的应用独占 → 先 appctl.sh stop）")
    else:
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640); cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        for _ in range(5):
            cap.read()
        ts = []
        for _ in range(30):
            t = time.perf_counter(); ok, f = cap.read()
            if ok:
                ts.append((time.perf_counter() - t) * 1000)
        cap.release()
        ts.sort()
        print(f"  {f.shape[1]}x{f.shape[0]} YUYV  median {ts[len(ts)//2]:.1f} ms  "
              f"fps {1000/ts[len(ts)//2]:.1f}")
PY

echo
echo "== 内存在用 =="
ps -eo rss,cmd --sort=-rss 2>/dev/null | grep -E "posturemon" | grep -v grep | \
    awk '{printf "  python3 -m posturemon  RSS %.1f MB\n", $1/1024}' || echo "  应用未运行"
