# 实测证据（2026-10-28，板端实测，非估算）

| 文件 | 内容 |
|---|---|
| `01-live-fullscreen.png` | 一体机全屏界面实拍（kmsgrab 抓 DRM 真实扫描输出，1280×800） |
| `02-demo-skeleton.png` | 用离线样图 `samples/zidane.jpg` 驱动，可见两人骨架叠加 |
| `status-sample.json` | `/run/posture-monitor/status.json` 运行态快照 |
| `verify-output.txt` | `scripts/verify.sh` 全量输出：**PASS=15 FAIL=0 WARN=0** |
| `bench-output.txt` | `scripts/bench.sh` 性能基线（NPU vs CPU 各 50 次） |

## 性能（`bench-output.txt` 原样）

```
== 环境 ==
Debian GNU/Linux 13 (trixie)   内核 6.1.118-rt36   nproc 8   mem 3.8 GB
librknnrt 2.3.2                RKNPU driver v0.9.8

== 推理后端（50 次，640x640 输入）==
  RKNN w8a8 @ NPU            median   20.1 ms   p90   20.4 ms   min   20.0   fps  49.8
  ONNX fp32 @ CPU 4 线程       median  401.6 ms   p90  402.6 ms   min  400.5   fps   2.5
  ONNX fp32 @ CPU 6 线程       median  331.1 ms   p90  334.5 ms   min  329.4   fps   3.0
  ONNX fp32 @ CPU 8 线程       median  366.7 ms   p90  368.4 ms   min  363.2   fps   2.7
  ONNX int8 @ CPU 6 线程       median  536.0 ms   p90  538.2 ms   min  525.6   fps   1.9

  NPU 相对 CPU(6 线程) 加速: 16.5x

== 摄像头 ==
  640x480 YUYV  median 44.0 ms  fps 22.7
```

> 说明：应用运行中（+GTK 界面 +采集线程同时跑）实测 23–34 ms，纯推理基线 20.1 ms。

## 关键点精度（`tests/verify_decode.py`）

RKNN 与 ONNX fp32 逐点对齐（同一 letterbox 输入、按 IoU 配对）：

```
zidane.jpg: RKNN 2 / ONNX 2 person(s)
  [PASS] #0 IoU=0.876 关键点平均误差 5.5px
  [PASS] #1 IoU=0.588 关键点平均误差 5.2px
bus.jpg:    RKNN 8 / ONNX 3 person(s)
  [PASS] #0 IoU=0.510 关键点平均误差 6.9px
  [PASS] #1 IoU=0.532 关键点平均误差 5.8px
  [PASS] #3 IoU=0.564 关键点平均误差 4.9px
```

平均误差 4.9–6.9 px（640 输入空间），即 ≈0.8%–1.1% 画面宽度。

## 自检（`verify-output.txt` 摘要）

```
== 1. 系统与依赖 ==    python 3.13.5 | cv2 4.10.0 | numpy 2.2.4 | Gtk 4.18 | pycairo 1.27.0
                       librknnrt.so version 2.3.2
== 2. 模型与后端 ==    rknn(NPU) input=640x640 sdk=2.3.2 median=27.2ms
== 3. 判定规则 ==      6 条规则状态机 6 项通过（合成关键点，不需人和摄像头）
== 4. 摄像头 ==        应用独占中：fps=28.8 推理=34.2ms
== 5. 端到端 ==        rknn(NPU) | 人员=1 | 在座=True | 不良=False
== 6. 安装状态 ==      代码/配置/数据库/自启/GDM 自动登录/进程 全部 PASS
================ 结果：PASS=15  FAIL=0  WARN=0 ================
```

## 运行态样本（`status-sample.json`）

```json
{"ts": 1793202366.5, "window": [1280, 800], "fullscreen": true,
 "backend": "rknn(NPU)", "sdk": "2.3.2 (429f97ae6b@2025-04-09T09:09:27)",
 "latency_ms": 32.5, "persons": 2, "fps": 29.4, "present": true,
 "bad": false, "label": "", "bad_duration_s": 0.0, "today_bad_s": 0.0,
 "counts": {}, "rest_active": false, "banner": ""}
```

字段含义：`window/fullscreen` 证明界面真的全屏；`fps` 是显示帧率；
`latency_ms` 是 NPU 单帧耗时；`present/bad/label` 是判定结果；
`today_bad_s`/`counts` 是当日累计（来自 SQLite）。

## 复现命令

```bash
adb shell 'cd /tmp/posture-src && sudo bash scripts/verify.sh'
adb shell 'cd /tmp/posture-src && bash scripts/appctl.sh stop'
adb shell 'cd /tmp/posture-src && sudo bash scripts/bench.sh'
adb shell 'cd /opt/posture-monitor && PYTHONPATH=. python3 tests/verify_decode.py'
adb shell 'cd /tmp/posture-src && bash scripts/appctl.sh start'
```

> ⚠️ 应用运行时会独占 UVC 摄像头，所以摄像头/端到端两项会自动改用
> `status.json` 佐证；想直接测设备就先 `appctl.sh stop`。
