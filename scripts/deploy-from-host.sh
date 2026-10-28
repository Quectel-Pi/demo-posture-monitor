#!/bin/bash
# deploy-from-host.sh — 在开发机（PC）上一条命令把工程推到板子并安装。
#
#   bash scripts/deploy-from-host.sh                 # 用默认 adb 设备
#   bash scripts/deploy-from-host.sh -s <serial> --user=quectel
#
# 做三件事：
#   1) 本机准备模型（缺了就跑 fetch-model.sh）
#   2) adb push 代码 + 模型 + ORT 备援运行时
#   3) 远端跑 scripts/install.sh
set -euo pipefail

SERIAL=""
INSTALL_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -s|--serial) SERIAL="$2"; shift 2 ;;
        *) INSTALL_ARGS+=("$1"); shift ;;
    esac
done
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE=/opt/posture-monitor

log() { echo -e "\033[1;34m[*]\033[0m $*"; }
ok()  { echo -e "\033[1;32m[+]\033[0m $*"; }
err() { echo -e "\033[1;31m[x]\033[0m $*" >&2; }

"${ADB[@]}" get-state >/dev/null || { err "adb 设备未连接"; exit 1; }
ok "adb 设备: $("${ADB[@]}" devices | sed -n 2p)"

# ---------- 1. 模型 ----------
mkdir -p "$REPO/models"
if ! ls "$REPO"/models/*.rknn >/dev/null 2>&1 && ! ls "$REPO"/models/*.onnx >/dev/null 2>&1; then
    log "本地没有模型，先下载"
    bash "$REPO/scripts/fetch-model.sh"
fi

# ---------- 2. 推送 ----------
log "推送代码"
TMP=$(mktemp -d)
mkdir -p "$TMP/pkg"
cp -a "$REPO/src" "$REPO/scripts" "$TMP/pkg/"
tar -C "$TMP/pkg" -czf "$TMP/pkg.tgz" .
"${ADB[@]}" shell "mkdir -p /tmp/posture-src"
"${ADB[@]}" push "$TMP/pkg.tgz" /tmp/posture-src/pkg.tgz >/dev/null
"${ADB[@]}" shell "cd /tmp/posture-src && tar xzf pkg.tgz && ls src/posturemon >/dev/null && echo ok"
ok "代码已推送"

if ls "$REPO"/models/* >/dev/null 2>&1; then
    log "推送模型（$(du -sh "$REPO"/models | cut -f1)）"
    "${ADB[@]}" shell "mkdir -p $REMOTE/models"
    "${ADB[@]}" push "$REPO"/models/. "$REMOTE/models/" >/dev/null
    ok "模型已推送"
fi

# ---------- 3. 安装 ----------
log "远端安装"
ARGS_STR="${INSTALL_ARGS[*]:-}"
"${ADB[@]}" shell "cd /tmp/posture-src && bash scripts/install.sh $ARGS_STR"
ok "安装完成"

echo
log "下一步"
cat <<HINT
  # 自检
  adb shell "cd /tmp/posture-src && bash scripts/verify.sh"
  # 看界面（需要重启 gdm 触发自动登录）
  adb shell "systemctl restart gdm"
  # 抓一张屏幕截图留证
  adb shell "sudo bash $REMOTE/../posture-screenshot.sh /tmp/posture.png" && adb pull /tmp/posture.png
HINT
