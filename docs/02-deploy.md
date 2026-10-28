# 02 · 部署方案

## 1. 目标环境

| 项 | 值（实测） |
|---|---|
| 板卡 | Quectel Pi M2 / `QSM610ZAP-WFPI` / 模组 SH603ZA（RK3576） |
| 系统 | Debian GNU/Linux 13 (trixie)，内核 6.1.118-rt36 |
| 桌面 | GNOME 46 · Wayland · 显示 1280×800 |
| Python | 3.13.5 |
| 图形栈 | Gtk 4.18 · pycairo 1.27.0 · Mesa |
| AI 运行时 | `librknnrt.so` **2.3.2** · RKNPU 驱动 **0.9.8** |
| 摄像头 | USB UVC `icSpring` → `/dev/video73`（640×480@30 YUYV） |
| 外网 | **无**（eth0 有 IP 但网关不通、无 DNS）→ 全部离线部署 |

## 2. 目录布局（板端）

```
/opt/posture-monitor/
├── posturemon/            # 应用代码（root:root，只读）
├── models/
│   ├── yolov8n-pose-rknn.rknn    # 主用，NPU
│   └── yolov8n-pose.onnx         # 备援，CPU
├── samples/               # 回归样图
├── tests/                 # verify_decode.py
├── assets/beep.wav        # 提示音
└── lib/                   # 可选：onnxruntime wheel 解压目录（PYTHONPATH）
/userdata/posture-monitor/
├── data/posture.db        # ★ 持久分区，重启不丢
└── log/app.log            # ★ 持久分区（/var/log 是 tmpfs 软链，重启清空）
/etc/posture-monitor/config.json              # 全部阈值/参数
/etc/tmpfiles.d/posture-monitor.conf          # 开机自动建 /run 与 /userdata 下的目录
/run/posture-monitor/status.json              # 运行态快照（每秒刷新，tmpfs 正常）
/usr/local/bin/posture-monitor.sh             # 崩溃自愈启动器
/home/quectel/.config/autostart/posture-monitor.desktop
```

## 3. 部署链路（为什么这么设计）

```
开机 → GDM 自动登录 quectel → GNOME Wayland 会话
     → XDG autostart 执行 /usr/local/bin/posture-monitor.sh
     → GSK_RENDERER=ngl / GDK_BACKEND=wayland 环境就绪
     → python3 -m posturemon（Gtk4 全屏）
         └─ 崩溃 → 启动器 2s 后重启（保证展会不中断）
```

- **为什么用 autostart 而不是 systemd unit**：应用需要 Wayland 会话环境
  （`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`），autostart 由会话直接拉起，
  不用手拼环境变量；崩溃自愈交给启动器里的 while 循环。
- **为什么必须 GDM 自动登录**：无人值守的一体机不能停在登录页。安装脚本改
  `/etc/gdm3/daemon.conf`（先备份 `daemon.conf.bak`），卸载脚本原样恢复。

## 4. 分步部署

### 4.1 准备模型（开发机，需要外网）

```bash
bash scripts/fetch-model.sh
# models/yolov8n-pose-rknn.rknn   7.1MB  (RKNNAI/RK3576-CNN-yolov8n-pose, w8a8)
# models/yolov8n-pose.onnx         13MB  (Xenova/yolov8n-pose, fp32)
# dist/onnxruntime-*.whl           21MB  (cp313 aarch64，仅 CPU 备援时需要)
```

有代理就带上：`HTTPS_PROXY=http://127.0.0.1:7897 bash scripts/fetch-model.sh`

### 4.2 推送 + 安装

```bash
bash scripts/deploy-from-host.sh                    # 全自动
bash scripts/deploy-from-host.sh --user=w           # 装机主已有用户
bash scripts/deploy-from-host.sh --no-autologin     # 不动 GDM
```

等价的离线手工流程：

```bash
tar czf pkg.tgz src scripts
adb push pkg.tgz models /tmp/                       # 或者推到 /opt/posture-monitor/models
adb shell
mkdir -p /userdata/posture-monitor/pkg && cd /userdata/posture-monitor/pkg && tar xzf /tmp/pkg.tgz
bash scripts/install.sh --user=quectel
```

### 4.3 生效与验证

```bash
adb shell 'systemctl restart gdm'                    # 出现自动登录 + 全屏界面
adb shell 'cat /run/posture-monitor/status.json'
adb shell 'cd /userdata/posture-monitor/pkg && sudo bash scripts/verify.sh'
```

`status.json` 正常长这样：

```json
{"ts":1793202169.3,"window":[1280,800],"fullscreen":true,"backend":"rknn(NPU)",
 "sdk":"2.3.2 (429f97ae6b@2025-04-09T09:09:27)","latency_ms":25.1,"persons":1,
 "fps":29.3,"present":true,"bad":false,"label":"","bad_duration_s":0.0,
 "today_bad_s":0.0,"counts":{},"rest_active":false,"banner":""}
```

### 4.4 抓屏留证（Mutter 下只有这一条路）

```bash
adb shell 'sudo bash /opt/posture-monitor/../posture-screenshot.sh'   # 用仓库里的 scripts/screenshot.sh
adb pull /tmp/screen-*.png
```

## 5. 参数调整

全部参数在 `/etc/posture-monitor/config.json`（见 `posturemon/config.py` 的 `DEFAULTS`），
改完重启应用生效（`Esc`×2 退出后启动器不会重启，用 `sudo -u quectel /usr/local/bin/posture-monitor.sh &` 重开）。

常用：

| 键 | 默认 | 作用 |
|---|---|---|
| `camera_device` | `/dev/video73` | 换成 `/dev/v4l/by-id/usb-...` 更稳 |
| `conf_threshold` | 0.45 | 调高减少误检，调低提高召回 |
| `infer_interval_ms` | 80 | 推理最小间隔；想更省电就调大 |
| `bad_posture_remind_s` | 60 | 连续不良多久才弹提醒（演示时可改 10） |
| `eye_rest_interval_s` | 1200 | 20 分钟 |
| `backend` | `rknn` | `onnx` 强制走 CPU（对比演示用） |

## 6. 回滚

```bash
adb shell 'sudo bash /userdata/posture-monitor/pkg/scripts/uninstall.sh'          # 保留数据
adb shell 'sudo bash /userdata/posture-monitor/pkg/scripts/uninstall.sh --purge'  # 连数据一起删
adb shell 'systemctl restart gdm'
```

`uninstall.sh` 会：停进程 → 删 autostart → 删启动器/代码 → 从 `daemon.conf.bak`
恢复 GDM 配置。

## 7. 部署检查清单

- [ ] `verify.sh` 无 FAIL
- [ ] `status.json` 的 `backend` 是 `rknn(NPU)`（不是 `onnx(CPU)`）
- [ ] `status.json` 的 `fullscreen` 为 `true`，`window` = 屏幕分辨率
- [ ] 人手进入画面 → `present: true`；坐歪 2s 后 `bad: true` 且 `label` 有值
- [ ] `sqlite3 /userdata/posture-monitor/data/posture.db "select * from day"` 有当日行
- [ ] 重启整机能自己回到界面
