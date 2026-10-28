#!/bin/bash
# screenshot.sh — 抓一张真实屏幕画面（GNOME Wayland 下唯一可行方案：ffmpeg kmsgrab）
#
# grim / gnome-screenshot / xwd 在 Mutter 上都不行（见 Jellyfin_for_M2 docs/17）：
#   · grim 依赖 wlr-screencopy → Mutter 没实现
#   · org.gnome.Shell.Screenshot → 需要交互授权，AccessDenied
#   · x11grab → 只能抓 Xwayland 窗口，抓不到合成后的整屏
#
# 用法： sudo bash scripts/screenshot.sh [out.png] [WxH]
set -uo pipefail
OUT="${1:-/tmp/posture-$(date +%H%M%S).png}"
FORCE="${2:-}"

drv_size() {
    ffmpeg -hide_banner -f kmsgrab -i - -frames:v 1 -f null - 2>&1 \
        | sed -n 's/.*wrapped_avframe, drm_prime, \([0-9]\{3,4\}x[0-9]\{3,4\}\).*/\1/p' | head -1
}
SIZE="$FORCE"
[ -n "$SIZE" ] || SIZE=$(drv_size)
if [ -z "$SIZE" ]; then
    for m in /sys/class/drm/card*/modes; do
        [ -s "$m" ] && { SIZE=$(head -1 "$m"); break; }
    done
fi
[ -n "$SIZE" ] || { echo "无法确定分辨率" >&2; exit 1; }

RAW=$(mktemp /tmp/kmsgrab.XXXXXX.raw)
trap 'rm -f "$RAW"' EXIT
W=${SIZE%x*}; H=${SIZE#*x}
_bpp() { case "$1" in nv12|yuv420p|nv21) echo 1.5 ;; *) echo 4 ;; esac; }

GRABBED=0
for f in x2rgb10 bgr0 nv12 yuv420p; do
    if ffmpeg -hide_banner -loglevel error -f kmsgrab -i - -frames:v 1 \
         -vf "hwdownload,format=$f" -pix_fmt "$f" -f rawvideo -y "$RAW" 2>/dev/null \
       && [ "$(stat -c%s "$RAW" 2>/dev/null || echo 0)" -eq \
            "$(awk -v w="$W" -v h="$H" -v b="$(_bpp $f)" 'BEGIN{printf "%d", w*h*b}')" ]; then
        PIXFMT=$f; GRABBED=1; break
    fi
done
[ "$GRABBED" = 1 ] || { echo "kmsgrab 抓帧失败" >&2; exit 1; }
echo "格式 $PIXFMT ($SIZE)" >&2
ffmpeg -hide_banner -loglevel error -f rawvideo -pix_fmt "$PIXFMT" -s "$SIZE" \
    -i "$RAW" -frames:v 1 -pix_fmt rgb24 -y "$OUT" || { echo "转 PNG 失败" >&2; exit 1; }
echo "$OUT ($(stat -c%s "$OUT") 字节)"
