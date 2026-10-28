#!/bin/bash
# posture-monitor.sh — 一体机前端启动器（由 scripts/install.sh 安装到 /usr/local/bin）
#
# 行为：
#   · 崩溃自动重启（间隔 2s），保证演示不中断
#   · Esc 连按两次退出：窗口侧写 /tmp/.posture-monitor.quit，本脚本看到就不再重启
#   · 写 /run/posture-monitor/status.json（由程序内部写），便于"不看屏也能验证"
#
# 可用环境变量覆盖：
#   POSTURE_ARGS        追加给 python 的参数（如 "--backend=onnx"）
#   GSK_RENDERER        GTK 渲染后端，默认 ngl（Mesa 的 Zink/Vulkan 在 RK3576 上会报
#                       VK_ERROR_INCOMPATIBLE_DRIVER，虽然会回退，但显式指定更干净）
#   GDK_BACKEND         默认 "wayland,x11"
set -uo pipefail

APP_DIR="${APP_DIR:-/opt/posture-monitor}"
LOG_DIR="${LOG_DIR:-/var/log/posture-monitor}"
RUN_DIR="${RUN_DIR:-/run/posture-monitor}"
QUIT_FLAG=/tmp/.posture-monitor.quit

rm -f "$QUIT_FLAG"
mkdir -p "$LOG_DIR" "$RUN_DIR" 2>/dev/null || true

export PYTHONPATH="$APP_DIR${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONUNBUFFERED=1
export GDK_BACKEND="${GDK_BACKEND:-wayland,x11}"
export GSK_RENDERER="${GSK_RENDERER:-ngl}"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR
[ -S "$XDG_RUNTIME_DIR/bus" ] && export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"

# shellcheck disable=SC2086
while true; do
    echo "=== $(date '+%F %T') start posture-monitor (pid $$) ==="
    python3 -m posturemon ${POSTURE_ARGS:-}
    rc=$?
    echo "=== $(date '+%F %T') exit rc=$rc ==="
    if [ -f "$QUIT_FLAG" ]; then
        rm -f "$QUIT_FLAG"
        echo "=== manual quit, 不再重启 ==="
        exit 0
    fi
    sleep 2
done
