#!/usr/bin/env bash
# 部署自检：验证「服务在跑 + 真的在推理 + 状态可读 + 统计落盘」
#
# 用法：bash scripts/verify.sh [--json]
# 退出码：0 = 无 FAIL；1 = 有 FAIL
set -uo pipefail

SOCKET="${SOCKET:-/run/posture-guard/status.sock}"
SERVICE="${SERVICE:-posture-guard}"
CONF="${CONF:-/etc/posture-guard/posture.yaml}"
PREFIX="${PREFIX:-/opt/posture-guard}"
STATE="${STATE:-/var/lib/posture-guard}"
JSON=0
[[ "${1:-}" == "--json" ]] && JSON=1

ok=0; warn=0; fail=0
declare -a LINES=()
add() {
  local level="$1" name="$2" detail="$3"
  case "$level" in PASS) ok=$((ok+1));; WARN) warn=$((warn+1));; FAIL) fail=$((fail+1));; esac
  LINES+=("${level}|${name}|${detail}")
  [[ $JSON -eq 0 ]] && printf '  [%-4s] %-24s %s\n' "$level" "$name" "$detail"
  return 0
}

[[ $JSON -eq 0 ]] && echo "=== posture-guard 自检 ==="

# ---- 1. 服务状态 ----
if systemctl is-active "$SERVICE" >/dev/null 2>&1; then
  pid="$(systemctl show -p MainPID --value "$SERVICE" 2>/dev/null)"
  add PASS "服务运行中" "pid $pid"
else
  add FAIL "服务运行中" "$(systemctl is-active "$SERVICE" 2>&1)"
fi
if systemctl is-enabled "$SERVICE" >/dev/null 2>&1; then
  add PASS "开机自启" "enabled"
else
  add FAIL "开机自启" "未 enable（重启不会自己起来）"
fi

# ---- 2. 账号与权限 ----
if id posture >/dev/null 2>&1; then
  groups="$(id -nG posture)"
  if [[ "$groups" == *video* ]]; then
    add PASS "服务账号" "posture，组: $groups"
  else
    add FAIL "服务账号" "posture 不在 video 组，打不开摄像头"
  fi
else
  add FAIL "服务账号" "posture 用户不存在"
fi

# ---- 3. 配置与模型 ----
if [[ -f "$CONF" ]]; then
  if out="$("$PREFIX/bin/posture_guard" --config "$CONF" --print-config 2>&1)"; then
    add PASS "配置可解析" "$(echo "$out" | head -1)"
  else
    add FAIL "配置可解析" "$(echo "$out" | tail -2 | tr '\n' ' ')"
  fi
else
  add FAIL "配置可解析" "缺少 $CONF"
fi
model="$PREFIX/models/yolov8n-pose.rknn"
if [[ -f "$model" ]]; then
  add PASS "模型就位" "$model ($(du -h "$model" | cut -f1), sha256 $(sha256sum "$model" | cut -c1-16))"
else
  add FAIL "模型就位" "缺少 $model"
fi

# ---- 4. NPU runtime ----
# /usr/lib/librknnrt.so 通常没进 ldconfig 缓存（裸 .so 没有 soname 版本号），
# 所以既查缓存也直接找文件，否则会误报
rknn_path="$(ldconfig -p 2>/dev/null | awk '/librknnrt/{print $NF; exit}')"
[[ -n "$rknn_path" ]] || rknn_path="$(find /usr/lib /usr/local/lib /lib /opt -maxdepth 3 -name 'librknnrt.so*' 2>/dev/null | head -1)"
if [[ -n "$rknn_path" ]]; then
  add PASS "librknnrt" "$rknn_path"
else
  add FAIL "librknnrt" "系统里找不到（NPU 不可用）"
fi

read_status() {
  # 读一行「type == status」的 JSON（socket 上会混着 video 消息，必须跳过）
  timeout 8 python3 - "$SOCKET" <<'PROBE' 2>/dev/null
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(6); s.connect(sys.argv[1])
buf = b""; t0 = time.time()
while time.time() - t0 < 5.0:
    try:
        c = s.recv(65536)
    except socket.timeout:
        break
    if not c:
        break
    buf += c
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        if not line.strip():
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") == "status":
            print(json.dumps(d, ensure_ascii=False))
            sys.exit(0)
sys.exit(1)
PROBE
}

# ---- 5. 状态 socket + 内容 ----
if [[ -S "$SOCKET" ]]; then
  add PASS "状态 socket" "$SOCKET"
  status="$(read_status)"
  if [[ -n "$status" ]]; then
    add PASS "状态可读" "$(echo "$status" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["posture_state"], d["posture_state_zh"])' 2>/dev/null)"
    backend="$(echo "$status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["backend"])' 2>/dev/null)"
    [[ "$backend" == "rknn" ]] && add PASS "推理后端" "rknn(NPU)" || add FAIL "推理后端" "$backend（期望 rknn）"
    lat="$(echo "$status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["latency_ms"])' 2>/dev/null)"
    lat_i="${lat%.*}"
    if [[ -n "$lat_i" && "$lat_i" -lt 200 ]]; then
      add PASS "推理耗时" "${lat} ms/帧（NPU，目标 <200ms）"
    elif [[ -n "$lat_i" && "$lat_i" -lt 500 ]]; then
      add WARN "推理耗时" "${lat} ms/帧，偏慢（看看是否在跑 CPU 后端）"
    else
      add FAIL "推理耗时" "${lat} ms/帧，远超目标"
    fi
    fps="$(echo "$status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["fps"])' 2>/dev/null)"
    fx="${fps%.*}"; fx="${fx:-0}"
    if [[ "$fx" -ge 4 ]]; then
      add PASS "推理帧率" "${fps} fps（配置上限 camera.infer_fps）"
    else
      add WARN "推理帧率" "${fps} fps，低于 4"
    fi
    fid="$(echo "$status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["frame_id"])' 2>/dev/null)"
    sleep 1.5
    status2="$(read_status)"
    fid2="$(echo "$status2" | python3 -c 'import json,sys;print(json.load(sys.stdin)["frame_id"])' 2>/dev/null)"
    if [[ -n "$fid" && -n "$fid2" && "$fid2" -gt "$fid" ]]; then
      add PASS "出帧中" "frame_id $fid -> $fid2"
    else
      add FAIL "出帧中" "frame_id 没增长（$fid -> $fid2）：摄像头打不开或推理卡住"
    fi
    echo "$status2" > /tmp/posture-status.json
  else
    add FAIL "状态可读" "socket 存在但读不到 JSON（服务卡住？）"
  fi
else
  add FAIL "状态 socket" "$SOCKET 不存在"
fi

# ---- 5b. 视频流（界面能不能看到画面）----
vid="$(timeout 8 python3 - "$SOCKET" <<'PROBE' 2>/dev/null
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(6); s.connect(sys.argv[1])
buf = b""; n = 0; wh = (0, 0); t0 = time.time()
while time.time() - t0 < 3.0 and n < 3:
    try:
        c = s.recv(65536)
    except socket.timeout:
        break
    if not c:
        break
    buf += c
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") == "video":
            n += 1
            wh = (d.get("w", 0), d.get("h", 0))
print(n, wh[0], wh[1])
PROBE
)"
read -r vn vw vh <<< "${vid:-0 0 0}"
if [[ "${vn:-0}" -ge 2 && "${vw:-0}" -gt 0 ]]; then
  add PASS "视频流" "3 秒内收到 ${vn} 帧 ${vw}x${vh} JPEG（界面能看到画面）"
elif [[ "${vn:-0}" -ge 1 ]]; then
  add WARN "视频流" "只收到 ${vn} 帧，帧率偏低"
else
  add FAIL "视频流" "收不到视频帧：检查 ui.stream_video，或摄像头没出帧"
fi

# ---- 6. 统计与基线 ----
stats_today="$STATE/stats/stats-$(date +%F).jsonl"
if [[ -f "$stats_today" ]]; then
  add PASS "统计落盘" "$(basename "$stats_today") ($(wc -l < "$stats_today") 行)"
else
  add WARN "统计落盘" "今天的统计文件还没生成（服务刚起需要 10s）"
fi
if [[ -f "$STATE/baseline.json" ]]; then
  add PASS "校准基线" "$(python3 -c "
import json,sys
try:
    d=json.load(open('$STATE/baseline.json'))
    print('肩线基准 %.1f°, 样本 %d 帧, 建立于 %s' % (d['shoulder_angle_deg'], d['samples'], d['created_at']))
except Exception as e:
    print('解析失败:', e)
")"
else
  add WARN "校准基线" "还没有 baseline.json（有人坐在镜头前 5 秒会自动校准）"
fi

# ---- 7. 日志健康 ----
if command -v journalctl >/dev/null; then
  errs="$(journalctl -u "$SERVICE" --since '-10min' --no-pager 2>/dev/null | grep -c 'ERROR' || true)"
  if [[ "${errs:-0}" -eq 0 ]]; then
    add PASS "近 10 分钟无 ERROR" "0 条"
  else
    add WARN "近 10 分钟无 ERROR" "$errs 条，看 journalctl -u $SERVICE"
  fi
fi

# ---- 8. 单元测试（有源码目录时）----
if [[ -d "$(dirname "${BASH_SOURCE[0]}")/../build" ]]; then
  if ctest --test-dir "$(dirname "${BASH_SOURCE[0]}")/../build" >/dev/null 2>&1; then
    add PASS "单元测试" "ctest 全通过"
  else
    add FAIL "单元测试" "ctest 有失败项"
  fi
fi
if [[ -x /opt/posture-guard/ui/posture_guard_ui.py || -f /opt/posture-guard/ui/posture_guard_ui.py ]]; then
  add PASS "桌面界面" "/opt/posture-guard/ui/posture_guard_ui.py"
else
  add WARN "桌面界面" "未安装 UI"
fi

if [[ $JSON -eq 1 ]]; then
  printf '{"pass":%d,"warn":%d,"fail":%d,"items":[' "$ok" "$warn" "$fail"
  first=1
  for line in "${LINES[@]}"; do
    IFS='|' read -r level name detail <<< "$line"
    [[ $first -eq 0 ]] && printf ','
    first=0
    printf '{"level":"%s","name":"%s","detail":"%s"}' "$level" "$name" "${detail//\"/\\\"}"
  done
  printf ']}\n'
else
  echo
  echo "================ 结果：PASS=$ok  FAIL=$fail  WARN=$warn ================"
fi
[[ $fail -eq 0 ]]
