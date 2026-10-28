#!/bin/bash
# uninstall.sh — 卸载（默认保留数据库/日志，加 --purge 一起删）
set -uo pipefail
APP_DIR=/opt/posture-monitor
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

echo "[*] 停止进程"
pkill -f "python3 -m posturemon" 2>/dev/null && echo "    已发送 TERM" || echo "    没在运行"
sleep 1

echo "[*] 移除自启"
for f in /home/*/.config/autostart/posture-monitor.desktop; do
    [ -f "$f" ] && rm -f "$f" && echo "    $f"
done

echo "[*] 移除启动器与代码"
rm -f /usr/local/bin/posture-monitor.sh
rm -rf "$APP_DIR/posturemon" "$APP_DIR/assets"

echo "[*] 回滚 GDM 自动登录"
if [ -f /etc/gdm3/daemon.conf.bak ]; then
    cp -a /etc/gdm3/daemon.conf.bak /etc/gdm3/daemon.conf
    echo "    已从 daemon.conf.bak 恢复"
fi

if [ "$PURGE" = 1 ]; then
    echo "[*] --purge：删除数据/日志/配置/模型"
    rm -rf /userdata/posture-monitor /var/lib/posture-monitor /etc/posture-monitor "$APP_DIR" /etc/tmpfiles.d/posture-monitor.conf
else
    echo "[*] 保留 /userdata/posture-monitor（统计数据与日志）、$APP_DIR/models"
fi
echo "[+] 完成。重启 gdm 或重启整机让桌面恢复原状。"
