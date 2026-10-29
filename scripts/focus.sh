#!/usr/bin/env bash
# 电动对焦摄像头的焦距标定（对应 docs/calibration.md 的「焦距」一节）
#
# 适用：支持 V4L2_CID_FOCUS_ABSOLUTE 的 UVC 摄像头（如本项目这块 UGREEN 1080P）。
# 定焦摄像头没有这个控件，脚本会直接告诉你「不需要调」。
#
# 用法（板端，需要 root 或 video 组）：
#   bash scripts/focus.sh --info            # 看当前焦距/是否有电动对焦
#   bash scripts/focus.sh --scan            # 扫描找最佳焦距（人需坐在常规位置不动）
#   bash scripts/focus.sh --set 70          # 锁定到某个值
#   bash scripts/focus.sh --af              # 恢复连续自动对焦
#   bash scripts/focus.sh --measure         # 只测当前清晰度（不改变焦距）
#
# 为什么需要这个脚本（实测结论）：
#   * 连续自动对焦开着时，镜头每帧都在微调 → 画面持续轻微失焦，
#     实测同一位置的清晰度只有锁死时的 1/8；
#   * 焦距值上限之外会彻底散焦（本项目 40cm 场景：20~120 都清晰，
#     160 已经明显变糊，250 以上基本全糊）。
#   所以固定工位场景应关掉 AF，锁一个经过实测的值。
set -uo pipefail

DEV="${DEV:-/dev/video73}"
W="${W:-1280}"
H="${H:-720}"
MODE="${1:---info}"
ARG="${2:-}"

need_v4l2() { command -v v4l2-ctl >/dev/null || { echo "!! 缺 v4l2-ctl：apt install v4l-utils" >&2; exit 1; }; }
need_v4l2

info() {
  echo "== 设备 $DEV =="
  v4l2-ctl -d "$DEV" --info 2>/dev/null | sed -n 's/^\(.*Card type.*\)/  \1/p'
  if v4l2-ctl -d "$DEV" --list-ctrls 2>/dev/null | grep -q focus_absolute; then
    echo "  ✔ 支持电动对焦："
    v4l2-ctl -d "$DEV" --list-ctrls 2>/dev/null | grep -E "focus" | sed 's/^/    /'
    echo "  当前值："
    v4l2-ctl -d "$DEV" --get-ctrl focus_automatic_continuous,focus_absolute 2>&1 | sed 's/^/    /'
  else
    echo "  ✘ 没有 focus_absolute 控件 → 这是定焦摄像头，软件调不了焦距。"
    echo "    若是固定工位仍不清晰，只能：① 挪近/挪远整体机位 ② 手拧镜头前端的对焦环（部分型号有）"
  fi
}

set_ctrl() { v4l2-ctl -d "$DEV" --set-ctrl "$1" 2>&1; }

measure() {
  # 输出一帧的清晰度（中心区域 Laplacian 方差），不改变焦距
  local tmp f
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN
  ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
    -video_size "${W}x${H}" -i "$DEV" -frames:v 1 -y "$tmp/f.jpg" 2>/dev/null || {
      echo "抓帧失败（摄像头被占用？先 systemctl stop posture-guard）" >&2; return 1; }
  python3 - "$tmp/f.jpg" <<'PY'
import sys
try:
    import cv2
except Exception:
    print("缺 python3-opencv"); raise SystemExit(1)
im = cv2.imread(sys.argv[1], cv2.IMREAD_GRAYSCALE)
if im is None:
    print("读不到帧"); raise SystemExit(1)
h, w = im.shape
c = im[int(h*0.15):int(h*0.95), int(w*0.20):int(w*0.90)]
print("%.1f" % cv2.Laplacian(c, cv2.CV_64F).var())
PY
}

scan() {
  cat > /tmp/.focus_scan.py <<'PY'
import os, subprocess, tempfile, time, shutil
import cv2, numpy as np
DEV = os.environ.get("DEV", "/dev/video73")
W = int(os.environ.get("W", "1280")); H = int(os.environ.get("H", "720"))
VALUES = [0, 20, 40, 60, 80, 100, 120, 140, 160, 200, 250, 320, 400, 600, 800, 1023]
ROUNDS = 3; NF = 8

def ctl(**kw):
    a = ["v4l2-ctl", "-d", DEV]
    for k, v in kw.items(): a += ["--set-ctrl", f"{k}={v}"]
    subprocess.run(a, capture_output=True)

def grab(n=NF):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg","-hide_banner","-loglevel","error","-f","v4l2",
                    "-input_format","mjpeg","-video_size",f"{W}x{H}","-i",DEV,
                    "-frames:v",str(n),"-y",os.path.join(d,"f%02d.jpg")], capture_output=True)
    out = []
    for i in range(1, n+1):
        p = os.path.join(d, f"f{i:02d}.jpg")
        if os.path.exists(p):
            im = cv2.imread(p, cv2.IMREAD_GRAYSCALE)
            if im is not None: out.append((p, im))
    return out

def sharp(im):
    h, w = im.shape
    c = im[int(h*0.15):int(h*0.95), int(w*0.20):int(w*0.90)]
    return float(cv2.Laplacian(c, cv2.CV_64F).var())

print("关掉连续自动对焦…")
ctl(focus_automatic_continuous=0); time.sleep(0.5)
print("请保持常规坐姿、不要大幅移动（每档要抓几帧）\n")

best = {v: -1.0 for v in VALUES}
for r in range(ROUNDS):
    print(f"--- 第 {r+1}/{ROUNDS} 轮 ---")
    for v in VALUES:
        ctl(focus_absolute=v); time.sleep(0.45)
        frames = grab()
        if not frames: continue
        vals = [sharp(im) for _, im in frames]
        mx = float(np.max(vals))
        if mx > best[v]: best[v] = mx
        bar = "#" * int(min(mx, 800)/25)
        print(f"  focus={v:4d}  最优清晰度={mx:7.1f}  {bar}")

print("\n=== 排序 ===")
for v in sorted(VALUES, key=lambda x: -best[x]):
    print(f"  focus={v:4d}  {best[v]:8.1f}")

top = max(VALUES, key=lambda x: best[x])
# 取「达到最高值 85% 以上」的连续区间，中心值更稳（留出距离变化余量）
good = [v for v in sorted(VALUES) if best[v] >= best[top]*0.85]
if good:
    mid = good[len(good)//2]
else:
    mid = top
print(f"\n最佳值 = {top}（{best[top]:.1f}）")
print(f"够清晰的区间 = {good}")
print(f"建议锁定 = {mid}（区间中间，前后留余量）")
PY
  DEV="$DEV" W="$W" H="$H" python3 /tmp/.focus_scan.py
}

case "$MODE" in
  --info) info;;
  --measure)
    v="$(measure)"; echo "当前清晰度 = ${v}（越大越清晰；同一机位下比较才有意义）";;
  --set)
    [[ -n "$ARG" ]] || { echo "用法: $0 --set <0-1023>" >&2; exit 1; }
    set_ctrl focus_automatic_continuous=0
    set_ctrl "focus_absolute=$ARG"
    sleep 0.5
    v4l2-ctl -d "$DEV" --get-ctrl focus_automatic_continuous,focus_absolute
    echo "清晰度 = $(measure)"
    echo
    echo "要持久生效，把这两个值写进 /etc/posture-guard/posture.yaml："
    echo "  camera:"
    echo "    focus_autofocus: false"
    echo "    focus_absolute: $ARG"
    echo "然后 sudo systemctl restart posture-guard"
    ;;
  --af)
    set_ctrl focus_automatic_continuous=1
    v4l2-ctl -d "$DEV" --get-ctrl focus_automatic_continuous,focus_absolute
    echo "已恢复连续自动对焦（注意：会导致画面持续轻微失焦，不建议固定工位使用）";;
  --scan) scan;;
  -h|--help)
    sed -n '2,20p' "$0";;
  *) echo "未知参数 $MODE，用 --help" >&2; exit 1;;
esac
