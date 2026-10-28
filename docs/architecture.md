# 系统架构

> 实现依据：《RK3576 坐姿检测与护眼提醒_完整实施方案》§3（软件架构）、§4（算法）、
> §5（护眼业务）、§6（模块接口）、§11（数据结构与状态机）。
> 本文只描述**已经实现并实测**的内容；未实现的部分在末尾「边界」里明确列出。

## 1. 进程拓扑

```
┌──────────────────────────────────────────────────────────────────────┐
│ posture-guard.service   (systemd 系统服务, User=posture, 无图形会话)  │
│                                                                      │
│  CaptureThread ──最新帧槽(容量1)──> MonitorThread                    │
│   V4L2/OpenCV                         ├─ RknnPoseEngine   (NPU)     │
│   指数退避重连                        ├─ PostureAnalyzer  (几何+迟滞)│
│                                       ├─ EyeCareMonitor   (20-20-20) │
│                                       ├─ ReminderManager (冷却/合并) │
│                                       ├─ StatsStore       (JSONL)    │
│                                       └─ Notifier ─┐                 │
└─────────────────────────────────────────────────── │ ────────────────┘
                                                     │ AF_UNIX (JSON 行)
                                                     │ /run/posture-guard/status.sock
┌────────────────────────────────────────────────────▼───────────────┐
│ posture_guard_ui.py  (用户会话, GTK4, 全屏)                        │
│  订阅状态 / 接收通知 / 画骨架 / 底部按钮（校准·免打扰·已休息）     │
└────────────────────────────────────────────────────────────────────┘
```

**为什么拆成两个进程**：systemd 系统服务拿不到 `DISPLAY` / `WAYLAND_DISPLAY` 权限，
在自动登录、多用户、无屏场景下会不稳定（实施方案 §13.3）。拆开后后台服务完全无界面，
界面挂了也不影响计时与检测。

**界面拿到的画面**：socket 上有两类消息 ——
`{"type":"status"}`（每 0.5s，含 17 个归一化关键点）与
`{"type":"video"}`（默认 10fps，`<w>x<h>` + base64 JPEG）。
界面用 `Gtk.Picture` 显示视频、透明 `Gtk.DrawingArea` 叠骨架。

画面**只在本机内存与本地 unix socket 里流转**：不落盘、不出网，
`privacy.save_raw_video` / `save_face_images` 仍强制为 false（§18.2）。
不想要画面就设 `ui.stream_video: false`，界面退回纯骨架模式。

## 2. 线程模型

| 线程 | 职责 | 频率 |
|---|---|---|
| capture | V4L2 取帧、写「最新帧槽」 | 摄像头原生（本机 15–30fps，实测 20fps） |
| monitor | 抽帧推理 → 规则 → 计时 → 通知 → 推送状态 | 推理上限 `camera.infer_fps`（默认 8） |
| main | 信号、退出编排、`--run-seconds` | — |

两条硬规则：

1. **视频流绝不排队**。帧槽容量为 1，推理慢就覆盖旧帧（§14.3）。实测 NPU 一帧 17–25ms，
   采集 50ms 一帧，永远不会积压。
2. **业务计时用 `steady_clock`**，与 FPS 无关。即使推理某帧失败、摄像头短暂断流，
   20 分钟用眼计时也不会跑偏（§3.2）。日志与统计另用墙上时钟。

## 3. 数据契约

`include/posture/Types.hpp` 是唯一的公共数据结构：

| 结构 | 说明 |
|---|---|
| `Frame` | BGR 图 + `frame_id` + `steady_clock` 时间戳 + 墙上时间 |
| `PersonPose` | 人体框 + 17 关键点（原图坐标）+ 人物置信度 |
| `PostureMetrics` | 肩线角度、肩宽、头肩归一化偏移、宽度比、手腕-鼻子距离 |
| `PostureObservation` | 状态 + 指标 + **本帧新激活的规则** + 仍激活的规则 |
| `Baseline` | 个人校准基准 + 摄像头 ID + 分辨率 + 模型 SHA256 |

**三态分离**（§6.3 强制要求）：`NORMAL`（姿态正常）、`USER_AWAY`（画面里没人）、
`UNKNOWN`（有人但关键点不可信/多人/机位不对）是三件不同的事，绝不混用。

## 4. 姿态判定

### 4.1 关键点有效性

w8a8 量化模型的关键点 `conf` 通道被整体量化摧毁（scale≈3.19，恒为 0），
因此**关键点可信度用人物框置信度做代理**，并且加了三道几何闸门
（都在 `PostureAnalyzer::ComputeMetrics`）：

1. 鼻点、双肩点必须高于 `joint_confidence`；
2. 肩宽 `>= 24px`；
3. **取景门**：鼻子不能贴画面上边缘、双肩不能贴边、鼻子到肩线的距离要
   `>= 0.22 × 肩宽`。不满足就判 `UNKNOWN` 并给出人话原因（例如
   「头顶贴到画面上边缘（摄像头偏低或人太近），请把摄像头抬高」）。

第 3 条是现场调试时加的：机位不对时几何量完全失真，用它校准会得到一个畸形基准，
之后每次坐正都会误报。

### 4.2 特征量

| 特征 | 定义 | 用途 |
|---|---|---|
| `shoulder_angle_deg` | `atan2(dy, dx)`，折叠到 `[-90°, 90°)` | 高低肩 |
| `shoulder_dev_deg` | 与个人基准的角差，同样做 180° 周期折叠 | 高低肩（判据） |
| `head_offset_y` | `(鼻子Y − 肩中点Y) / 肩宽`（负值，鼻子在上方） | 低头（近似量） |
| `head_axis_deg` | 肩中点→鼻子连线相对竖直方向的偏角 | 歪头（近似量） |
| `width_ratio` | `肩宽 / 画面宽` | 离屏太近（趋势，不是物理距离） |
| `wrist_to_nose` | `min(左右手腕到鼻子距离) / 肩宽` | 托腮 |

除 `shoulder_dev_deg` 外都是**归一化到肩宽**的量，人走近走远时数值基本稳定（§4.3）。

### 4.3 规则与迟滞

`HoldRule` 实现「持续 N 秒才激活、恢复到 M 秒才解除」的通用状态机（§11.3）：

| 规则 | 触发 | 恢复 | 持续 | 优先级 |
|---|---|---|---|---|
| 高低肩 `shoulder_tilt` | 偏离基准 > 12° | < 8° | 8s（恢复 3s） | 40 |
| 低头/趴桌 `head_down` | 头肩纵向比基准低 > 0.35 肩宽 | < 0.175 | 5s | 50 |
| 歪头 `head_tilt` | 头轴偏角相对基准 > 18° | < 10.8° | 5s | 30 |
| 离屏太近 `too_close` | 肩宽占比 > 0.75 | < 0.67 | 5s | 20 |
| 托腮 `hand_on_face` | 手腕-鼻子 < 0.95 肩宽 | > 1.19 | 5s | 60 |

> 高低肩/低头/歪头**必须先有个人基准**；没有基准时只跑距离和托腮两条，
> 然后在状态里明确写「未校准」，绝不假装判过了（§4.6 / §11.4）。

### 4.4 主用户选择

1. 过滤：框面积、置信度、ROI、几何合理性；
2. **按 IoU/中心距去重**：w8a8 的 conf 通道很粗，同一个人会被多尺度重复检出，
   框中心抖动几十像素、IoU 达不到 NMS 阈值。不去重会把一个人判成「多人」；
3. 去重后仍有两个人尺寸接近 → `ambiguous`，不做个体判定（§11.4 / TC-11）。

## 4.5 摄像头参数协商

不硬编码采集参数，先问设备（`src/V4l2Probe.cpp`）：

```
VIDIOC_QUERYCAP → 是不是采集节点（/dev/video74 这类 metadata 节点会被排除）
VIDIOC_ENUM_FMT → 支持的像素格式
VIDIOC_ENUM_FRAMESIZES → 每个格式的尺寸
VIDIOC_ENUM_FRAMEINTERVALS → 每个尺寸的帧率
        ↓
ChooseCapture(): 格式优先压缩(JPG省 USB 带宽) → 尺寸取最接近面积 → 帧率取不超期望的最大值
        ↓
cap.set(FOURCC) 再 cap.set(WIDTH/HEIGHT)     ← 顺序不能反
        ↓
日志记录「请求值 vs 实际值」，只有真不一致才 WARN
```

本机这块 icSpring 只给 `YUYV 640x480 @ 30/25/20/15/10/5fps`，一个格式一个分辨率。
日志：`摄像头能力：icspring camera [YUYV(640x480) ]` +
`实际 640x480@20fps 格式 YUYV（请求 640x480 YUYV）`。

## 5. 护眼业务

状态机（§5.2 原文实现，`src/EyeCareMonitor.cpp`）：

```
ABSENT  --在座连续 2s-->  ACTIVE（开始累计）
ACTIVE  --累计 >= 1200s--> BREAK_DUE（发一次休息提醒）
ACTIVE  --无人连续 10s-->  ABSENT（会话结束、清零）
BREAK_DUE --用户点「已休息」/ 自动策略满 20s--> ACTIVE（重新计时）
BREAK_DUE --无人连续 10s--> ABSENT
```

`reminder_policy` 支持两种：`manual_ack_or_session_reset`（默认，等人确认）
和 `auto_restart`（休息满 `far_look_s` 自动开始新一轮）。

**明确不做的事**：不去判断「用户是否真的在看远处」。摄像头看到人在这里，
不能证明眼睛放松了。所以提醒是提醒，不是测量（§5.1）。

## 6. 提醒去重

`ReminderManager`：

* 同类提醒 `notification.cooldown_s`（默认 60s）内不重复；
* 同一帧多条规则同时成立 → 合并成一条通知，取优先级最高的作标题；
* 「忽略 15 分钟」全局免打扰（`scripts/snooze.sh` 或界面按钮）。

## 7. 输出

| 通道 | 内容 | 位置 |
|---|---|---|
| journald | 生命周期、校准、告警、错误 | `journalctl -u posture-guard` |
| AF_UNIX | `{"type":"status",...}` 每 0.5s；`{"type":"video",...}` 10fps；`{"type":"notification",...}` 即时 | `/run/posture-guard/status.sock` |
| JSONL 统计 | 每 10s 一条 tick + 每次告警一条 alert | `/var/lib/posture-guard/stats/stats-YYYY-MM-DD.jsonl` |

界面命令（客户端 → 服务）：`{"cmd":"ack"}`、`{"cmd":"snooze","minutes":15}`、
`{"cmd":"clear_snooze"}`、`{"cmd":"calibrate"}`。

## 8. 配置

`/etc/posture-guard/posture.yaml`，解析器是内置的 YAML 子集实现
（`src/Yaml.cpp`，只支持本项目用到的写法：嵌套 map、标量、行内数组、注释）。
不用 yaml-cpp/nlohmann，因为板端没有这些包也没有外网。

隐私红线：`privacy.save_raw_video` / `save_face_images` 只要有一个是 `true`，
程序**直接拒绝启动**（§18.2）。

## 9. 边界（本文档不吹的部分）

* 正面单目摄像头**测不出脊柱曲度**，也不测真实的低头角度。`head_down`、`head_tilt`
  是肩宽归一化的近似量，界面与日志都标了 approx 的含义（§1.1 / §16.7）。
* `width_ratio` 只是「离屏过近趋势」，不是厘米距离。要真实距离需要 ToF（V2）。
* 没做人脸 Landmark、没有相机标定、没有 ToF/环境光。
* 视频流是 base64 编码进 JSON 的（简单可靠，但比裸二进制多 33% 带宽）；
  640×480 q70 ≈ 540KB/s，本机 unix socket 完全够，换到网络传输要改成二进制帧。
* 没做 Qt 界面，界面是 GTK4 单进程（同一台一体机够用）。
* 多人场景只支持「锁定主用户」，不支持同时判定多个人。
