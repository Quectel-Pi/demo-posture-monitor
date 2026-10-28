#!/bin/bash
# verify.sh — 板端自检。不改任何东西，只读 + 跑短时推理。
#   sudo bash scripts/verify.sh            # 全项
#   sudo bash scripts/verify.sh --no-cam   # 跳过摄像头/端到端（无摄像头时）
set -uo pipefail

APP_DIR=/opt/posture-monitor
DATA_DIR=/userdata/posture-monitor/data
CONF=/etc/posture-monitor/config.json
LOG_DIR=/userdata/posture-monitor/log
SKIP_CAM=0
[ "${1:-}" = "--no-cam" ] && SKIP_CAM=1

unset DISPLAY WAYLAND_DISPLAY 2>/dev/null || true
STATUS=/run/posture-monitor/status.json
APP_PID=$(pgrep -f "^python3 -m posturemon" | head -1)
APP_RUNNING=0
[ -n "${APP_PID:-}" ] && APP_RUNNING=1

PASS=0; FAIL=0; WARN=0
res() {  # $1=级别 $2=名称 $3=细节
    case "$1" in
        PASS) PASS=$((PASS+1)); printf "  \033[1;32m[PASS]\033[0m %-34s %s\n" "$2" "$3" ;;
        FAIL) FAIL=$((FAIL+1)); printf "  \033[1;31m[FAIL]\033[0m %-34s %s\n" "$2" "$3" ;;
        WARN) WARN=$((WARN+1)); printf "  \033[1;33m[WARN]\033[0m %-34s %s\n" "$2" "$3" ;;
    esac
}

echo "== 1. 系统与依赖 =="
python3 - <<'PY'
import sys
sys.path.insert(0, "/opt/posture-monitor")
import cv2, numpy, sqlite3
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk
import cairo
print(f"  python {sys.version.split()[0]} | cv2 {cv2.__version__} | numpy {numpy.__version__}")
print(f"  Gtk {Gtk.get_major_version()}.{Gtk.get_minor_version()} | pycairo {cairo.version}")
PY
[ $? -eq 0 ] && res PASS "python 运行时依赖" "cv2/gi/cairo/sqlite3 齐全" || res FAIL "python 运行时依赖" "见上"

if [ -f /usr/lib/librknnrt.so ]; then
    VER=$(python3 -c "
data=open('/usr/lib/librknnrt.so','rb').read()
import re
m=re.search(rb'librknnrt version: ([0-9.]+)', data)
print(m.group(1).decode() if m else 'unknown')")
    res PASS "librknnrt.so" "version $VER"
else
    res WARN "librknnrt.so" "缺失，只能用 ONNX CPU 后端"
fi

echo "== 2. 模型与后端 =="
[ -f "$APP_DIR/models/yolov8n-pose-rknn.rknn" ] && res PASS "RKNN 模型" "$(du -h "$APP_DIR/models/yolov8n-pose-rknn.rknn" | cut -f1)" || res FAIL "RKNN 模型" "缺失"
[ -f "$APP_DIR/models/yolov8n-pose.onnx" ] && res PASS "ONNX 备援模型" "$(du -h "$APP_DIR/models/yolov8n-pose.onnx" | cut -f1)" || res WARN "ONNX 备援模型" "缺失（NPU 挂了就没退路）"

python3 - <<'PY'
import sys, time
sys.path.insert(0, "/opt/posture-monitor")
import numpy as np
from posturemon import backends, config
cfg = config.Config.load()
try:
    be, desc = backends.make_backend(cfg)
except Exception as e:
    print(f"  BACKEND_FAIL {e}"); sys.exit(1)
img = np.random.randint(0, 255, (be.in_h, be.in_w, 3), dtype=np.uint8)
be.infer(img)
ts = []
for _ in range(5):
    t = time.perf_counter(); be.infer(img); ts.append((time.perf_counter() - t) * 1000)
ts.sort()
print(f"  BACKEND {desc} input={be.in_h}x{be.in_w} sdk={be.sdk} median={ts[2]:.1f}ms")
PY
if [ $? -eq 0 ]; then res PASS "推理后端可加载" "见上"; else res FAIL "推理后端可加载" "后端初始化/推理失败"; fi

echo "== 3. 坐姿判定规则（合成关键点自检）=="
PYTHONPATH=$APP_DIR python3 -m posturemon.headless --selftest-rules >/tmp/pm_rules.txt 2>&1
if [ $? -eq 0 ]; then
    res PASS "6 条规则状态机" "$(grep -c PASS /tmp/pm_rules.txt) 项通过"
else
    res FAIL "6 条规则状态机" "见 /tmp/pm_rules.txt"
    sed -n '1,20p' /tmp/pm_rules.txt
fi

echo "== 4. 摄像头 =="
# UVC 同一时刻只允许一个进程打开。应用在跑时就由 status.json 佐证（fps/latency/persons），
# 不再重复抢占设备 —— 否则一定报"打不开"，那是假故障。
if [ "$SKIP_CAM" = 1 ]; then
    res WARN "摄像头" "已按 --no-cam 跳过"
elif [ "$APP_RUNNING" = 1 ] && [ -s "$STATUS" ]; then
    DEV=$(python3 -c "
import sys; sys.path.insert(0,'/opt/posture-monitor')
from posturemon.camera import list_cameras
d=list_cameras(); print(d[0] if d else '')")
    [ -n "$DEV" ] && res PASS "摄像头设备" "$DEV" || res FAIL "摄像头设备" "没有 /dev/v4l/by-id/*video-index0"
    python3 - <<'PY2'
import json, os, sys, time
st = json.load(open("/run/posture-monitor/status.json"))
age = time.time() - st.get("ts", 0)
fps = st.get("fps") or 0
lat = st.get("latency_ms") or 0
ok = age < 5 and fps > 5 and lat > 0
print(("PASS " if ok else "FAIL ") +
      f"应用独占中：fps={fps} 推理={lat}ms 人员={st.get('persons')} "
      f"状态={'在座' if st.get('present') else '无人'} 快照{age:.1f}s前")
sys.exit(0 if ok else 1)
PY2
    if [ $? -eq 0 ]; then
        res PASS "摄像头出帧（应用内）" "$(python3 -c "
import json;st=json.load(open('$STATUS'));print(f\"fps={st['fps']} 推理={st['latency_ms']}ms\")" 2>/dev/null)"
    else
        res FAIL "摄像头出帧（应用内）" "status.json 过期或 fps=0（见 $STATUS）"
    fi
else
    DEV=$(python3 -c "
import sys; sys.path.insert(0,'/opt/posture-monitor')
from posturemon.camera import list_cameras
d=list_cameras(); print(d[0] if d else '')")
    if [ -z "$DEV" ]; then
        res FAIL "摄像头设备" "没有 /dev/v4l/by-id/*video-index0"
    else
        res PASS "摄像头设备" "$DEV"
        OUT=$(DEV="$DEV" python3 - <<'PY3'
import os, sys, time
sys.path.insert(0, "/opt/posture-monitor")
import cv2
dev = os.environ["DEV"]
cap = cv2.VideoCapture(dev, cv2.CAP_V4L2)
if not cap.isOpened():
    print("FAIL 打不开（可能被其它进程独占）"); sys.exit(0)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640); cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
ts = []
for i in range(10):
    t = time.perf_counter(); ok, f = cap.read()
    if ok and i >= 2:
        ts.append((time.perf_counter() - t) * 1000)
cap.release()
if not ts:
    print("FAIL 读不到帧"); sys.exit(0)
ts.sort()
print(f"PASS {f.shape[1]}x{f.shape[0]} median={ts[len(ts)//2]:.0f}ms")
PY3
)
        case "$OUT" in
            PASS*) res PASS "摄像头出帧" "${OUT#PASS }" ;;
            *)     res FAIL "摄像头出帧" "${OUT#FAIL }" ;;
        esac
    fi
fi

echo "== 5. 端到端（真实推理 + 判定）=="
if [ "$SKIP_CAM" = 1 ]; then
    res WARN "端到端" "跳过"
elif [ "$APP_RUNNING" = 1 ] && [ -s "$STATUS" ]; then
    python3 - <<'PY4'
import json, sys
st = json.load(open("/run/posture-monitor/status.json"))
ok = st.get("backend", "").startswith("rknn") or st.get("backend", "").startswith("onnx")
print(("PASS " if ok else "FAIL ") +
      f"{st['backend']} | 推理 {st['latency_ms']}ms | 人员={st['persons']} | "
      f"在座={st['present']} | 不良={st['bad']}{('('+st['label']+')') if st['label'] else ''} | "
      f"今日不良={st['today_bad_s']}s")
sys.exit(0 if ok else 1)
PY4
    [ $? -eq 0 ] && res PASS "端到端推理（应用内）" "见上" || res FAIL "端到端推理（应用内）" "见上"
else
    PYTHONPATH=$APP_DIR timeout 40 python3 -m posturemon.headless --seconds 6 --quiet >/tmp/pm_e2e.txt 2>&1
    if [ $? -eq 0 ]; then
        python3 - <<'PY5'
import json
d = json.load(open("/tmp/pm_e2e.txt"))
print(f"PASS {d['backend']} | 推理 {d['infer_count']} 次 | {d['latency_ms']}ms | 人员={d['persons']}")
PY5
        res PASS "端到端推理" "$(tail -1 /tmp/pm_e2e.txt | cut -c1-60)"
    else
        res FAIL "端到端推理" "见 /tmp/pm_e2e.txt"
        tail -6 /tmp/pm_e2e.txt
    fi
fi

echo "== 6. 安装状态 =="
[ -d "$APP_DIR/posturemon" ] && res PASS "代码目录" "$APP_DIR/posturemon" || res FAIL "代码目录" "缺失"
[ -f "$CONF" ] && res PASS "配置文件" "$CONF" || res WARN "配置文件" "缺失（用内置默认值）"
DB_PATH=$(python3 -c "
import json
try: print(json.load(open('$CONF'))['db_path'])
except Exception: print('$DATA_DIR/posture.db')" 2>/dev/null)
if [ -f "$DB_PATH" ]; then
    N=$(python3 -c "
import sqlite3
c=sqlite3.connect('$DB_PATH')
print(c.execute('select count(*) from violation').fetchone()[0])" 2>/dev/null || echo 0)
    res PASS "统计数据库" "$DB_PATH（违规记录 $N 条）"
else
    res WARN "统计数据库" "还没有（首次运行后生成）"
fi

AUTOSTART=$(ls /home/*/.config/autostart/posture-monitor.desktop 2>/dev/null | head -1)
[ -n "$AUTOSTART" ] && res PASS "开机自启" "$AUTOSTART" || res WARN "开机自启" "未注册"
grep -q "AutomaticLoginEnable=true" /etc/gdm3/daemon.conf 2>/dev/null \
    && res PASS "GDM 自动登录" "$(grep AutomaticLogin= /etc/gdm3/daemon.conf | head -1)" \
    || res WARN "GDM 自动登录" "未开启（重启后停在登录页）"

if pgrep -f "posturemon" >/dev/null 2>&1; then
    res PASS "进程运行中" "pid $(pgrep -f 'python3 -m posturemon' | tr '\n' ' ')"
else
    res WARN "进程运行中" "没在跑（登录桌面后自动起，或手动 $APP_DIR/../run.sh）"
fi

echo
echo "================ 结果：PASS=$PASS  FAIL=$FAIL  WARN=$WARN ================"
[ "$FAIL" -eq 0 ] || exit 1
