# 部署

> 依据：实施方案 §7（板端环境）、§8.3（编译）、§13.2（systemd 部署）
>
> 目标板：Quectel Pi M2 / RK3576 / Debian 13 (trixie) aarch64
>
> 与实施方案的差异：**编译在板端原生做**，不交叉编译 —— 因为板端已经有
> `libopencv-dev` 头文件与 `librknnrt.so`，只要补一个编译器就能避免
> 交叉 sysroot / glibc / OpenCV 版本三重对齐的麻烦。交叉编译的说明在 §7。

---

## 0. 环境速查

| 项 | 板端实测值 |
|---|---|
| 板卡 | Quectel PI M2 RK3576 Board，aarch64，8 核 |
| 内存 | 3.8 GiB（服务实测占用 ~51 MB，`MemoryMax=1500M`）|
| 系统 | Debian GNU/Linux 13 (trixie) |
| 编译器 | g++ 14.2.0（**出厂没有，需 §1 安装**）|
| 构建 | cmake 3.31.6、make、binutils（同上）|
| OpenCV | 4.10.0（`core imgproc videoio imgcodecs`，镜像自带）|
| RKNN | librknnrt.so **2.3.2** / RKNPU 驱动 **v0.9.8**（镜像自带）|
| 摄像头 | USB UVC，实测 UGREEN 1080P，640×480 可用 |

---

## 1. 一次性准备：装编译器（主机 + 板端）

板端 Debian **没有任何编译器、也没有外网**，所以由**主机**把离线依赖准备好：

```bash
# 主机侧（需要能上外网；脚本默认走 http://127.0.0.1:7897 代理）
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

> 只用于开发/演示。量产应当交叉编译，只把二进制推到板端（见 §7）。

---

## 2. 把源码同步到板端

`/tmp` 是 tmpfs，重启就没了；用持久分区放源码：

```bash
tar czf /tmp/posture-src.tgz --exclude=build --exclude=.git .
adb shell 'rm -rf /userdata/posture-src && mkdir -p /userdata/posture-src'
adb push /tmp/posture-src.tgz /userdata/posture-src.tar.gz
adb shell 'cd /userdata/posture-src && tar xzf ../posture-src.tar.gz'
```

如果只需要编译产物相关的部分（避免传 `evidence/` 这类大目录）：

```bash
tar -C . -czf - CMakeLists.txt include src tests config models vendor scripts assets ui \
  | adb shell 'mkdir -p /userdata/posture-src && tar xzf - -C /userdata/posture-src'
```

---

## 3. 编译 + 单元测试

```bash
adb shell 'cd /userdata/posture-src && bash scripts/build_board.sh'
```

脚本做三件事：`cmake -S . -B build`、`cmake --build build -j8`、`ctest`。

**当前状态：6 个测试可执行，`ctest` 6/6 通过。**

| 测试 | 覆盖 |
|---|---|
| `test_posture_rules` | 几何规则、门控、取景门、投票制、托腮哨兵一致性 |
| `test_session_timer` | 护眼会话状态机、提醒防抖与冷却 |
| `test_config` | 配置解析与生产取值合理性 |
| `test_profile_hysteresis` | 画像级联迟滞、guard 互斥、托腮规则 |
| `test_command_parse` | UI socket 命令解析（JSON 空格容错）|
| `test_rknn_decode` | RKNN 输出解码（DFL/NMS/关键点）|

> ⚠️ 用 `tar` 传源码时，tar 会保留原 mtime。如果板端构建目录里的对象文件比源码
> 「新」，`make` 会认为不需要重编，出现「改了代码但行为没变」的假象。
> `build_board.sh` 里已经先 `touch` 了一遍源码，手工编译时请照做。

---

## 4. 部署

```bash
adb shell 'cd /userdata/posture-src && sudo bash scripts/deploy.sh'
```

做的事：

| 步骤 | 内容 |
|---|---|
| 1 | 检查 `build/posture_guard` 与 `models/*.rknn` |
| 2 | 创建系统账号 `posture`（`nologin`），加入 `video` 组（摄像头节点是 `root:video 0660`）|
| 3 | 装 `/opt/posture-guard/{bin,models,ui,docs,scripts}` + `assets/voice/*.wav` |
| 4 | 装配置到 `/etc/posture-guard/posture.yaml`（旧配置自动备份）|
| 5 | 建 `/var/lib/posture-guard{,/stats}`，写 `/etc/tmpfiles.d/posture-guard.conf` |
| 6 | 调扬声器 DAC 音量（出厂 179 = -6dB，配小喇叭听不清，调到 205 并试听）|
| 7 | 装 `posture-guard.service`，`daemon-reload` + `enable` |
| 8 | `systemctl restart`，失败时直接打印 30 行日志 |

> 第 6 步失败不影响部署（例如非 M2 板没有那个 ALSA path 文件）。

### 4.1 服务单元要点

```ini
[Service]
User=posture
Group=posture
SupplementaryGroups=video
ExecStart=/opt/posture-guard/bin/posture_guard \
          --config /etc/posture-guard/posture.yaml --headless
Restart=on-failure
RestartSec=3
RuntimeDirectory=posture-guard       # 自动建 /run/posture-guard 并授权
StateDirectory=posture-guard         # 自动建 /var/lib/posture-guard 并授权
Nice=-5                              # 护眼计时是时间敏感服务
MemoryMax=1500M                      # 防止内存泄漏拖死整机
```

---

## 5. 自检

```bash
adb shell 'bash /opt/posture-guard/scripts/verify.sh'
# ================ 结果：PASS=17  FAIL=0  WARN=0 ================
```

覆盖：服务状态、socket、视频流、NPU 模型、摄像头、校准基准、统计落盘、
近 10 分钟无 ERROR、桌面界面存在等。

---

## 6. 摄像头与校准

### 6.1 摄像头参数（自动协商，不用手配）

服务启动时先用 `VIDIOC_ENUM_*` 问清设备能力，再挑最接近 `camera.*` 的一组，
日志会打印「请求 vs 实际」：

```
摄像头能力：UGREEN Camera 1080P [YUYV(640x480) MJPG(640x480) ]
摄像头已打开 /dev/video73 实际 640x480@30fps 格式 MJPG（请求 640x480 MJPG）
```

`camera.preferred_pixel_format: "auto"` = 用设备能给的最好格式
（有 MJPG 就用 MJPG 省 USB 带宽）。换摄像头不用改代码。

**设备路径用 `by-id`**：热插拔后 `videoN` 会换号，`by-id` 不会。

```yaml
device: "/dev/v4l/by-id/usb-UGREEN_Camera_1080P_...-video-index0"
```

### 6.2 定焦（必须做）

自动对焦摄像头的连续 AF 会让画面持续轻微失焦（实测清晰度差 8 倍），
而且出厂默认焦点往往不在工位距离上。

```bash
adb shell 'bash /opt/posture-guard/scripts/focus.sh --scan'
```

扫出最佳值写进 `config/posture.yaml` 的 `focus_absolute`，然后重启服务。

### 6.3 调机位（必须做）

```bash
adb shell 'python3 /opt/posture-guard/scripts/frame_check.py'
```

要求：头顶留白 ≥6%、人在画面内不贴边、双肩完整入画。

**机位不对是判定失真的头号原因**：头顶出框会导致手腕不在画面内，
托腮判不出来——这是物理摆位问题，代码解决不了。

### 6.4 校准

坐到摄像头前保持正常坐姿 **5 秒**，程序自动采集基准（中位数 + 离群过滤）并保存。

换机位 / 换人 / 换摄像头后**必须重新校准**。

### 6.5 采姿势样本（提高精度）

系统用「逐姿势画像」做判定，规则阈值来自真实采集样本：

```bash
adb shell 'python3 /opt/posture-guard/scripts/pose_capture.py --guided'
```

引导式走完 6 个姿势，每个姿势采 6 秒 → 自动生成
`/var/lib/posture-guard/postures.txt`。

> **两套阈值不要混用**：
> - `config/posture.yaml` 是**工程初始值**（人工设定），用于无画像文件时的传统规则路径
> - `postures.txt` 是**从采集数据算出来的**级联画像规则，有画像文件时优先用它
>
> 模型升级或换机位后必须重新采样本，否则画像会与新的关键点分布错位。

---

## 7. 装桌面界面（可选）

后台服务是无界面的。要屏幕提醒就再装一个用户会话里的 UI
（界面默认显示摄像头实时画面 + 骨架叠加，配置见 `ui.stream_video`）。

```bash
adb shell 'sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 \
  bash /opt/posture-guard/scripts/install_ui_autostart.sh'
```

### 7.1 立即启动（推荐用 systemd user unit）

**不要用 `nohup ... &`**：那样起的进程挂在 adb shell 上，adb 一断就可能被杀。

```bash
adb shell 'sudo -u quectel XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  systemctl --user start "app-posture\x2dguard\x2dui@autostart.service"'

# 查状态
adb shell 'sudo -u quectel XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  systemctl --user status "app-posture\x2dguard\x2dui@autostart.service" --no-pager'
```

### 7.2 开机自启

靠 `~/.config/autostart/posture-guard-ui.desktop`，`X-GNOME-Autostart-Delay=4`
留出 4 秒等用户 D-Bus 就绪。装完后 **GNOME 会为它生成一个 systemd 单元**：

```
/run/user/1000/systemd/generator.late/app-posture\x2dguard\x2dui@autostart.service
```

> **实测踩过的坑**：这个单元可能是 `inactive (dead)` 且**没有失败记录** ——
> 说明 GNOME 的自动启动环节没触发它，而不是程序崩了。
> 排查方法：`systemctl --user status "app-posture\x2dguard\x2dui@autostart.service"`，
> 若确认手动能起但开机不自动起，就用上面的 `--user start` 手动拉起。

> **不要在 `Exec=` 里写 `>> /var/log/...`**：板端 `/var/log` 是
> `→ /var/volatile/log` 的软链（tmpfs），目录不存在会让整条 Exec 直接不执行，
> 表现为「开机没有界面也没有日志」。需要日志就让程序自己写。

---

## 8. 交叉编译（替代方案）

如果板端不想装编译器：

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

---

## 9. 目录一览

```
/opt/posture-guard/
├── bin/posture_guard          # 后台服务（C++）
├── models/yolov8n-pose.rknn   # w8a8 模型（7 348 654 B ≈ 7.0 MiB，
│                              #   sha256 5c25db4f…）
├── ui/posture_guard_ui.py     # GTK4 全屏界面（用户会话）
├── assets/voice/*.wav         # 6 条神经女声（24kHz mono 16bit）
├── scripts/                   # 见下表
└── docs/                      # 本目录的副本

/etc/posture-guard/posture.yaml          # 配置（旧版自动备份）
/etc/systemd/system/posture-guard.service
/etc/tmpfiles.d/posture-guard.conf       # 开机建 /run 与 /var/lib 目录
/var/lib/posture-guard/baseline.json     # 个人校准基准
/var/lib/posture-guard/postures.txt      # 逐姿势画像规则（采样本生成）
/var/lib/posture-guard/posture-samples/  # 姿势特征样本（重算画像用）
/var/lib/posture-guard/poses/            # 姿势截图 + 每姿势 summary
/var/lib/posture-guard/stats/*.jsonl     # 每日统计（追加写）
/run/posture-guard/status.sock           # 状态/事件 socket
```

### 9.1 板端安装的脚本

| 脚本 | 用途 |
|---|---|
| `build_board.sh` | 板端编译 + 跑单元测试 |
| `deploy.sh` | 安装/升级/卸载到系统 |
| `verify.sh` | 17 项自检 |
| `check_board.sh` | 板卡环境体检 |
| `focus.sh` | 对焦扫描与锁定（`--scan` / `--info`）|
| `smoke_camera.sh` | 摄像头体检，查实际节点 |
| `frame_check.py` | 取景检查，逐项提示直到 ✅ |
| `frame_tune.py` | 取景参数微调 |
| `pose_capture.py` | 姿势样本采集（`--guided` 引导式）|
| `teach_posture.py` | 从样本生成 `postures.txt` |
| `status.sh` / `probe_status.py` | 读 socket 看实时状态 |
| `calibrate.sh` | 重新校准 |
| `snooze.sh` | 免打扰 / 「已休息」确认 |
| `screenshot.sh` | Wayland 下抓屏 |
| `speaker_check.sh` / `tune_speaker.sh` | 音量与试听 |
| `live_check.py` | 实时指标检查 |
| `install_ui_autostart.sh` | 装桌面界面自启 |

`/run` 与 `/var/lib` 下的目录由 systemd 的 `RuntimeDirectory=` / `StateDirectory=`
自动创建并授权给 `posture` 用户，不需要手工 `chown`。

---

## 10. 常用运维命令

```bash
systemctl status posture-guard --no-pager
journalctl -u posture-guard -f

bash /opt/posture-guard/scripts/status.sh                # 实时指标
bash /opt/posture-guard/scripts/status.sh --watch        # 每秒刷新
bash /opt/posture-guard/scripts/verify.sh                # 17 项自检
bash /opt/posture-guard/scripts/calibrate.sh             # 重新校准
bash /opt/posture-guard/scripts/snooze.sh 15             # 免打扰 15 分钟
bash /opt/posture-guard/scripts/snooze.sh --ack          # 「已休息」，开始新一轮
bash /opt/posture-guard/scripts/smoke_camera.sh          # 摄像头体检
bash /opt/posture-guard/scripts/frame_check.py           # 取景检查
bash /opt/posture-guard/scripts/screenshot.sh /tmp/s.png # Wayland 下抓屏
```

---

## 11. 卸载

```bash
sudo bash scripts/deploy.sh --uninstall   # 停服务、删程序，保留数据与配置
sudo bash scripts/deploy.sh --purge       # 连 /var/lib/posture-guard 和配置一起删
```

---

## 12. 已知的现场前提

1. **摄像头要能看到头和双肩**。UVC 摄像头位置偏低时人坐近鼻子会贴到画面上沿；
   程序会判 `UNKNOWN` 并提示「请把摄像头抬高」，而不是给一个错的判定。
   现场必须先把机位调好，再做校准。
2. **手腕必须入画**。头顶出框时手腕常在画面外，此时托腮判不出来（读到哨兵值）。
   这是物理摆位问题，不是算法缺陷。
3. 部分 UVC 只给 YUYV 不给 MJPG（日志里会有 WARN）。功能不受影响，
   只是 USB 带宽占用更高；换支持 MJPG 的摄像头可降低 CPU。
4. 摄像头同时只能被一个进程打开。切换实例（例如用 `--source-image` 做离线回归）
   前先 `systemctl stop posture-guard`，否则会 `EBUSY`。
5. 摄像头热插拔后会换 `videoN` 编号，`by-id` 路径不受影响；重连由服务内部
   指数退避处理，进程不会退出。
