#!/usr/bin/env bash
# 摄像头冒烟测试（对应实施方案 §7.3 / §16.1）
#
# 用法：
#   bash scripts/smoke_camera.sh            # 自动挑 UVC 设备，抓 5 帧统计
#   bash scripts/smoke_camera.sh /dev/video73 640 480
#
# 退出码：0 = 能稳定出帧；1 = 打不开/不出帧
set -uo pipefail

DEV="${1:-}"
WIDTH="${2:-640}"
HEIGHT="${3:-480}"
FRAMES=5

if [[ -z "$DEV" ]]; then
  echo "== 枚举视频节点 =="
  for d in /dev/v4l/by-id/*-video-index0 /dev/video*; do
    [[ -e "$d" ]] || continue
    card="$(v4l2-ctl -d "$d" --info 2>/dev/null | sed -n 's/.*Card type *: //p')"
    drv="$(v4l2-ctl -d "$d" --info 2>/dev/null | sed -n 's/.*Driver name *: //p')"
    [[ -n "$card" ]] || continue
    printf '  %-42s %s (%s)\n' "$d" "$card" "$drv"
    if [[ -z "$DEV" && ( "$card" == *icSpring* || "$card" == *USB* || "$card" == *camera* ) ]]; then
      DEV="$d"
    fi
  done
fi

if [[ -z "$DEV" || ! -e "$DEV" ]]; then
  echo "!! 没有可用的摄像头节点" >&2
  exit 1
fi
echo "== 使用设备 $DEV =="

owner="$(stat -c '%U:%G %a' "$DEV" 2>/dev/null)"
echo "   权限 $owner"
if [[ "$(id -u)" -ne 0 ]]; then
  if ! [[ -r "$DEV" && -w "$DEV" ]]; then
    echo "!! 当前用户读写不了该节点：把自己加进 video 组，或用 sudo 运行" >&2
    echo "   提示：lsof $DEV 看是否被别的进程独占（同一 UVC 同时只能一个进程打开）" >&2
    exit 1
  fi
fi

echo "== 支持的格式（前 12 行）=="
v4l2-ctl -d "$DEV" --list-formats-ext 2>/dev/null | head -12 | sed 's/^/   /'

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
echo "== 抓 $FRAMES 帧 $WIDTH x $HEIGHT =="
if ! command -v ffmpeg >/dev/null; then
  echo "!! 缺 ffmpeg，退回 v4l2-ctl 单帧抓取" >&2
  v4l2-ctl -d "$DEV" --set-fmt-video=width=$WIDTH,height=$HEIGHT \
           --stream-mmap --stream-count=$FRAMES --stream-to="$OUT/f.raw" 2>&1 | sed 's/^/   /'
  sz=$(stat -c %s "$OUT/f.raw" 2>/dev/null || echo 0)
  echo "   抓到 $sz 字节原始帧"
  [[ "$sz" -gt 1000 ]] || exit 1
  echo "== 摄像头可用 =="
  exit 0
fi

start=$(date +%s.%N)
if ffmpeg -hide_banner -loglevel error -f v4l2 -video_size "${WIDTH}x${HEIGHT}" \
      -i "$DEV" -frames:v "$FRAMES" -y "$OUT/f%02d.jpg" 2>"$OUT/err.txt"; then
  end=$(date +%s.%N)
  n=$(ls "$OUT"/f*.jpg 2>/dev/null | wc -l)
  sz=$(du -sk "$OUT" | cut -f1)
  fps=$(awk -v n="$n" -v s="$start" -v e="$end" 'BEGIN{printf "%.1f", n/(e-s)}')
  echo "   抓到 $n 帧，共 ${sz}KB，平均 ${fps} fps"
  # 用首帧做个体检：非全黑 / 非纯色
  if command -v python3 >/dev/null && python3 -c "
import sys
try:
    import cv2
except Exception:
    sys.exit(2)
im = cv2.imread('$OUT/f01.jpg')
sys.exit(0 if im is not None and im.std() > 5 else 1)
" 2>/dev/null; then
    echo "   首帧内容正常（有画面细节）"
  fi
  /usr/bin/time -f "   耗时 %e s" true 2>/dev/null || true
  echo "== 摄像头可用 =="
  exit 0
else
  echo "!! ffmpeg 抓帧失败：" >&2
  sed 's/^/   /' "$OUT/err.txt" >&2
  echo "   常见原因：分辨率不支持 / 节点不是摄像头 / 被其它进程占用 / 需要 -input_format" >&2
  exit 1
fi
