#!/bin/sh
# 把喇叭音量调到「听得清」并固化（幂等，可重复执行）
#
# 为什么需要单独一个脚本：
#   1. DAC 音量不是随手 amixer 改就行的 —— WirePlumber 按 ALSA path 配置
#      在每次播放时把 DACL/DACR 强制写回配置值，amixer 的改动会被覆盖。
#      所以要改的是 /usr/share/alsa-card-profile/mixer/paths/analog-output-es8389-m2.conf
#   2. 出厂值 179 = -6dB，配一般小喇叭听不清；我们实测 205 ≈ +1dB 才够
#   3. 语音提示的 wav 峰值虽然高，但 RMS 只有 ~11%，听感比满刻度正弦低 9dB，
#      需要对素材做响度归一化（仓库里的 assets/voice 已经处理过）
#
# 用法（板端，root 执行）：
#   sh scripts/tune_speaker.sh [DAC值]
#   默认 205；想更响用 215，想还原出厂用 179
set -eu

VAL="${1:-205}"
CONF=/usr/share/alsa-card-profile/mixer/paths/analog-output-es8389-m2.conf
VOICE_DIR="${VOICE_DIR:-/opt/posture-guard/assets/voice}"

[ -f "$CONF" ] || { echo "!! 找不到 $CONF（不是 M2 板？）" >&2; exit 1; }

cp -n "$CONF" "${CONF}.orig" 2>/dev/null || true
sed -i "s/^volume = [0-9]*$/volume = ${VAL}/" "$CONF"
echo "已写入 $CONF:"
grep -A1 -E '^\[Element DAC[LR]\]' "$CONF"

# 顺手把 ALSA 层的值也设一次（对不走 WirePlumber 的直连播放生效）
amixer -c 0 sset DACL "$VAL" >/dev/null 2>&1 || true
amixer -c 0 sset DACR "$VAL" >/dev/null 2>&1 || true
amixer -c 0 cset numid=60 on >/dev/null 2>&1 || true   # Speaker DAPM 上电（使能 PA）

# 让 WirePlumber 重新读配置（必须用桌面用户）
if [ -d /run/user/1000 ]; then
  su quectel -s /bin/sh -c \
    "XDG_RUNTIME_DIR=/run/user/1000 systemctl --user restart wireplumber.service" \
    2>/dev/null || true
  sleep 2
  su quectel -s /bin/sh -c '
    export XDG_RUNTIME_DIR=/run/user/1000
    SINK=$(pactl list short sinks 2>/dev/null | awk "\$2 ~ /es8389/ {print \$2; exit}")
    [ -n "$SINK" ] && pactl set-sink-volume "$SINK" 100% && pactl set-sink-mute "$SINK" 0 \
      && echo "sink $SINK -> 100%"' 2>/dev/null
fi

# 试听
if [ -f "$VOICE_DIR/head_down.wav" ]; then
  echo "试听一句提示语音…"
  if [ -d /run/user/1000 ]; then
    su quectel -s /bin/sh -c \
      "XDG_RUNTIME_DIR=/run/user/1000 paplay $VOICE_DIR/head_down.wav" 2>/dev/null || true
  fi
fi

cat <<TIP

完成。若还是偏轻/偏响，重跑：sh $0 <179~220>
  · 179 = 出厂 -6dB
  · 205 = 约 +1dB（实测「够大听得清」）
  · 220 = 约 +8.5dB（可能削顶）
排障看：sh $(dirname "$0")/speaker_check.sh
TIP
