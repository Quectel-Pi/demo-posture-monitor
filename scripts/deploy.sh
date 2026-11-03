#!/usr/bin/env bash
# 部署到板端（对应实施方案 §13.2）
#
# 在**板端**以 root 运行。默认从仓库当前目录取构建产物。
#
# 用法：
#   sudo bash scripts/deploy.sh                 # 安装/升级 + 启动服务
#   sudo bash scripts/deploy.sh --no-start      # 只安装不启动
#   sudo bash scripts/deploy.sh --uninstall     # 停服务并删除程序（保留数据）
#   sudo bash scripts/deploy.sh --purge         # 连数据一起删
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
PROJ="$(pwd)"

PREFIX=/opt/posture-guard
CONF_DIR=/etc/posture-guard
STATE_DIR=/var/lib/posture-guard
SERVICE=posture-guard
SERVICE_USER=posture
BUILD_DIR="${BUILD_DIR:-build}"

DO_START=1
MODE=install
for a in "$@"; do
  case "$a" in
    --no-start) DO_START=0;;
    --uninstall) MODE=uninstall;;
    --purge) MODE=purge;;
    -h|--help) sed -n '2,14p' "$0"; exit 0;;
  esac
done

[[ "$(id -u)" -eq 0 ]] || { echo "!! 需要 root：sudo bash $0" >&2; exit 1; }

# ---------------------------------------------------------------------------
if [[ "$MODE" == "uninstall" || "$MODE" == "purge" ]]; then
  echo "== 停止并禁用服务 =="
  systemctl disable --now "$SERVICE" 2>/dev/null || true
  rm -f "/etc/systemd/system/$SERVICE.service"
  systemctl daemon-reload
  echo "== 删除程序 =="
  rm -rf "$PREFIX"
  rm -f /etc/tmpfiles.d/posture-guard.conf
  if [[ "$MODE" == "purge" ]]; then
    echo "== 删除数据与配置 =="
    rm -rf "$STATE_DIR" "$CONF_DIR" /run/posture-guard
    id "$SERVICE_USER" >/dev/null 2>&1 && userdel "$SERVICE_USER" || true
  else
    echo "   （数据保留在 $STATE_DIR，用 --purge 才删）"
  fi
  echo "== 卸载完成 =="
  exit 0
fi

# ---------------------------------------------------------------------------
echo "== 1/7 检查构建产物 =="
BIN="$BUILD_DIR/posture_guard"
[[ -x "$BIN" ]] || { echo "!! 找不到 $BIN，先跑 bash scripts/build_board.sh" >&2; exit 1; }
echo "   $BIN $(du -h "$BIN" | cut -f1)"

MODEL_SRC="models/yolov8n-pose.rknn"
[[ -f "$MODEL_SRC" ]] || { echo "!! models/ 下没有 .rknn 模型" >&2; exit 1; }

echo "== 2/7 服务账号 =="
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  useradd -r -M -s /usr/sbin/nologin "$SERVICE_USER"
  echo "   已创建 $SERVICE_USER"
else
  echo "   $SERVICE_USER 已存在"
fi
usermod -aG video "$SERVICE_USER"
echo "   已加入 video 组（摄像头节点是 root:video 0660）"

echo "== 3/7 安装程序与模型 =="
install -d -m 755 "$PREFIX" "$PREFIX/bin" "$PREFIX/models" "$PREFIX/ui" "$PREFIX/assets/voice" "$PREFIX/docs" "$PREFIX/scripts"
install -m 755 "$BIN" "$PREFIX/bin/posture_guard"
install -m 644 "$MODEL_SRC" "$PREFIX/models/yolov8n-pose.rknn"
[[ -f "$PROJ/ui/posture_guard_ui.py" ]] && install -m 755 "$PROJ/ui/posture_guard_ui.py" "$PREFIX/ui/"
for f in "$PROJ"/docs/*.md; do [[ -f "$f" ]] && install -m 644 "$f" "$PREFIX/docs/"; done
for f in "$PROJ"/scripts/*.sh; do [[ -f "$f" ]] && install -m 755 "$f" "$PREFIX/scripts/"; done
install -m 644 "$PROJ"/assets/voice/*.wav "$PREFIX/assets/voice/"
echo "   $PREFIX/bin/posture_guard"
echo "   $PREFIX/models/yolov8n-pose.rknn (sha256 $(sha256sum "$MODEL_SRC" | cut -c1-16))"

echo "== 4/7 配置文件 =="
install -d -m 755 "$CONF_DIR"
if [[ -f "$CONF_DIR/posture.yaml" ]]; then
  cp -a "$CONF_DIR/posture.yaml" "$CONF_DIR/posture.yaml.bak.$(date +%s)"
  echo "   已备份旧配置"
fi
install -m 644 "$PROJ/config/posture.yaml" "$CONF_DIR/posture.yaml"
echo "   $CONF_DIR/posture.yaml"

echo "== 5/7 运行目录 =="
install -d -m 755 -o "$SERVICE_USER" -g "$SERVICE_USER" "$STATE_DIR" "$STATE_DIR/stats"
install -d -m 755 /run/posture-guard
cat > /etc/tmpfiles.d/posture-guard.conf <<'EOF'
# 开机自动建运行目录（/run 是 tmpfs，重启就没了）
d /run/posture-guard 0755 root root -
d /var/lib/posture-guard 0755 posture posture -
d /var/lib/posture-guard/stats 0755 posture posture -
EOF
systemd-tmpfiles --create /etc/tmpfiles.d/posture-guard.conf 2>/dev/null || true
echo "   $STATE_DIR + /etc/tmpfiles.d/posture-guard.conf"

echo "== 5.5/7 喇叭音量（M2 专用，非致命）=="
# WirePlumber 按 ALSA path 配置把 DAC 钉死，出厂 179(-6dB) 配小喇叭听不清；
# 这里调到 205 并试听。失败不影响部署（例如非 M2 板没有这个 path 文件）。
if [[ -f scripts/tune_speaker.sh ]]; then
  sh scripts/tune_speaker.sh "${SPEAKER_DAC:-205}" >/dev/null 2>&1 \
    && echo "   已设置 DAC=${SPEAKER_DAC:-205} 并试听" \
    || echo "   跳过（该板没有 es8389-m2 path 配置）"
fi

echo "== 6/7 systemd 单元 =="
install -m 644 "$PROJ/systemd/$SERVICE.service" "/etc/systemd/system/$SERVICE.service"
systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null
echo "   /etc/systemd/system/$SERVICE.service (enabled)"

echo "== 7/7 启动 =="
if [[ $DO_START -eq 1 ]]; then
  systemctl restart "$SERVICE"
  sleep 2
  systemctl is-active "$SERVICE" >/dev/null && echo "   服务已启动" || {
    echo "!! 启动失败，最近日志：" >&2
    journalctl -u "$SERVICE" -n 30 --no-pager >&2
    exit 1
  }
else
  echo "   已跳过启动（--no-start）"
fi

cat <<EOF

部署完成。常用命令：
  systemctl status $SERVICE --no-pager
  journalctl -u $SERVICE -f
  python3 $PREFIX/ui/posture_guard_ui.py --probe     # 只看状态不开窗
  bash $PREFIX/scripts/verify.sh                     # 自检

桌面全屏界面（可选，需在图形会话里）：
  bash $PREFIX/scripts/install_ui_autostart.sh
EOF
