#!/bin/bash
# appctl.sh — 板端控制一体机应用（不经 GDM 重启）
#
#   sudo bash scripts/appctl.sh status
#   sudo bash scripts/appctl.sh stop
#   sudo bash scripts/appctl.sh start            # 用 autostart 同一套环境
#   sudo bash scripts/appctl.sh restart
#   sudo bash scripts/appctl.sh demo <图片/视频>  # 用离线素材演示（窗口化，方便抓屏）
#
# 为什么需要它：
#   · 应用由 GNOME autostart 拉起，直接 kill 会被启动器 2s 后重启，调试很烦；
#   · UVC 摄像头同一时刻只能被一个进程打开，换实例必须先停旧实例。
set -uo pipefail

APP_DIR=/opt/posture-monitor
LOG_DIR=/userdata/posture-monitor/log
LAUNCHER=/usr/local/bin/posture-monitor.sh
STATUS=/run/posture-monitor/status.json
USER_NAME="${POSTURE_USER:-$(awk -F: '$3>=1000 && $3<65534 {print $1; exit}' /etc/passwd)}"
UID_N="$(id -u "$USER_NAME")"

as_user() {
    runuser -u "$USER_NAME" -- env \
        XDG_RUNTIME_DIR="/run/user/$UID_N" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$UID_N/bus" \
        PYTHONPATH="$APP_DIR" PYTHONUNBUFFERED=1 \
        GDK_BACKEND="${GDK_BACKEND:-wayland,x11}" \
        GSK_RENDERER="${GSK_RENDERER:-ngl}" \
        "$@"
}

pids() {
    python3 - <<'PY'
import os
out = []
for p in os.listdir("/proc"):
    if not p.isdigit():
        continue
    try:
        c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="replace").replace("\0", " ").strip()
    except Exception:
        continue
    if c.startswith("python3 -m posturemon") or c.startswith("/bin/bash /usr/local/bin/posture-monitor.sh") \
       or c.startswith("bash /usr/local/bin/posture-monitor.sh"):
        out.append(f"{p}\t{c[:70]}")
print("\n".join(out))
PY
}

do_stop() {
    local list
    list="$(pids)"
    if [ -z "$list" ]; then echo "  没在运行"; return 0; fi
    # 先杀启动器（否则它会重启 python），再杀 python
    echo "$list" | awk -F'\t' '$2 ~ /posture-monitor.sh/ {print $1}' | xargs -r kill -9 2>/dev/null
    sleep 1
    echo "$list" | awk -F'\t' '$2 ~ /python3 -m posturemon/ {print $1}' | xargs -r kill -9 2>/dev/null
    sleep 1
    [ -z "$(pids)" ] && echo "  已停止" || { echo "  还在："; pids; }
}

do_start() {
    [ -n "$(pids)" ] && { echo "  已在运行"; return 0; }
    mkdir -p "$LOG_DIR" /run/posture-monitor
    chown "$USER_NAME:$USER_NAME" /run/posture-monitor 2>/dev/null || true
    as_user systemd-run --user --unit=posture-monitor --collect "$LAUNCHER" "$@" >/dev/null 2>&1 \
        || { echo "  systemd-run 失败，退化为 nohup"; as_user nohup "$LAUNCHER" "$@" >>"$LOG_DIR/app.log" 2>&1 & }
    for _ in $(seq 1 20); do
        sleep 0.5
        [ -s "$STATUS" ] && { echo "  已启动"; return 0; }
    done
    echo "  启动超时，看 $LOG_DIR/app.log"
    return 1
}

case "${1:-status}" in
    status)
        echo "用户: $USER_NAME (uid=$UID_N)"
        echo "进程:"; pids | sed 's/^/  /'
        [ -z "$(pids)" ] && echo "  （无）"
        if [ -s "$STATUS" ]; then
            echo "运行态: $(cat "$STATUS")"
        else
            echo "运行态: 无 $STATUS"
        fi
        ;;
    stop) do_stop ;;
    start) shift || true; do_start "$@" ;;
    restart) do_stop; sleep 1; shift || true; do_start "$@" ;;
    demo)
        SRC="${2:?用法: appctl.sh demo <图片或视频>}"
        do_stop
        echo "  以窗口模式跑离线素材: $SRC"
        as_user setsid python3 -m posturemon --source="$SRC" --fullscreen=false \
            >/tmp/posture-demo.log 2>&1 < /dev/null &
        for _ in $(seq 1 20); do
            sleep 0.5
            if grep -q '"fps"' /tmp/posture-demo.log 2>/dev/null || [ -s /run/posture-monitor/status.json ]; then break; fi
        done
        echo "  日志 /tmp/posture-demo.log；抓屏 scripts/screenshot.sh；结束后 appctl.sh start"
        ;;
    *) echo "用法: $0 {status|start|stop|restart|demo <file>}"; exit 2 ;;
esac
