# 部署

> 依据：实施方案 §7（板端环境）、§8.3（编译）、§13.2（systemd 部署）
>
> 目标板：Quectel Pi M2 / RK3576 / Debian 13 (trixie) aarch64
>
> 与实施方案的差异：**编译在板端原生做**，不交叉编译 —— 因为板端已经有
> `libopencv-dev` 头文件与 `librknnrt.so`，只要补一个编译器就能避免
> 交叉 sysroot / glibc / OpenCV 版本三重对齐的麻烦。交叉编译的说明在 §6。

## 1. 一次性准备（主机侧）

板端 Debian 没有编译器、也没有外网，所以由**主机**把离线依赖准备好：

```bash
# 主机需要能上外网（脚本默认走 http://127.0.0.1:7897 代理）
PROXY=http://127.0.0.1:7897 bash scripts/fetch_deps.sh

# 产物
#   vendor/rknn/rknn_api.h       与板端 librknnrt.so 2.3.2 配套的头文件
#   tools/board-deps.tgz         g++-14 / cmake / make / binutils（Debian trixie arm64）
```

推到板端并安装：

```bash
adb push tools/board-deps.tgz /userdata/
adb shell 'mkdir -p /userdata/board-deps && cd /userdata/board-deps \
  && tar xzf ../board-deps.tgz && dpkg -i --force-depends *.deb \
  && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
       --slave /usr/bin/g++ g++ /usr/bin/g++-14'
adb shell 'g++ --version; cmake --version'
# g++ (Debian 14.2.0-19) 14.2.0
# cmake version 3.31.6
```

> 只用于开发/演示。量产应当交叉编译，只把二进制推到板端。

## 2. 把源码同步到板端

`/tmp` 是 tmpfs，重启就没了；用持久分区放源码包：

```bash
tar czf /tmp/posture-src.tgz --exclude=build --exclude=.git .
adb shell 'rm -rf /userdata/posture-src && mkdir -p /userdata/posture-src'
adb push /tmp/posture-src.tgz /userdata/posture-src.tar.gz
adb shell 'cd /userdata/posture-src && tar xzf ../posture-src.tar.gz'
```

## 3. 编译 + 单元测试

```bash
adb shell 'cd /userdata/posture-src && bash scripts/build_board.sh'
```

脚本做三件事：`cmake -S . -B build`、`cmake --build build -j8`、`ctest`。

> ⚠️ 如果你像我一样用 `tar` 传源码，注意 tar 会保留原 mtime。如果板端构建目录里
> 的对象文件比源码「新」，`make` 会认为不需要重编，出现「改了代码但行为没变」的
> 假象。`build_board.sh` 里已经先 `touch` 了一遍源码，手工编译时请照做。

## 4. 部署

```bash
adb shell 'cd /userdata/posture-src && bash scripts/deploy.sh'
```

做的事（7 步）：

| 步骤 | 内容 |
|---|---|
| 1 | 检查 `build/posture_guard` 与 `models/*.rknn` |
| 2 | 创建系统账号 `posture`（`nologin`），加入 `video` 组（摄像头节点是 `root:video 0660`） |
| 3 | 装 `/opt/posture-guard/{bin,models,ui,docs,scripts}` |
| 4 | 装配置到 `/etc/posture-guard/posture.yaml`（旧配置自动备份） |
| 5 | 建 `/var/lib/posture-guard{,/stats}`，写 `/etc/tmpfiles.d/posture-guard.conf` |
| 6 | 装 `posture-guard.service`，`daemon-reload` + `enable` |
| 7 | `systemctl restart`，失败时直接打印 30 行日志 |

## 5. 装桌面界面（可选）

后台服务是无界面的。要屏幕提醒就再装一个用户会话里的 UI：

```bash
adb shell 'sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 bash /opt/posture-guard/scripts/install_ui_autostart.sh'
# 立即启动（不用重启）：
adb shell 'sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 GSK_RENDERER=ngl \
  nohup python3 /opt/posture-guard/ui/posture_guard_ui.py >/tmp/ui.log 2>&1 &'
```

开机自启靠 `~/.config/autostart/posture-guard-ui.desktop`，`X-GNOME-Autostart-Delay=4`
留出 4 秒等用户 D-Bus 就绪。

> **不要在 `Exec=` 里写 `>> /var/log/...`**：板端 `/var/log` 是
> `→ /var/volatile/log` 的软链（tmpfs），目录不存在会让整条 Exec 直接不执行，
> 表现为「开机没有界面也没有日志」。需要日志就让程序自己写。

## 6. 交叉编译（替代方案）

如果你的板端不想装编译器：

```bash
sudo apt install -y g++-aarch64-linux-gnu
cmake -S . -B build-aarch64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-aarch64 -j8
```

toolchain 文件需要提供 `CMAKE_SYSROOT`（板端 `/usr` 的拷贝）、`RKNN_LIBRARY`
（板端 aarch64 的 `librknnrt.so`）与 aarch64 版 OpenCV。注意 glibc 版本：
主机工具链自带的 glibc 必须 **≤** 板端版本（板端是 2.41）。

## 7. 目录一览

```
/opt/posture-guard/
├── bin/posture_guard          # 后台服务（C++，静态库内部链接）
├── models/yolov8n-pose.rknn   # w8a8 模型（7.1MB，sha256 5c25db4f…）
├── ui/posture_guard_ui.py     # GTK4 全屏界面（用户会话）
├── scripts/                   # check_board / smoke_camera / build_board / deploy
│                              # verify / status / calibrate / snooze / screenshot
└── docs/                      # 本目录的副本

/etc/posture-guard/posture.yaml          # 配置
/etc/systemd/system/posture-guard.service
/etc/tmpfiles.d/posture-guard.conf       # 开机建 /run 与 /var/lib 目录
/var/lib/posture-guard/baseline.json     # 个人校准基准
/var/lib/posture-guard/stats/*.jsonl     # 每日统计（追加写）
/run/posture-guard/status.sock           # 状态/事件 socket
```

`/run` 与 `/var/lib` 下的目录由 systemd 的 `RuntimeDirectory=` / `StateDirectory=`
自动创建并授权给 `posture` 用户，不需要手工 `chown`。

## 8. 常用运维命令

```bash
systemctl status posture-guard --no-pager
journalctl -u posture-guard -f
bash /opt/posture-guard/scripts/status.sh --watch        # 实时指标
bash /opt/posture-guard/scripts/verify.sh                # 16 项自检
bash /opt/posture-guard/scripts/calibrate.sh             # 重新校准
bash /opt/posture-guard/scripts/snooze.sh 15             # 免打扰 15 分钟
bash /opt/posture-guard/scripts/snooze.sh --ack          # 「已休息」，开始新一轮
bash /opt/posture-guard/scripts/smoke_camera.sh          # 摄像头体检
bash /opt/posture-guard/scripts/screenshot.sh /tmp/s.png # Wayland 下抓屏
```

## 9. 卸载

```bash
sudo bash scripts/deploy.sh --uninstall   # 停服务、删程序，保留数据与配置
sudo bash scripts/deploy.sh --purge       # 连 /var/lib/posture-guard 和配置一起删
```

## 10. 已知的现场前提

1. **摄像头要能看到头和双肩**。这台机器上 UVC 摄像头位置偏低，人坐近时
   鼻子会贴到画面上边缘；程序会判 `UNKNOWN` 并提示「请把摄像头抬高」，
   而不是给一个错的判定。现场必须先把机位调好，再做校准。
2. 这台 UVC 只给 YUYV，不给 MJPG（日志里会有 WARN）。功能不受影响，
   只是 USB 带宽占用更高；换支持 MJPG 的摄像头可以降低 CPU。
3. 摄像头同时只能被一个进程打开。切换实例（例如用 `--source-image` 做离线回归）
   前先 `systemctl stop posture-guard`，否则会 `EBUSY`。
