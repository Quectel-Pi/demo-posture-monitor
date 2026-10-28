# 坐姿检测与护眼提醒一体机（PostureMonitor）

> DEMO-036 · Quectel Pi M2 (RK3576) · Debian 13 · 人体 17 关键点 + 坐姿判定 + 屏幕提醒 + 时长统计
>
> 关联需求表：QuectelPi Demo 开发计划 · `DEMO-036`「坐姿检测与护眼提醒一体机」
> 实施日期：2026-10-28 · 目标板：`QSM610ZAP-WFPI` / `SH603ZA`

---

## 1. 一句话

插上 USB 摄像头，上电自动全屏：实时骨架叠加 → 判托腮 / 低头趴桌 / 高低肩（另加歪头 /
前倾 / 离屏太近 / 久坐）+ 20-20-20 护眼 → 不合格弹横幅 + 提示音 → 全程累计时长与次数落库。

**实测指标（本机实测，非估算）**

| 项 | 数值 | 说明 |
|---|---|---|
| 姿态推理 | **20.1 ms/帧**（50 次中位数，p90 20.4） | RKNN w8a8 on NPU（`librknnrt` 2.3.2 + RKNPU 驱动 0.9.8）；界面同时跑时 23–34 ms |
| 显示帧率 | **29–30 fps** | 采集 25fps，骨架按推理节奏叠加（不做逐帧推理）；进程 RSS 278 MB |
| 关键点精度 | **平均 4.9–6.9 px**（640 空间） | 与 ONNX fp32 参考实现逐点对齐（`tests/verify_decode.py`） |
| CPU 备援 | 331 ms/帧（6 线程） | ONNX Runtime fp32；**NPU 快 16.5×**（无 NPU 时自动退化） |
| 摄像头 | 640×480@30 YUYV，44 ms/帧（22.7fps） | USB UVC（`icSpring`） |

---

## 2. 交付物

```
PostureMonitor/
├── src/posturemon/            # 应用源码（Python，GTK4 + cairo + RKNN）
│   ├── app.py                 # GTK4 入口
│   ├── window.py              # 全屏界面：视频+骨架 / 状态面板 / 告警横幅 / 护眼全屏
│   ├── pipeline.py            # 采集→推理→判定 三线程流水线
│   ├── backends.py            # RKNN(NPU, ctypes 直调) / ONNX(CPU) 双后端
│   ├── yolopose.py            # YOLOv8-pose 后处理（DFL 解码 / split-head / NMS）
│   ├── rules.py               # 几何指标 + 6 条规则 + 迟滞状态机
│   ├── camera.py              # V4L2 采集线程（只保留最新帧）
│   ├── overlay.py             # cairo 骨架/人体框绘制
│   ├── stats.py               # SQLite 时长与违规统计
│   ├── config.py              # 全部阈值/参数（可被 /etc 覆盖）
│   └── headless.py            # 无界面模式 + 规则自检
├── scripts/
│   ├── fetch-model.sh         # 【开发机】下载模型 + aarch64 ORT wheel
│   ├── deploy-from-host.sh    # 【开发机】一条命令推送 + 安装
│   ├── install.sh             # 【板端】装代码/配置/自启/GDM 自动登录
│   ├── verify.sh              # 【板端】21 项自检
│   ├── launcher.sh            # 崩溃自愈启动器
│   ├── screenshot.sh          # kmsgrab 抓真实屏幕（Mutter 下唯一可行）
│   ├── make-assets.py         # 生成提示音
│   └── uninstall.sh           # 卸载（--purge 连数据一起删）
├── tests/verify_decode.py     # NPU vs ONNX 关键点对齐回归
├── docs/                      # 需求映射 / 部署 / 判定规则 / 模型与后端 / 排障
└── samples/                   # 离线回归样图
```

---

## 3. 五分钟部署

### 3.1 开发机（有外网）

```bash
git clone <repo> PostureMonitor && cd PostureMonitor
bash scripts/fetch-model.sh          # 拉 rknn + onnx 模型，约 21MB
```

### 3.2 推到板子并安装

```bash
adb devices                          # 确认 M2 已连
bash scripts/deploy-from-host.sh
```

等价的手工版（板端离线也可用）：

```bash
adb push . /tmp/posture-src && adb shell
cd /tmp/posture-src && sudo bash scripts/install.sh --user=quectel
```

### 3.3 生效

```bash
adb shell 'systemctl restart gdm'    # 触发 GDM 自动登录 → autostart 拉起应用
adb shell 'cat /run/posture-monitor/status.json'   # 不看屏也能确认在跑
```

### 3.4 自检

```bash
adb shell 'cd /tmp/posture-src && sudo bash scripts/verify.sh'
```

---

## 4. 使用时

| 操作 | 效果 |
|---|---|
| 上电 | 自动登录 → 全屏进监测界面 |
| 坐正 | 左侧画面显示绿色骨架 + 绿框 |
| 违规持续 2s | 面板状态变红、显示具体类型与指标 |
| 连续违规 60s | 顶部红色横幅 + 提示音 + 记一次违规 |
| 每 20 分钟 | 全屏「护眼休息」倒计时 20s（看 6 米外） |
| 连续坐 45 分钟 | 久坐提醒 |
| `R` | 手动触发一次护眼休息（演示用） |
| `F` | 全屏/窗口切换 |
| `Esc` ×2 | 退出程序（启动器不再拉起） |

面板实时显示：状态、连续不良时长、今日不良时长、连续坐姿时长、6 类违规今日次数、
护眼倒计时、后端与推理耗时。

---

## 5. 架构

```
USB UVC ──► camera 线程（只保留最新帧，覆盖式）
                │
                ▼
         infer 线程  ── letterbox 640×640 ──► RKNN(NPU) 18~25ms
                │                              └─ 退化：ONNX CPU 330ms
                ▼
        yolopose.decode ──► 17 关键点（COCO）
                │
                ▼
   rules：几何指标 → 6 条规则 → 迟滞状态机（hold 2s / clear 1.5s）
                │
      ┌─────────┴──────────┐
      ▼                    ▼
 GTK4 界面（30fps）    SQLite（时长/次数/事件）
```

设计要点：

- **推理与显示解耦**：显示永远用最新帧（25–30fps），骨架按推理节奏（~12fps 上限，
  `infer_interval_ms=80`）叠加。姿态变化慢，不需要逐帧推理。
- **阈值全部相对化**：以肩宽 `sw` 为单位，换机位、换分辨率都不用重标定。
- **NPU 走 ctypes 直调**：板端无 pip、python3.13 也没有 rknn wheel，
  但 `librknnrt.so` 是系统自带的 —— 绑 5 个结构体 + 5 个函数即可，零安装。

---

## 6. 已知限制（诚实版）

| 限制 | 影响 | 现状/对策 |
|---|---|---|
| NPU 模型的**关键点置信度通道被量化摧毁** | 无法用 conf 过滤单点 | 量化 scale≈3.19 时 conf 只剩 1 个量化级（实测恒 0）；改用"人体框内 + 几何合理性"校验 |
| RKNN 量化后**误检略多** | 空场景偶尔检成 1 个人 | 加了 `reliable` 几何门 + 4 帧迟滞；`conf_threshold` 已从 0.35 提到 0.45 |
| **MIPI 摄像头未接通** | `/dev/video-camera1` 出流 `not-negotiated` | 当前用 USB UVC（已实测）；MIPI 见 `docs/05-troubleshooting.md` |
| 板子**无外网** | 不能 apt/pip | 模型与运行时从开发机 `adb push` |
| 板端 RTC 曾停在 2026-04-13 | 统计日期错 | 部署时对时：`adb shell date $(date +%m%d%H%M%Y.%S)` |
| 判定阈值未做真人标定 | 换环境可能要调 | 全部在 `/etc/posture-monitor/config.json`，调参见 `docs/03-posture-rules.md` |
| 需求文档指定 PeopleKptDet v1.2 SDK | 本次未使用 | 该 SDK 不在板端也不在仓库，改用 YOLOv8n-pose；接口已抽象成后端，换 SDK 只改 `backends.py` |

---

## 7. 实测证据

`evidence/` 目录是板端跑出来的原始记录，可直接贴汇报：

| 文件 | 内容 |
|---|---|
| `01-live-fullscreen.png` | 全屏界面实拍（kmsgrab 抓 DRM 真实输出） |
| `02-demo-skeleton.png` | 用离线样图驱动，可见骨架叠加 |
| `verify-output.txt` | 自检 15 项全 PASS |
| `bench-output.txt` | NPU 20.1ms / CPU 331ms（16.5×）基线 |
| `status-sample.json` | 运行态快照 |

---

## 8. 文档索引

| 文档 | 内容 |
|---|---|
| [docs/01-requirements.md](docs/01-requirements.md) | DEMO-036 需求逐条 → 实现映射 |
| [docs/02-deploy.md](docs/02-deploy.md) | 完整部署方案、目录布局、自启链路、回滚 |
| [docs/03-posture-rules.md](docs/03-posture-rules.md) | 6 条判定规则的公式、阈值、标定方法 |
| [docs/04-model-and-backends.md](docs/04-model-and-backends.md) | 模型来源、RKNN 输出布局、双后端切换、性能数据 |
| [docs/05-troubleshooting.md](docs/05-troubleshooting.md) | 本次踩到的 9 个坑与定位方法 |

---

## 9. 关键代码位置

| 想改什么 | 改哪里 |
|---|---|
| 判定灵敏度 | `/etc/posture-monitor/config.json`（无需改代码） |
| 新增一条坐姿规则 | `src/posturemon/rules.py`：`evaluate()` 加判断 + `VIOLATION_LABEL` 加名字 |
| 换算法/换 SDK | `src/posturemon/backends.py`：实现 `infer(rgb) -> PoseResult` 即可 |
| 界面配色/布局 | `src/posturemon/window.py` 顶部 `CSS` 常量 |
| 提醒策略 | `window.py` 的 `trigger_posture_alert()` / `start_rest()` |
