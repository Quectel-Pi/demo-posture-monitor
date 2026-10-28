#!/usr/bin/env bash
# 免打扰 / 确认休息（对应实施方案 §5.3 的用户控制）
#   bash scripts/snooze.sh 15      # 忽略提醒 15 分钟
#   bash scripts/snooze.sh --clear # 立刻解除
#   bash scripts/snooze.sh --ack   # 点了「已休息」，开始新一轮 20 分钟
set -euo pipefail
SOCKET="${SOCKET:-/run/posture-guard/status.sock}"
case "${1:---clear}" in
  --clear) cmd='{"cmd":"clear_snooze"}';;
  --ack)   cmd='{"cmd":"ack"}';;
  *)       cmd="{\"cmd\":\"snooze\",\"minutes\":${1}}";;
esac
python3 - "$SOCKET" "$cmd" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(4)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\n")
print("已发送:", sys.argv[2])
PY
