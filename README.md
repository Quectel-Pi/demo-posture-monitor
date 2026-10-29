# RK3576 坐姿检测与护眼提醒一体机

Quectel Pi M2（RK3576 / Debian 13 aarch64）上的坐姿检测 + 20-20-20 护眼提醒系统。
按《RK3576 坐姿检测与护眼提醒_完整实施方案》实现：**C++ 后台服务（NPU 推理）+ 用户会话
GTK4 界面**，两个进程通过本地 socket 通信。

> 项目编号：DEMO-036 ｜ 平台：Quectel Pi M2 (RK3576) ｜ 摄像头：USB UVC
>
> 健康声明：本系统只是习惯提醒工具，不是医疗设备。正面单目摄像头测不出脊柱曲度，
> 也不测真实的低头角度；所有「近似量」在界面与文档里都标了。

## 它做什么

| 能力 | 实现方式 | 状态 |
|---|---|---|
| 人体 17 关键点 | YOLOv8n-pose w8a8 跑 NPU（`librknnrt.so` 直调） | ✅ 实测 17.4ms/帧 |
| 高低肩 | 肩线相对**个人基准**偏 > 12° 持续 8s | ✅ |
| 低头/趴桌 | 头肩纵向比基准低 > 0.35 肩宽 持续 5s（近似量） | ✅ |
| 托腮 | 手腕-鼻子距离 < 0.95 肩宽 持续 5s | ✅ |
| 歪头 | 头轴相对基准偏 > 18° 持续 5s（近似量） | ✅ |
| 离屏过近 | 肩宽/画面宽 > 0.75 持续 5s（趋势，非厘米） | ✅ |
| 屏幕提醒 | 全屏**摄像头实时画面** + 骨架叠加 + 红/绿状态 + 顶部横幅 | ✅ |
| 摄像头适配 | 启动时 `VIDIOC_ENUM_*` 枚举格式/分辨率/帧率后协商，不硬编码 | ✅ |
| 焦距锁定 | 关掉连续自动对焦并锁实测值（清晰度 1.87×）；`scripts/focus.sh --scan` | ✅ |
| 累计时长 | 每日在座/不良秒数 + 提醒次数，JSONL 持久化 | ✅ |
| 20-20-20 护眼 | 连续用眼 20 分钟 → 全屏「远眺 20 秒」倒计时 | ✅ |
| 个人校准 | 自动采集 5 秒正常坐姿 → 中位数基准（带离群过滤） | ✅ |
| 无人暂停 | 离开 10s 结束会话清零 | ✅ |
| 断线自愈 | systemd `Restart=on-failure` + 摄像头指数退避重连 | ✅ |

不做（明确边界）：脊柱曲度、真实低头角、厘米级距离、人脸识别、云端上传、保存原始画面。

## 快速开始

```bash
# ── 主机侧：准备离线依赖（板端没有编译器和外网）
PROXY=http://127.0.0.1:7897 bash scripts/fetch_deps.sh
adb push tools/board-deps.tgz /userdata/

# ── 板端：装编译器 → 编译 → 测试
adb shell 'mkdir -p /userdata/board-deps && cd /userdata/board-deps && \
  tar xzf ../board-deps.tgz && dpkg -i --force-depends *.deb && \
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-14'
adb push . /userdata/posture-src   # 或用 tar 传
adb shell 'cd /userdata/posture-src && bash scripts/build_board.sh'

# ── 板端：部署 + 自检
adb shell 'cd /userdata/posture-src && bash scripts/deploy.sh'
adb shell 'bash /opt/posture-guard/scripts/verify.sh'
# ================ 结果：PASS=17  FAIL=0  WARN=0 ================

# ── 板端：装桌面全屏界面（自动登录后自动起）
adb shell 'sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 \
  bash /opt/posture-guard/scripts/install_ui_autostart.sh'
```

第一次使用：

1. **先定好焦**：`bash scripts/focus.sh --scan` 扫出最佳焦距，写进配置后重启服务。
   自动对焦摄像头的连续 AF 会让画面持续轻微失焦（实测清晰度差 8 倍），
   而且出厂默认焦点往往不在工位距离上（实测换焦距后清晰度 1.87×）。
2. 让人坐到摄像头前保持正常坐姿 5 秒，程序自动校准并保存基准。

## 实测数据

| 项 | 值 |
|---|---|
| NPU 推理 | **17.4 ms/帧**（15–25ms），对比 ONNX CPU 331–401ms，**快 16–19×** |
| 关键点精度 | 与 ONNX fp32 逐点比对：平均 **12–17 px**，IoU 0.876 / 0.588（zidane） |
| 推理帧率 | 6.9 fps（`camera.infer_fps: 8` 限流） |
| 采集 | 640×480 YUYV（该 UVC 不提供 MJPG） |
| 启动延迟 | `systemctl restart` → 出帧 < 2s |
| 自检 | 17 项 PASS（含视频流） |
| 单元测试 | 3 个可执行，全通过 |

## 目录

```
CMakeLists.txt              # 板端原生编译（OpenCV + librknnrt）
include/posture/*.hpp       # 数据契约与模块接口
src/
  main.cpp                  # 线程编排、参数、图像/离线送帧模式
  CameraCapture.cpp         # V4L2 采集 + 指数退避重连
  RknnPoseEngine.cpp        # RKNN 推理 + DFL/NMS/关键点解码
  PostureAnalyzer.cpp       # 几何特征 + 迟滞规则 + 主用户选择
  UserCalibration.cpp       # 个人基准（中位数 + 离群过滤）
  EyeCareMonitor.cpp        # 20-20-20 会话状态机
  ReminderManager.cpp       # 冷却 / 合并 / 免打扰
  Notifier.cpp                # journald + 状态/视频/事件 socket + JSONL 统计
  Config.cpp Yaml.cpp Json.cpp Logger.cpp Types.cpp
ui/posture_guard_ui.py      # GTK4 全屏界面（用户会话，订阅 socket）
config/posture.yaml         # 运行配置
systemd/posture-guard.service
scripts/                    # check_board / smoke_camera / build_board / deploy
                            # verify / status / calibrate / snooze / screenshot / focus
                            # install_ui_autostart / fetch_deps
tests/                      # test_posture_rules / test_session_timer / test_config
                            # compare_onnx.py（M2 验收）
docs/                       # architecture / deploy / calibration / testing / troubleshooting
models/                     # yolov8n-pose.rknn (w8a8) + onnx 参考模型
vendor/rknn/rknn_api.h      # 与板端 runtime 2.3.2 配套的头文件
evidence/                   # 截图、自检输出、实测指标
tools/board-deps.tgz        # 板端离线编译依赖（fetch_deps.sh 生成）
legacy/                     # 上一版 Python+GTK 单体实现（已停用，留作对照）
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 进程/线程模型、数据契约、判定规则、状态机、边界 |
| [docs/deploy.md](docs/deploy.md) | 离线依赖、编译、部署、目录、运维、卸载 |
| [docs/calibration.md](docs/calibration.md) | 校准流程、质量门限、阈值怎么调、现场看指标 |
| [docs/testing.md](docs/testing.md) | 单元测试、M2 验收、性能、TC-01~14 结果 |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 13 个真实踩过的坑 |
| [evidence/README.md](evidence/README.md) | 实测证据与复现命令 |

## 设计取舍

1. **C++ 而不是 Python**：板端要跑满 8fps 推理 + 全屏 cairo 绘制，Python 的 GIL 与
   解释开销没必要承担；而且实施方案的模块划分本来就是 C++ 的。
2. **板端原生编译而不是交叉编译**：板端已有 OpenCV 头文件与 RKNN 运行时，
   只缺编译器；补编译器比对齐「交叉工具链 + sysroot + OpenCV + glibc」四件套便宜得多。
3. **后台服务无界面 + 用户会话 UI**：systemd 系统服务拿不到图形会话权限，
   混在一起会在自动登录/多用户/无屏场景下不稳定（实施方案 §13.3 也是这个结论）。
4. **界面能看到真实画面**：后台把采集帧压成 JPEG（默认 640 宽 / 10fps / q70）沿本地
   socket 发给界面，界面用 `Gtk.Picture` 显示、透明层叠骨架。图像不落盘不出网，
   不想要画面就 `ui.stream_video: false` 退回纯骨架模式。**没有画面就没法调机位**，
   而机位不对正是判定失真的头号原因。
5. **摄像头参数靠协商，不靠硬编码**：先 `VIDIOC_ENUM_*` 问清设备支持什么，再挑最接近的
   一组（本地这块 UVC 只有 YUYV 640×480，连 MJPG 都没有）。
6. **不引入第三方库**：YAML/JSON 都是内置的极小子集实现，因为板端没有这些包、也没有外网。
7. **规则全部「相对个人基准」**：不搞「标准坐姿角度」，因为机位和人都会变。

## 状态

- 已交付：源码、部署脚本、配置、单元测试、文档、证据
- 已实测：板端编译 / 16 项自检 / 静态图与 ONNX 逐点比对 / 真实摄像头端到端 / 全屏界面截图
- 待做：24 小时连续运行（TC-12）、真实工作场景误报率统计、多用户基准
