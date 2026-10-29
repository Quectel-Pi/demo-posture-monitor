#!/bin/sh
# 喇叭/音频链路自检 —— 逐层确认「软件通不通」和「硬件响不响」
#
# 背景（踩过的坑，2026-10-29）：
#   1. adb 默认是 root，pactl/paplay 连不上 PipeWire → "Connection refused / Host is down"
#      必须 su 到桌面用户（quectel）并设 XDG_RUNTIME_DIR=/run/user/1000
#   2. amixer 调 DACL/DACR 无效：WirePlumber 按
#      /usr/share/alsa-card-profile/mixer/paths/analog-output-es8389-m2.conf
#      在每次播放时把 DAC 强制写回 179（-6dB）
#      → 实际音量要用 PipeWire sink 音量（wpctl / pactl）
#   3. 板载 MIC 采集音量默认 8（-91.5dB），等于没开 → 自检录不到声音要先把 ADCL/ADCR 调高
#
# 用法（板端，root 或普通用户都行）：
#   sh /opt/posture-guard/scripts/speaker_check.sh
set -u

say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

say "1. 声卡与 PCM 设备"
cat /proc/asound/cards
echo "--- 播放设备 ---"
aplay -l | sed -n '1,20p'

say "2. 关键开关（Speaker 必须 on，PA 才会使能）"
for n in 57 58 59 60; do
  printf 'numid=%-3s ' "$n"
  amixer -c 0 cget numid=$n 2>/dev/null | grep -E "name=|values=" | tr '\n' ' '
  echo
done
echo "--- 功放使能脚（播放时应为 hi）---"
grep -E "spk-con|hp-det" /sys/kernel/debug/gpio 2>/dev/null

say "3. DAC 音量（WirePlumber 会强制成 179 = -6dB）"
printf '0x46='; sed -n '71p' /sys/kernel/debug/regmap/2-0010/registers 2>/dev/null | awk '{print $2}'
printf '0x47='; sed -n '72p' /sys/kernel/debug/regmap/2-0010/registers 2>/dev/null | awk '{print $2}'
echo "（0xB3=179=-6dB 是出厂默认；WirePlumber 按 path 配置钉死）"

say "4. PipeWire（必须用桌面用户，不是 root）"
su quectel -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/1000 wpctl status" 2>/dev/null \
  | sed -n '/Sinks/,/Sources/p' | head -6

say "5. 用官方推荐方式播放（paplay → PipeWire → ALSA）"
T=/tmp/spk_check.wav
python3 - "$T" <<'PY'
import math, struct, sys, wave
n = 48000 * 3
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
buf = bytearray()
for i in range(n):
    env = min(1.0, i / 2400.0, (n - i) / 2400.0)
    v = int(math.sin(2 * math.pi * 440 * i / 48000.0) * env * 0.5 * 32767)
    buf += struct.pack('<hh', v, v)      # 注意：L=R 是标准立体声，不是反相
w.writeframes(bytes(buf)); w.close()
PY
chmod 666 "$T"
echo "把 sink 音量开到 85%，播 3 秒 440Hz…"
su quectel -s /bin/sh -c "
  export XDG_RUNTIME_DIR=/run/user/1000
  SINK=\$(pactl list short sinks | awk '\$2 ~ /es8389/ {print \$2; exit}')
  [ -n \"\$SINK\" ] && pactl set-sink-volume \"\$SINK\" 85% && pactl set-sink-mute \"\$SINK\" 0
  paplay \"$T\"
" 2>&1 | grep -v cookie

say "6. 绕过 PipeWire 直连 ALSA（排除 PipeWire 因素）"
aplay -D hw:0,0 "$T" 2>&1 | tail -2

cat <<'TIP'

结论怎么读：
  · 第 5 步和 6 步都听不到 → 硬件链路问题，重点查下面两点
      ① 喇叭必须接在 J0901 的 1 脚(VOP) 和 2 脚(VON) 之间
         3/4 脚是 GND。BTL 差分输出的一端接地 = 功放被保护，只有微弱失真声
      ② AW8010 差分输入 IN+/IN- 分别来自 ES8389 的 LINEOUT_P/N（pin16/pin15）
         用万用表量 U0903：B1/B2 = PA_VDD_5V(5V)、C2 = SHDN 播放时应为高
  · 第 5 步有声、第 6 步没声 → PipeWire 路由问题
  · 第 6 步有声、第 5 步没声 → sink 音量/静音，用 pactl 调
TIP
