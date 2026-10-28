#!/usr/bin/env bash
# 触发一次个人校准（对应实施方案 §4.6）
#
# 用法：
#   bash scripts/calibrate.sh            # 通过 socket 让正在跑的服务进入校准
#   bash scripts/calibrate.sh --reset    # 删掉旧基准（下次有人坐下会自动重新校准）
#   bash scripts/calibrate.sh --only     # 停服务，前台跑 --calibrate-only，完成后退出
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SOCKET="${SOCKET:-/run/posture-guard/status.sock}"
PREFIX="${PREFIX:-/opt/posture-guard}"
CONF="${CONF:-/etc/posture-guard/posture.yaml}"

if [[ "${1:-}" == "--reset" ]]; then
  "${PREFIX}/bin/posture_guard" --config "$CONF" --reset-baseline
  exit $?
fi

if [[ "${1:-}" == "--only" ]]; then
  command -v g++ >/dev/null || { echo "!! 需要先编译"; exit 1; }
  systemctl stop posture-guard 2>/dev/null || true
  "${PREFIX}/bin/posture_guard" --config "$CONF" --calibrate-only 2>&1 | tail -20
  rc=$?
  systemctl start posture-guard 2>/dev/null || true
  exit $rc
fi

if [[ ! -S "$SOCKET" ]]; then
  echo "!! 没有状态 socket（服务没跑？）。也可以直接： $0 --only" >&2
  exit 1
fi
python3 - "$SOCKET" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(4)
s.connect(sys.argv[1])
s.sendall(b'{"cmd":"calibrate"}\n')
print("已请求校准：请保持正常坐姿 5 秒（看 journalctl -u posture-guard -f 确认结果）")
PY
