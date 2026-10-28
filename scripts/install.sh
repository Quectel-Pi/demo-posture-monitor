#!/bin/bash
# install.sh — 在 Quectel Pi M2 板端安装「坐姿检测与护眼提醒一体机」
#
# 幂等，可重复执行。在仓库根目录运行（板端）：
#     sudo bash scripts/install.sh                 # 创建/复用用户 quectel，开自启
#     sudo bash scripts/install.sh --user w        # 指定已有用户
#     sudo bash scripts/install.sh --no-autologin  # 不改 GDM 自动登录
#     sudo bash scripts/install.sh --no-autostart  # 只装，不开机自启
#
# 依赖：python3(+cv2, gi/Gtk4, sqlite3)、librknnrt.so、/opt/posture-monitor/models/*
set -euo pipefail

APP_DIR=/opt/posture-monitor
# 注意：板端 /var/log -> /var/volatile/log 是 tmpfs，重启清空；/userdata 是持久分区
DATA_DIR=/userdata/posture-monitor/data
LOG_DIR=/userdata/posture-monitor/log
CONF_DIR=/etc/posture-monitor
LAUNCHER=/usr/local/bin/posture-monitor.sh

TARGET_USER=""
DO_AUTOLOGIN=1
DO_AUTOSTART=1
DO_START=1

for a in "$@"; do
    case "$a" in
        --user=*) TARGET_USER="${a#*=}" ;;
        --no-autologin) DO_AUTOLOGIN=0 ;;
        --no-autostart) DO_AUTOSTART=0; DO_START=0 ;;
        --no-start) DO_START=0 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

log() { echo -e "\033[1;34m[*]\033[0m $*"; }
ok()  { echo -e "\033[1;32m[+]\033[0m $*"; }
warn(){ echo -e "\033[1;33m[!]\033[0m $*"; }
err() { echo -e "\033[1;31m[x]\033[0m $*" >&2; }

[ "$(id -u)" = 0 ] || { err "请用 root 运行"; exit 1; }
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -d "$REPO/src/posturemon" ] || { err "找不到 $REPO/src/posturemon（请在仓库里运行）"; exit 1; }

# ---------- 1. 依赖检查 ----------
log "检查运行时依赖"
python3 - <<'PY' || { err "python 依赖缺失（见上）"; exit 1; }
import sys
miss = []
try:
    import cv2
    print("  cv2", cv2.__version__)
except Exception as e:
    miss.append(f"cv2: {e}")
try:
    import numpy
    print("  numpy", numpy.__version__)
except Exception as e:
    miss.append(f"numpy: {e}")
try:
    import sqlite3
    sqlite3.connect(":memory:").execute("select 1")
    print("  sqlite3 ok")
except Exception as e:
    miss.append(f"sqlite3: {e}")
try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk
    print("  Gtk", Gtk.get_major_version(), Gtk.get_minor_version())
except Exception as e:
    miss.append(f"Gtk4(pygobject): {e}")
try:
    import cairo
    print("  pycairo", cairo.version)
except Exception as e:
    miss.append(f"pycairo: {e}")
if miss:
    print("  缺失: " + "; ".join(miss))
    sys.exit(1)
PY

if [ -f /usr/lib/librknnrt.so ]; then
    ok "librknnrt.so 存在（NPU 后端可用）"
else
    warn "没有 /usr/lib/librknnrt.so → 只能走 ONNX CPU 后端"
fi

# ---------- 2. 模型 ----------
log "检查模型"
[ -f "$APP_DIR/models/yolov8n-pose-rknn.rknn" ] || warn "缺少 RKNN 模型 $APP_DIR/models/yolov8n-pose-rknn.rknn"
[ -f "$APP_DIR/models/yolov8n-pose.onnx" ] || warn "缺少 ONNX 备援模型 $APP_DIR/models/yolov8n-pose.onnx"
if ! ls "$APP_DIR"/models/*.rknn >/dev/null 2>&1 && ! ls "$APP_DIR"/models/*.onnx >/dev/null 2>&1; then
    err "一个模型都没有。先在开发机跑 scripts/fetch-model.sh，再 adb push 上来"
    exit 1
fi

# ---------- 3. 目标用户 ----------
if [ -z "$TARGET_USER" ]; then
    TARGET_USER="$(awk -F: '$3>=1000 && $3<65534 && $7!~/nologin|false/ {print $1; exit}' /etc/passwd || true)"
    [ -n "$TARGET_USER" ] || TARGET_USER=quectel
fi

if ! getent passwd "$TARGET_USER" >/dev/null 2>&1; then
    log "用户 $TARGET_USER 不存在，创建它"
    useradd -m -s /bin/bash "$TARGET_USER"
    echo "$TARGET_USER:${POSTURE_USER_PASS:-quectel}" | chpasswd
    ok "已创建用户 $TARGET_USER（口令 $TARGET_USER，建议改）"
fi
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[ -n "$TARGET_HOME" ] || { err "用户 $TARGET_USER 不存在"; exit 1; }
TARGET_UID="$(id -u "$TARGET_USER")"
ok "目标用户 $TARGET_USER (uid=$TARGET_UID, home=$TARGET_HOME)"

# ---------- 4. 目录与代码 ----------
log "安装到 $APP_DIR"
install -d "$APP_DIR" "$DATA_DIR" "$LOG_DIR" "$CONF_DIR" "$APP_DIR/assets"
rm -rf "$APP_DIR/posturemon"
cp -a "$REPO/src/posturemon" "$APP_DIR/posturemon"
rm -rf "$APP_DIR/posturemon/__pycache__"
chown -R root:root "$APP_DIR/posturemon"
python3 -m compileall -q "$APP_DIR/posturemon" >/dev/null && ok "字节码编译通过"

log "生成提示音"
python3 "$REPO/scripts/make-assets.py" "$APP_DIR/assets/beep.wav" >/dev/null && ok "beep.wav"

# ---------- 5. 目录权限 ----------
log "数据/日志目录权限"
install -d -o "$TARGET_USER" -g "$TARGET_USER" /userdata/posture-monitor "$DATA_DIR" "$LOG_DIR"
chmod 755 /userdata/posture-monitor "$DATA_DIR" "$LOG_DIR"

log "安装 systemd-tmpfiles 规则（保证 /run 下的运行态目录每次开机自动建好）"
cat > /etc/tmpfiles.d/posture-monitor.conf <<EOF
# 由 scripts/install.sh 生成：一体机运行时目录
d /run/posture-monitor 0775 $TARGET_USER $TARGET_USER -
d /userdata/posture-monitor 0775 $TARGET_USER $TARGET_USER -
d $DATA_DIR 0775 $TARGET_USER $TARGET_USER -
d $LOG_DIR 0775 $TARGET_USER $TARGET_USER -
EOF
systemd-tmpfiles --create /etc/tmpfiles.d/posture-monitor.conf 2>/dev/null || true
ok "/etc/tmpfiles.d/posture-monitor.conf"

# ---------- 6. 配置 ----------
if [ ! -f "$CONF_DIR/config.json" ]; then
    log "写默认配置 $CONF_DIR/config.json（可用它改阈值/摄像头）"
    python3 - "$CONF_DIR/config.json" "$DATA_DIR/posture.db" <<'PY'
import json, sys, os
sys.path.insert(0, "/opt/posture-monitor")
from posturemon.config import DEFAULTS
cfg = dict(DEFAULTS)
cfg["db_path"] = sys.argv[2]
os.makedirs(os.path.dirname(sys.argv[1]), exist_ok=True)
json.dump(cfg, open(sys.argv[1], "w"), ensure_ascii=False, indent=2)
PY
    ok "$CONF_DIR/config.json"
else
    ok "保留已有 $CONF_DIR/config.json"
fi

# ---------- 7. 启动器 ----------
log "生成启动器 $LAUNCHER"
install -m 755 "$REPO/scripts/launcher.sh" "$LAUNCHER"
ok "$LAUNCHER"

# ---------- 8. 自启 ----------
if [ "$DO_AUTOSTART" = 1 ]; then
    log "注册 GNOME 自启（$TARGET_HOME/.config/autostart）"
    install -d -o "$TARGET_USER" -g "$TARGET_USER" "$TARGET_HOME/.config/autostart"
    cat > "$TARGET_HOME/.config/autostart/posture-monitor.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Posture Monitor
Comment=坐姿检测与护眼提醒一体机
Exec=$LAUNCHER
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=3
Terminal=false
EOF
    chown "$TARGET_USER:$TARGET_USER" "$TARGET_HOME/.config/autostart/posture-monitor.desktop"
    chmod 664 "$TARGET_HOME/.config/autostart/posture-monitor.desktop"
    ok "autostart desktop"

    log "关掉自动息屏/休眠（演示时不能黑屏）"
    runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
        gsettings set org.gnome.desktop.session idle-delay 0 2>/dev/null || warn "idle-delay 设置失败（无图形会话时正常）"
    runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
        gsettings set org.gnome.desktop.screensaver idle-activation-enabled false 2>/dev/null || true
    runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
        gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing' 2>/dev/null || true
fi

# ---------- 9. GDM 自动登录 ----------
if [ "$DO_AUTOLOGIN" = 1 ]; then
    GDM_CONF=/etc/gdm3/daemon.conf
    if [ -f "$GDM_CONF" ]; then
        log "配置 GDM 自动登录 -> $TARGET_USER"
        [ -f "$GDM_CONF.bak" ] || cp -a "$GDM_CONF" "$GDM_CONF.bak"
        python3 - "$GDM_CONF" "$TARGET_USER" <<'PY'
import re, sys
path, user = sys.argv[1], sys.argv[2]
s = open(path).read()
if "[daemon]" in s:
    m = re.search(r"\[daemon\](.*?)(?=\n\[|\Z)", s, re.S)
    body = re.sub(r"\n\s*#?\s*AutomaticLoginEnable\s*=.*", "", m.group(1))
    body = re.sub(r"\n\s*#?\s*AutomaticLogin\s*=.*", "", body)
    body = body.rstrip() + f"\nAutomaticLoginEnable=true\nAutomaticLogin={user}\n"
    s = s[:m.start(1)] + body + s[m.end(1):]
else:
    s = f"[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin={user}\n\n" + s
open(path, "w").write(s)
PY
        ok "$(grep -E 'AutomaticLogin' "$GDM_CONF" | tr '\n' ' ')"
        warn "自动登录需重启 gdm 或重启整机生效：systemctl restart gdm"
    else
        warn "没有 $GDM_CONF，跳过自动登录"
    fi
fi

# ---------- 10. 摄像头探测 ----------
log "探测摄像头"
python3 - <<'PY' || warn "没找到可用视频设备"
import glob, os
devs = sorted(glob.glob("/dev/v4l/by-id/*video-index0"))
if not devs:
    raise SystemExit(1)
for d in devs:
    print("  ", d, "->", os.path.realpath(d))
PY

# ---------- 11. 立即启动 ----------
if [ "$DO_START" = 1 ] && [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    log "当前 shell 有图形环境，直接拉起"
    runuser -u "$TARGET_USER" -- env \
        XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
        DISPLAY="${DISPLAY:-:0}" \
        "$LAUNCHER" >>"$LOG_DIR/app.log" 2>&1 &
    ok "已后台启动（日志 $LOG_DIR/app.log）"
elif [ "$DO_START" = 1 ]; then
    warn "当前 shell 没有图形环境；登录桌面后会自动启动，或手动执行 sudo -u $TARGET_USER $LAUNCHER"
fi

echo
log "===== 安装完成 ====="
cat <<HINT
  代码      : $APP_DIR/posturemon
  模型      : $APP_DIR/models
  配置      : $CONF_DIR/config.json
  数据库    : $DATA_DIR/posture.db
  日志      : $LOG_DIR/app.log
  启动器    : $LAUNCHER
  自启      : $TARGET_HOME/.config/autostart/posture-monitor.desktop
  运行用户  : $TARGET_USER
  自检      : sudo bash $REPO/scripts/verify.sh
HINT
