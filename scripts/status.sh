#!/usr/bin/env bash
# 从状态 socket 读一帧状态，人肉可读地打出来
# 用法：bash scripts/status.sh [--raw] [--watch]
SOCKET="${SOCKET:-/run/posture-guard/status.sock}"
RAW=0; WATCH=0
for a in "$@"; do
  [[ "$a" == "--raw" ]] && RAW=1
  [[ "$a" == "--watch" ]] && WATCH=1
done
read_one() {
  python3 - "$SOCKET" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(4)
s.connect(sys.argv[1])
buf = b""
while b"\n" not in buf:
    c = s.recv(8192)
    if not c:
        break
    buf += c
line = buf.split(b"\n")[0]
if not line:
    print("（没有数据）"); sys.exit(1)
print(line.decode())
PY
}
render() {
  python3 - <<'PY'
import json, sys
d = json.load(sys.stdin)
w = d.get("frame_width"); h = d.get("frame_height")
print(f"后端      : {d['backend']} / {d['runtime_version']}")
print(f"窗口      : {w}x{h}   设备: {d.get('device_id','')}")
print(f"姿态      : {d['posture_state']}   {d['posture_state_zh']}   {d['explanation']}")
print(f"在座/人数 : {d['present']} / {d['persons']}   fps={d['fps']}  frame_id={d['frame_id']}")
m = {k: d[k] for k in ("shoulder_angle_deg","head_offset_y","head_axis_deg","width_ratio","wrist_to_nose")}
print("指标      : " + " ".join(f"{k}={v}" for k,v in m.items()) + f"  (valid={d['metrics_valid']})")
print(f"校准      : baseline={d['has_baseline']} calibrating={d['calibrating']} "
      f"{d['calibration_progress']:.0f}%  ({d.get('baseline_created_at','')})")
print(f"用眼      : {d['session_state']}  已用 {int(d['use_s'])}s  剩余 {int(d['remaining_s'])}s")
print(f"今日      : 在座 {int(d['today_use_s'])}s  不良 {int(d['today_bad_s'])}s  提醒 {d['today_alerts']} 次")
print(f"规则      : {', '.join(d['active_rules']) or '（无）'}"
      + (f"   免打扰中" if d['snoozed'] else ""))
if d.get("last_notification"):
    print(f"最近提醒  : {d['last_notification']} ({int(d['last_notification_ago_s'])}s 前)")
PY
}
if [[ $WATCH -eq 1 ]]; then
  while true; do
    out="$(read_one)" || exit 1
    clear 2>/dev/null || true
    echo "===== $(date '+%F %T') ====="
    echo "$out" | render
    echo "（Ctrl+C 退出）"
    sleep 1
  done
else
  out="$(read_one)" || { echo "读不到状态：服务没跑或 socket 路径不对（SOCKET=$SOCKET）" >&2; exit 1; }
  if [[ $RAW -eq 1 ]]; then echo "$out"; else echo "$out" | render; fi
fi
