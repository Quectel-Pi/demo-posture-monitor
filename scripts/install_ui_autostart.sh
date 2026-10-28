#!/usr/bin/env bash
# 给当前桌面用户装一个开机自启的全屏界面（对应实施方案 §13.3 的 UI 进程）
#
# 用法（在板端，用**桌面登录用户**运行，不要 sudo）：
#   bash scripts/install_ui_autostart.sh
#   bash scripts/install_ui_autostart.sh --remove
set -euo pipefail

PREFIX="${PREFIX:-/opt/posture-guard}"
AUTOSTART_DIR="$HOME/.config/autostart"
DESKTOP="$AUTOSTART_DIR/posture-guard-ui.desktop"

if [[ "${1:-}" == "--remove" ]]; then
  rm -f "$DESKTOP"
  echo "已移除 $DESKTOP"
  exit 0
fi

if [[ "$(id -u)" -eq 0 ]]; then
  echo "!! 不要用 root 跑：自启项要装到桌面用户的家目录" >&2
  exit 1
fi

[[ -f "$PREFIX/ui/posture_guard_ui.py" ]] || {
  echo "!! 找不到 $PREFIX/ui/posture_guard_ui.py（先跑 scripts/deploy.sh）" >&2; exit 1; }
[[ -S /run/posture-guard/status.sock ]] || {
  echo "!! /run/posture-guard/status.sock 不存在，后台服务没起来？" >&2
  echo "   sudo systemctl status posture-guard --no-pager" >&2
}

mkdir -p "$AUTOSTART_DIR"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=坐姿检测与护眼提醒
Comment=读取 posture-guard 后台服务的状态并全屏显示
Exec=/usr/bin/python3 $PREFIX/ui/posture_guard_ui.py
Terminal=false
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=4
# 不要用 Exec 里的 >> 重定向：/var/log 是 tmpfs 软链，目录不存在会让整条 Exec 不执行
EOF

echo "已写入 $DESKTOP"
echo
echo "立即启动界面（不重启）："
echo "  nohup /usr/bin/python3 $PREFIX/ui/posture_guard_ui.py &"
echo "只看状态不开窗："
echo "  python3 $PREFIX/ui/posture_guard_ui.py --probe | python3 -m json.tool"
echo "取消自启："
echo "  bash $0 --remove"
