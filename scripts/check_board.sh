#!/usr/bin/env bash
# 板端环境体检（对应实施方案 §7.1 - §7.4 的检查清单）
#
# 用法：sudo bash scripts/check_board.sh [--json]
# 退出码：0 = 关键项都满足；1 = 有阻塞项
set -uo pipefail

JSON=0
[[ "${1:-}" == "--json" ]] && JSON=1

ok=0; warn=0; fail=0
declare -a LINES=()

add() { # add <LEVEL> <名称> <详情>
  local level="$1" name="$2" detail="$3"
  case "$level" in
    PASS) ok=$((ok+1));;
    WARN) warn=$((warn+1));;
    FAIL) fail=$((fail+1));;
  esac
  LINES+=("${level}|${name}|${detail}")
  if [[ $JSON -eq 0 ]]; then
    printf '  [%-4s] %-26s %s\n' "$level" "$name" "$detail"
  fi
}

if [[ $JSON -eq 0 ]]; then echo "=== RK3576 板端环境体检 ==="; fi

# --- 1. OS / 架构 ---
arch="$(uname -m)"; bits="$(getconf LONG_BIT 2>/dev/null || echo '?')"
if [[ "$arch" == "aarch64" && "$bits" == "64" ]]; then
  add PASS "架构" "$arch / ${bits}bit"
else
  add FAIL "架构" "$arch / ${bits}bit（需要 aarch64 64 位用户空间）"
fi
add PASS "内核" "$(uname -r)"
add PASS "发行版" "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"

# --- 2. 编译器（板端原生编译，避免交叉编译 sysroot 坑）---
if command -v g++ >/dev/null && command -v cmake >/dev/null; then
  add PASS "编译工具链" "g++ $(g++ -dumpversion), cmake $(cmake --version | head -1 | awk '{print $3}')"
else
  add FAIL "编译工具链" "缺少 g++/cmake：先跑 scripts/fetch_deps.sh 在主机侧取 deb，再板端 dpkg -i"
fi

# --- 3. OpenCV ---
if pkg-config --exists opencv4 2>/dev/null; then
  add PASS "OpenCV" "$(pkg-config --modversion opencv4)"
else
  add WARN "OpenCV" "pkg-config 找不到 opencv4（headers 缺失会编译失败）"
fi

# --- 4. NPU ---
rknn_lib="$(ldconfig -p 2>/dev/null | awk '/librknnrt\.so/{print $NF; exit}')"
if [[ -n "$rknn_lib" ]]; then
  add PASS "librknnrt" "$rknn_lib"
else
  add FAIL "librknnrt" "找不到 librknnrt.so（NPU 推理不可用）"
fi
if [[ -e /dev/rknpu ]]; then
  add PASS "rknpu 设备节点" "/dev/rknpu"
else
  add WARN "rknpu 设备节点" "/dev/rknpu 不存在（驱动可能编进内核，先用 demo 实测）"
fi
drv="$(dmesg 2>/dev/null | grep -oiE 'rknpu[^ ]* [0-9]+\.[0-9]+\.[0-9]+' | tail -1)"
[[ -n "$drv" ]] && add PASS "rknpu 驱动" "$drv" || add WARN "rknpu 驱动" "dmesg 里没读到版本（需 root）"

# --- 5. 模型与头文件 ---
proj="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 源码树、安装目录、部署源码包三处任一处有就行（编译时才真正需要）
hdr=""
for cand in "$proj/vendor/rknn/rknn_api.h" /opt/posture-guard/vendor/rknn/rknn_api.h \
            /userdata/posture-src/vendor/rknn/rknn_api.h /userdata/build-src/vendor/rknn/rknn_api.h \
            /usr/include/rknn_api.h; do
  [[ -f "$cand" ]] && { hdr="$cand"; break; }
done
if [[ -n "$hdr" ]]; then
  add PASS "rknn_api.h" "$hdr"
elif [[ -x /opt/posture-guard/bin/posture_guard ]]; then
  add PASS "rknn_api.h" "已编译部署（头文件只在编译时需要）"
else
  add FAIL "rknn_api.h" "缺少 vendor/rknn/rknn_api.h（跑 scripts/fetch_deps.sh）"
fi
model="/opt/posture-guard/models/yolov8n-pose.rknn"
[[ -f "$model" ]] || model="$proj/models/yolov8n-pose.rknn"
if [[ -f "$model" ]]; then
  add PASS "pose 模型" "$(basename "$model") $(du -h "$model" | cut -f1) sha256=$(sha256sum "$model" | cut -c1-16)"
else
  add FAIL "pose 模型" "找不到 yolov8n-pose.rknn"
fi

# --- 6. 摄像头 ---
camera=""
for dev in /dev/v4l/by-id/*-video-index0 /dev/video*; do
  [[ -e "$dev" ]] || continue
  name="$(v4l2-ctl -d "$dev" --info 2>/dev/null | sed -n 's/.*Card type *: //p')"
  if [[ "$name" == *icSpring* || "$name" == *USB* || "$name" == *Camera* || "$name" == *camera* ]]; then
    camera="$dev"; break
  fi
done
if [[ -n "$camera" ]]; then
  add PASS "USB UVC 摄像头" "$camera ($(v4l2-ctl -d "$camera" --info 2>/dev/null | sed -n 's/.*Card type *: //p'))"
  fmts="$(v4l2-ctl -d "$camera" --list-formats-ext 2>/dev/null | grep -cE "Size: Discrete")"
  add PASS "可用分辨率档数" "$fmts"
else
  add FAIL "USB UVC 摄像头" "没有找到 USB 摄像头节点"
fi

# --- 7. 部署产物 ---
[[ -x /opt/posture-guard/bin/posture_guard ]] && add PASS "可执行文件" "/opt/posture-guard/bin/posture_guard" \
  || add WARN "可执行文件" "还没部署（scripts/deploy.sh）"
[[ -f /etc/posture-guard/posture.yaml ]] && add PASS "配置文件" "/etc/posture-guard/posture.yaml" \
  || add WARN "配置文件" "还没部署"
id posture >/dev/null 2>&1 && add PASS "服务账号" "posture (uid=$(id -u posture))" \
  || add WARN "服务账号" "posture 用户不存在（deploy.sh 会创建）"

# --- 8. 显示会话（UI 需要，但后台服务不需要）---
sess="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$3!="-" && $4=="seat0"{print $3; exit}')"
if [[ -n "$sess" ]]; then
  add PASS "图形会话" "用户 $sess 已在 seat0 登录"
else
  add WARN "图形会话" "当前没有图形会话（后台服务不受影响，但有屏提醒需要）"
fi

if [[ $JSON -eq 1 ]]; then
  python3 - "$@" <<'PY' 2>/dev/null || true
PY
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
  echo "==== 汇总：PASS=$ok  WARN=$warn  FAIL=$fail ===="
fi
[[ $fail -eq 0 ]]
