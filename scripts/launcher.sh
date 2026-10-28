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
LOG_DIR="${LOG_DIR:-/userdata/posture-monitor/log}"
RUN_DIR="${RUN_DIR:-/run/posture-monitor}"
QUIT_FLAG=/tmp/.posture-monitor.quit

rm -f "$QUIT_FLAG"
# /var/log 在板端是 tmpfs 软链（重启清空），所以日志默认落 /userdata（持久分区）
if ! mkdir -p "$LOG_DIR" "$RUN_DIR" 2>/dev/null; then
    LOG_DIR=/tmp/posture-monitor-log
    mkdir -p "$LOG_DIR"
fi
# 自己接管日志：autostart 的 `>> 路径` 在目录不存在时会让整条 Exec 失败（踩过）
exec >>"$LOG_DIR/app.log" 2>&1
echo "=== $(date '+%F %T') launcher start (uid=$(id -u)) LOG_DIR=$LOG_DIR ==="

# 等用户 D-Bus 就绪：autostart 在登录后 3s 就跑，偶尔 dbus 还没活，
# 会导致 GTK 报 "Unable to register the application ... NoReply"（窗口能开但没进 D-Bus）
if [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    for _ in $(seq 1 20); do
        busctl --user --timeout=1 list >/dev/null 2>&1 && break
        sleep 0.5
    done
fi

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
    # shellcheck disable=SC2086
    python3 -m posturemon ${POSTURE_ARGS:-} "$@"
    rc=$?
    echo "=== $(date '+%F %T') exit rc=$rc ==="
    if [ -f "$QUIT_FLAG" ]; then
        rm -f "$QUIT_FLAG"
        echo "=== manual quit, 不再重启 ==="
        exit 0
    fi
    sleep 2
done
