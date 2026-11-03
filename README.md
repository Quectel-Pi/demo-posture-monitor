# RK3576 坐姿检测与护眼提醒一体机

Quectel Pi M2（RK3576 / Debian 13 aarch64）上的坐姿检测 + 20-20-20 护眼提醒系统。

**一个摄像头，两个进程**：C++ 后台服务在 NPU 上跑人体关键点推理，GTK4 全屏界面在用户会话里显示实时画面与状态，两者通过本地 Unix socket 通信。

> 项目编号：DEMO-036 ｜ 平台：Quectel Pi M2 (RK3576) ｜ 摄像头：USB UVC  
> **健康声明**：本系统是习惯提醒工具，不是医疗设备。正面单目摄像头测不出脊柱曲度，也不测真实低头角度；所有「近似量」在界面与文档里都标了。

---

## 1. 它做什么

### 1.1 坐姿识别（六类）

摄像头取帧 → NPU 推理 17 个人体关键点 → 算出几何特征 → 与**你个人校准的基准**比较 → 得出姿势判定。

| 姿势 | 界面显示 | 语音播报 |
|---|---|---|
| 正常 | 绿色「正常」 | — |
| 低头 / 趴桌 | 红色「低头/趴桌」 | 请抬头，别低头趴桌 |
| 歪头 | 红色「歪头」 | 请注意头部姿势 |
| 离屏太近 | 红色「离屏太近」 | 离屏幕远一点 |
| 托腮 | 红色「托腮」 | 请放下手，坐直 |
| 高低肩 | 红色「高低肩」 | 请调整坐姿 |

另有「无人」（离开座位）和「无法判定」（画面不可用）两个状态，都不报警。

### 1.2 护眼提醒（20-20-20 规则）

连续用眼 **20 分钟** → 全屏弹出「该远眺休息了」，要求看向 6 米外 **20 秒**。

坐姿检测会判断你是否在座：离开 10 秒即暂停计时并清零，回到座位重新开始。

### 1.3 其他能力

| 能力 | 实现 |
|---|---|
| 全屏界面 | 摄像头实时画面 + 骨架叠加 + 红/绿状态 + 用眼进度 |
| 中文语音 | 神经女声预生成 WAV，运行期**完全离线** |
| 个人校准 | 坐下保持正常坐姿 5 秒，自动采基准（中位数 + 离群过滤） |
| 基准漂移自检 | 挪了摄像头 / 换了距离，自动发现并提示重新校准 |
| 每日统计 | 在座时长、不良时长、提醒次数，JSONL 持久化 |
| 断线自愈 | 摄像头热插拔后指数退避重连，进程不退出 |

### 1.4 明确不做

脊柱曲度、真实低头角度、**任何距离测量**（本机无 ToF，连预估都不做）、人脸识别、云端上传、保存原始画面。

---

## 2. 部署环境

### 2.1 硬件

| 项 | 要求 / 实测值 |
|---|---|
| 板卡 | **Quectel Pi M2（RK3576）**，aarch64，8 核 |
| 内存 | 3.8 GiB（本服务实测占用 ~51 MB） |
| NPU | RK3576 内置 NPU，驱动 **RKNPU v0.9.8** |
| 摄像头 | **USB UVC 免驱**。实测 UGREEN 1080P，支持 640×480@30fps |
| 显示 | 需要本地图形会话（Wayland / GNOME）用于全屏界面 |
| 音频 | 板载扬声器或 USB 音频，用于语音播报 |

> 摄像头可用 `DEV=.../video-index0` 稳定路径。热插拔后 `videoN` 会换号，`by-id` 不会。

### 2.2 软件

| 组件 | 版本（板端实测） | 说明 |
|---|---|---|
| 操作系统 | **Debian 13 (trixie) aarch64** | 出厂镜像 |
| 编译器 | **g++ 14.2.0** | **出厂没有**，需一次性装（见 3.1） |
| 构建工具 | cmake 3.31.6、make、binutils | 同上 |
| OpenCV | **4.10.0**（`core imgproc videoio imgcodecs`） | 镜像自带 |
| RKNN Runtime | **librknnrt.so 2.3.2** | 镜像自带，配 `vendor/rknn/rknn_api.h` |
| Python | 3.11+（界面 + 运维脚本） | 镜像自带 |
| GTK4 / PyGObject | 界面依赖 | 镜像自带 |

**关键前提**：板端出厂**没有任何编译器**，也没有外网。所以编译依赖必须在主机侧一次性下载好、打包推进去。

### 2.3 主机侧（编译依赖制作）

| 项 | 要求 |
|---|---|
| 架构 | x86_64 Linux（Windows 用 WSL） |
| 网络 | 能访问 Debian 源（需要代理时支持 `PROXY=` 环境变量） |
| 工具 | `dpkg`、`tar`、`adb`（或 `scp`） |

> 也可以用交叉编译替代，但需要同时对齐「工具链 + sysroot + OpenCV + glibc」四件套，比补一个编译器贵得多。见 `docs/deploy.md §8`。

---

## 3. 部署方法

### 3.1 一次性准备：离线编译依赖（主机侧）

```bash
# 在主机上，把 Debian trixie arm64 的编译器 + rknn_api.h 打包
PROXY=http://127.0.0.1:7897 bash scripts/fetch_deps.sh

# 产物：
#   vendor/rknn/rknn_api.h    与板端 librknnrt.so 2.3.2 配套的头文件
#   tools/board-deps.tgz      g++-14 / cmake / make / binutils
adb push tools/board-deps.tgz /userdata/
```

### 3.2 板端：安装编译器

```bash
adb shell 'mkdir -p /userdata/board-deps && cd /userdata/board-deps && \
  tar xzf ../board-deps.tgz && dpkg -i --force-depends *.deb && \
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-14'

# 验证
adb shell 'g++ --version && cmake --version'
```

### 3.3 同步源码并编译

```bash
# 同步（排除 .git / evidence 等大目录）
tar -C . -czf - CMakeLists.txt include src tests config models vendor scripts assets ui \
  | adb shell 'tar xzf - -C /userdata/posture-src'

# 编译 + 单元测试
adb shell 'cd /userdata/posture-src && bash scripts/build_board.sh'
```

`build_board.sh` 会编译出 `build/posture_guard` 并跑全部单元测试：**6 个测试可执行，全通过**。

### 3.4 部署到系统

```bash
adb shell 'cd /userdata/posture-src && sudo bash scripts/deploy.sh'
```

`deploy.sh` 做的事：

1. 装程序到 `/opt/posture-guard/`（二进制、模型、UI、语音、脚本、文档）
2. 装配置到 `/etc/posture-guard/posture.yaml`
3. 建 `posture` 系统账号与服务用户组（`video`）
4. 建状态目录 `/var/lib/posture-guard/` 与运行时目录 `/run/posture-guard/`
5. 把扬声器音量调到可用值（出厂 -6dB 听不清）
6. 装并 `enable` systemd 服务，立即启动

### 3.5 自检

```bash
adb shell 'bash /opt/posture-guard/scripts/verify.sh'
# ================ 结果：PASS=17  FAIL=0  WARN=0 ================
```

### 3.6 装全屏界面

```bash
# 用桌面登录用户跑，不要 sudo
adb shell 'sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 \
  bash /opt/posture-guard/scripts/install_ui_autostart.sh'
```

装完会在 `~/.config/autostart/` 写一个自启项，登录后自动全屏启动。

### 3.7 首次使用：两件必做的事

**① 先定焦**

```bash
adb shell 'bash /opt/posture-guard/scripts/focus.sh --scan'
```

自动对焦摄像头的连续 AF 会让画面持续轻微失焦（实测清晰度差 8 倍），而且出厂默认焦点往往不在工位距离上。扫出最佳值写进 `config/posture.yaml` 的 `focus_absolute` 后重启服务。

**② 再校准**

让人坐到摄像头前，保持正常坐姿 **5 秒**，程序自动采集基准并保存。

**机位要求**：`frame_check.py` 会给出逐项提示，必须看到 ✅：

```bash
adb shell 'python3 /opt/posture-guard/scripts/frame_check.py'
```

要求：头顶留白 ≥6%、人在画面内不贴边、肩部完整入画。**机位不对是判定失真的头号原因**。

### 3.8 采姿势样本（可选，用于提高精度）

系统用「逐姿势画像」做判定，规则阈值来自真实采集样本：

```bash
adb shell 'python3 /opt/posture-guard/scripts/pose_capture.py --guided'
```

引导式走完 6 个姿势，每个姿势采 6 秒 → 自动重算 `postures.txt` 规则。

---

## 4. 常用运维

```bash
systemctl status posture-guard          # 服务状态
sudo systemctl restart posture-guard    # 重启
bash /opt/posture-guard/scripts/status.sh        # 看实时状态
bash /opt/posture-guard/scripts/status.sh --watch # 每秒刷新
sudo journalctl -u posture-guard -f     # 跟日志
```

启动全屏界面（不重启）：

```bash
sudo -u quectel XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  systemctl --user start "app-posture\x2dguard\x2dui@autostart.service"
```

卸载（保留数据）：

```bash
sudo bash scripts/deploy.sh --uninstall
sudo bash scripts/deploy.sh --purge     # 连数据一起删
```

---

## 5. 判定是怎么做的

### 5.1 核心思路：相对个人基准，不搞标准角度

**先认识你，再管你。** 第一次坐下时采集 5 秒正常坐姿作为基准，之后所有判据都是「你偏离自己多少」。

所有几何量都除以**肩宽**归一化，所以人走近走远数值基本不变。

### 5.2 特征（7 维）

```
head_drop    头低量      耳中点比基准低多少肩宽
ear_tilt     耳线歪      耳线角相对肩线角的偏差
shoulder_tilt 肩线歪     肩线角相对基准的偏差
size         靠近倍数    当前肩宽占比 / 基准肩宽占比
wrist_rise   抬手量      手腕比基准腕高抬了多少
wrist_near   手离鼻      手腕到鼻子的距离
wrist_face   手离耳      手腕到耳中点的距离
```

### 5.3 有序级联判定

规则按优先级依次试，第一个命中的就是结论。**顺序由数据逼出来**：

1. `too_close` 凑近
2. `hand_face` 托腮
3. `head_tilt` 歪头
4. `head_down` 低头/趴桌（带互斥门：耳线已明显歪就不判低头）
5. `shoulder_tilt` 高低肩

阈值不是手调的，是采集样本后自动算出的（正常类中位数与目标类中位数之间的最优分割点）。

### 5.4 两层防抖：既快又稳

| 层 | 机制 |
|---|---|
| 界面判定 | **时间窗投票**：最近 0.6s 内 ≥80% 帧支持某标签就切换（退出用 1.2s / 70%）|
| 语音播报 | **连续 10 个有效不良帧**才播，避免和界面共用延迟 |

为什么不用「连续保持 N 秒」：那个模型下**一个噪声帧就把计时清零**，N 必须设得很大才稳，延迟和精度被绑死。投票制下单帧噪声只影响一票，实测确认时间从 1.5~2.5 秒降到 **0.2 秒**，抗噪反而更好。

### 5.5 其他

- **5.4 迟滞**：只给「退出」加惰性，进入仍用原阈值（不降低灵敏度）
- **3 帧中值平滑**：压肩宽和耳线角的单帧跳动
- **多人保护**：检出多人时不做个体判定，进入 UNKNOWN
- **隐私硬约束**：`save_raw_video` / `save_face_images` 一旦为 true，**程序拒绝启动**

---

## 6. 实测数据

| 项 | 值 |
|---|---|
| NPU 推理 | **~17.4 ms/帧**（对比 ONNX CPU 331–401ms，快 16–19×）|
| 端到端帧率 | **24.9 fps**（真实服务 + UI 同时运行）|
| 识别准确率 | **114/114 = 100%**（同基准标注样本，六类全对）|
| 姿势确认延迟 | **0.2 秒**（含噪声 0.33 秒）|
| 切换 → 语音播报 | **0.52 秒** |
| 关键点精度 | 与 ONNX fp32 逐点比对，平均 12–17px |
| 内存占用 | ~51 MB |
| 启动延迟 | `systemctl restart` → 出帧 < 2s |
| 自检 | 17 项 PASS |
| 单元测试 | 6 个可执行，全通过 |

> 帧率受摄像头、光照、系统负载影响。准确率来自标注样本回放，不等于任何场景下的真人识别率。

---

## 7. 目录

```
CMakeLists.txt              # 板端原生编译（OpenCV + librknnrt）
include/posture/*.hpp       # 数据契约与模块接口
src/
  main.cpp                  # 线程编排、参数、静态图/离线送帧模式
  CameraCapture.cpp         # V4L2 采集 + 指数退避重连
  RknnPoseEngine.cpp        # RKNN 推理 + DFL/NMS/关键点解码
  PostureAnalyzer.cpp       # 几何特征 + 时间窗投票 + 主用户选择
  PostureProfile.cpp        # 逐姿势画像（级联判定 + 迟滞）
  UserCalibration.cpp       # 个人基准（中位数 + 离群过滤）
  EyeCareMonitor.cpp        # 20-20-20 会话状态机
  ReminderManager.cpp       # 帧计数防抖 / 冷却 / 合并 / 免打扰
  Notifier.cpp              # journald + 状态/视频/事件 socket + JSONL 统计
  Config.cpp Yaml.cpp Json.cpp Logger.cpp Types.cpp
ui/posture_guard_ui.py      # GTK4 全屏界面（用户会话，订阅 socket）
config/posture.yaml         # 运行配置
systemd/posture-guard.service
scripts/                    # 部署、诊断、采集、评估脚本
tests/                      # 单元测试 + ONNX 比对
docs/                       # 架构 / 部署 / 校准 / 测试 / 排错
models/                     # yolov8n-pose.rknn (w8a8)
vendor/rknn/rknn_api.h      # 与板端 runtime 2.3.2 配套
assets/voice/*.wav          # 6 条神经女声
evidence/                   # 实测证据与复现命令
tools/board-deps.tgz        # 板端离线编译依赖
```

---

## 8. 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 进程/线程模型、数据契约、判定规则、状态机 |
| [docs/deploy.md](docs/deploy.md) | 离线依赖、编译、部署、目录、运维、卸载、交叉编译 |
| [docs/calibration.md](docs/calibration.md) | 校准流程、质量门限、阈值调法 |
| [docs/testing.md](docs/testing.md) | 单元测试、M2 验收、性能、TC-01~14 |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 真实踩过的坑 |
| [docs/posture-evaluation.md](docs/posture-evaluation.md) | 姿势评估方法与结果 |
| [docs/profile-recovery.md](docs/profile-recovery.md) | 画像文件丢失后的恢复 |

---

## 9. 设计取舍

1. **C++ 而不是 Python**：板端要跑满 20+ fps 推理 + 全屏绘制，Python 的 GIL 与解释开销没必要承担。
2. **板端原生编译而不是交叉编译**：板端已有 OpenCV 头文件和 RKNN 运行时，只缺编译器。补编译器比对齐「工具链 + sysroot + OpenCV + glibc」四件套便宜得多。
3. **后台服务无界面 + 用户会话 UI**：systemd 系统服务拿不到图形会话权限，混在一起会在自动登录/多用户/无屏场景下不稳定。
4. **界面上显示真实画面**：后台把帧压成 JPEG（640 宽 / 10fps / q70）沿 socket 发给界面。**没有画面就没法调机位**，而机位不对正是判定失真的头号原因。不想显示可 `ui.stream_video: false`。
5. **摄像头参数靠协商，不靠硬编码**：先 `VIDIOC_ENUM_*` 问清设备支持什么，再挑最接近的一组。
6. **不引入第三方库**：YAML/JSON 都是内置极小子集实现，因为板端没有这些包、也没有外网。
7. **语音离线烘焙，不做运行时合成**：只在制作素材时联网一次，产物是标准 24kHz WAV，板端零新依赖（还是 `paplay`）。

---

## 10. 语音素材的重新生成

当前语音是 `zh-CN-XiaoxiaoNeural` 神经女声（语速 -8%、音调 +6Hz），预生成 6 条 WAV。

```bash
pip install edge-tts                      # 主机一次性
export https_proxy=http://127.0.0.1:7897  # 需要代理时
python3 scripts/make_voice_assets_neural.py            # 女声（推荐）
python3 scripts/make_voice_assets_neural.py --check    # 只校验现有 wav
```

无网 / 装不上 edge-tts 时用机械音兜底，产物同名同格式、可随时互换：

```bash
python3 scripts/make_voice_assets.py      # libespeak-ng 兜底
```

---

## 11. 状态

**已交付**：源码、部署脚本、配置、**6 个单元测试**（`ctest` 6/6 通过）、文档、实测证据。

**已实测**：板端编译、17 项自检、静态图与 ONNX 逐点比对、真实摄像头端到端 24.9fps、全屏界面运行。

**待完成**：
- 24 小时连续运行（TC-12）
- 真实工作场景误报率统计
- 多用户基准支持

**已知问题**：
- 现场取景对判定影响极大。头顶出框会导致手腕不在画面内，托腮判不出来——这是物理摆位问题，不是算法问题。
- **两套阈值并存，不要混用**：
  - `config/posture.yaml` 里的是**工程初始值**（人工设定，带注释说明依据），用于无画像文件时的传统规则路径
  - `/var/lib/posture-guard/postures.txt` 是 `pose_capture.py --guided` 采样本后**从数据算出来的**级联画像规则，有画像文件时优先用它

  模型升级或换机位后必须重新采样本，否则画像规则会与新的关键点分布错位。
