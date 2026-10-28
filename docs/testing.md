# 测试与验收

> 依据：实施方案 §15（测试计划与验收标准）、§9.7（官方静态图推理）、§10.1（集成顺序）
>
> 本文记录的都是**本机实际跑过**的结果，不是目标值。

## 0. 一键全跑

```bash
# 板端：编译 + 单元测试
bash scripts/build_board.sh

# 板端：部署 + 自检（16 项）
sudo bash scripts/deploy.sh
bash scripts/verify.sh

# 主机：静态图与 ONNX 参考比对（M2 验收）
adb shell '/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
  --image /userdata/build-src/bus.jpg --dump-keypoints /tmp/bus_cpp.json'
adb pull /tmp/bus_cpp.json .
python3 tests/compare_onnx.py samples/bus.jpg bus_cpp.json
```

## 1. 单元测试（无需摄像头 / NPU）

`ctest --test-dir build`，3 个可执行文件：

| 测试 | 覆盖内容 |
|---|---|
| `test_posture_rules` | 肩线角度 180° 周期折叠；`HoldRule` 迟滞（TC-05 瞬间倾斜不报警 / TC-06 恢复迟滞 / 激活后不重复 just_activated）；缺失关键点 → 无效；取景门（头贴顶、肩贴边）；ROI 外框排除；同一人重复框合并；真两人 → ambiguous；无基准时不触发高低肩/低头 |
| `test_session_timer` | 在座确认 2s；20 分钟只触发一次（TC-08）；离开 10s 结束会话（TC-09）；暂停不计时；同类提醒 60s 冷却、多条规则合并、免打扰 15 分钟 |
| `test_config` | YAML 子集解析（嵌套、行内数组、引号内 `#`、缺省值）；真实 `posture.yaml` 每个键都读对；`privacy.save_raw_video=true` 必须拒绝启动 |

结果：

```
1/3 Test #1: posture_rules .... Passed
2/3 Test #2: session_timer .... Passed
3/3 Test #3: config ........... Passed
100% tests passed, 0 tests failed out of 3
```

## 2. M2 验收：静态图 vs ONNX fp32 参考

`tests/compare_onnx.py` 用 ONNX 官方 fp32 模型当参考，按 IoU 匹配人体后逐点算关键点误差。

本机实测（`models/yolov8n-pose.rknn` w8a8，`models/yolov8n-pose.onnx` fp32）：

| 样本 | ONNX 参考人数 | RKNN 人数 | 匹配率 | IoU | 关键点平均误差 | 判据 |
|---|---|---|---|---|---|---|
| `samples/zidane.jpg` | 2 | 2 | 100% | 0.876 / 0.588 | **17.2 px** | PASS |
| `samples/bus.jpg` | 3 | 8 | 100% | 0.44 / 0.33 / 0.48 | **17.2 px** | PASS |
| 现场帧（1 人，头略出框） | 1 | 1 | 100% | 0.709 | **12.0 px** | PASS |

判据（`tests/compare_onnx.py` 里的硬门限）：

* 匹配上的人体，关键点平均误差 ≤ 25px；
* 检出人数不超过参考值 3 倍（**w8a8 的 conf 通道量化很粗，同一人会被多尺度重复检出，
  这是已知现象**，见 `docs/troubleshooting.md` §6）；
* 匹配率 ≥ 50%。

> 17px 的误差来自关键点张量整体 w8a8 量化（scale≈3.19）。对坐姿判定够用，
> 因为所有判据都是「相对自己基准的归一化差值」，不是绝对坐标。

## 3. 性能

板端实测（640×480 YUYV 采集，`camera.infer_fps: 8`）：

| 指标 | 实测值 | 目标 |
|---|---|---|
| NPU 单帧推理 | **17.4 ms**（区间 15–25ms） | < 200ms |
| 推理帧率 | 6.9 fps（受 `infer_fps: 8` 限制） | 5–10 fps |
| 采集分辨率/格式 | 640×480 YUYV（这台 UVC 不提供 MJPG） | 640×480 / 15–30fps |
| 进程内存 | `MemoryMax=1500M` 限制，实测常驻 < 200MB | — |

对照：同一模型在本机用 ONNX Runtime CPU 跑是 **331–401 ms/帧**（6~8 线程），
NPU 快 16–19 倍。

## 4. 功能验收（现场）

| 用例 | 条件 | 预期 | 本机结果 |
|---|---|---|---|
| TC-01 | USB 摄像头插入 | 能读帧，分辨率格式正确 | PASS（640×480 YUYV，`smoke_camera.sh` 抓到 5 帧） |
| TC-02 | 摄像头拔掉再插 | 进程不退出，指数退避重连 | PASS（`reconnect_backoff_s` 1→15s，重连后自动恢复出帧） |
| TC-03 | 用户正坐 30s | 无频繁错误提醒 | PASS（状态 NORMAL，`bad_duration_s` 保持 0） |
| TC-04 | 明显歪肩 10s | 达到门限后提醒一次 | PASS（`shoulder_tilt` 12° / 8s 触发，冷却 60s） |
| TC-05 | 瞬间倾斜 1s | 不提醒 | PASS（单元测试覆盖） |
| TC-06 | 恢复正常 3s | 状态复位 | PASS（单元测试覆盖） |
| TC-07 | 肩点被遮挡 | 判 UNKNOWN，不当正常 | PASS（`invalid_reason` 写明「肩点不可信」） |
| TC-08 | 连续在场 20min | 触发一次护眼提醒 | PASS（单元测试覆盖；现场用 `--eye-seconds 30` 验证过一次） |
| TC-09 | 离开 10s | 会话结束清零 | PASS（单元测试覆盖） |
| TC-10 | 弱光 / 侧身 | 记录有效率，不强求可靠 | 已实测：关键点抖动加大，程序倾向输出 UNKNOWN（保守） |
| TC-11 | 多人入镜 | 锁定主用户或停用个体判定 | PASS（尺寸接近时 ambiguous，不做个体判定） |
| TC-12 | 连续运行 24h | 无内存泄漏 | **未做**（当前连续运行最长约 1.5 小时，无增长） |
| TC-13 | 模型版本不匹配 | 清晰报错 | PASS（`rknn_init` 失败/输出 shape 不符都会拒绝启动并说明原因） |
| TC-14 | 换机位 | 原校准失效并提示 | PASS（`BaselineMatches` 按摄像头 ID/分辨率/模型 SHA256 判定） |

## 5. 离线回归（没有人坐在镜头前也能验）

```bash
# 用一张静态图替代摄像头，把「引擎→规则→界面」整条链路跑起来
systemctl stop posture-guard
/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
  --source-image /path/to/frame.jpg
```

这个模式会把同一张图按 `capture_fps` 反复送进流水线，界面上能看到真实骨架、
真实指标、真实判定。用途：

* 没有人在镜头前时验证 UI 渲染与 socket 通信；
* 用同一张图对比不同阈值的效果；
* 取证（截图 + `status.json` 一起存）。

⚠️ **不能**用它做现场验收：它绕过了摄像头，也绕过了真实的光照与动作。

## 6. 提醒链路验证（不需要等 20 分钟）

```bash
adb shell 'systemctl stop posture-guard'
adb shell '/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
   --eye-seconds 30'          # 30 秒就触发护眼提醒
# 观察 30s 后 journald：
#   [提醒] 该远眺休息了 — 连续用眼已达 1200s，请看向 6 米外至少 20s
# 界面上会出现全屏「护眼休息」倒计时 20 → 0
```

## 7. 自检（`scripts/verify.sh`）

16 项，覆盖「服务在跑 + 真的在推理 + 状态可读 + 统计落盘」。本机结果：

```
  [PASS] 服务运行中          pid 112421
  [PASS] 开机自启             enabled
  [PASS] 服务账号             posture，组: ... video ...
  [PASS] 配置可解析          camera=/dev/video73 640x480@20fps infer=8fps fmt=MJPEG
  [PASS] 模型就位             /opt/posture-guard/models/yolov8n-pose.rknn (7.1M, sha256 5c25db4f6ac4fb99)
  [PASS] librknnrt           /lib/librknnrt.so
  [PASS] 状态 socket          /run/posture-guard/status.sock
  [PASS] 状态可读             UNKNOWN 无法判定
  [PASS] 推理后端             rknn(NPU)
  [PASS] 推理耗时             17.4 ms/帧（NPU，目标 <200ms）
  [PASS] 推理帧率             6.9 fps（配置上限 camera.infer_fps）
  [PASS] 出帧中               frame_id 311 -> 345
  [PASS] 统计落盘             stats-2026-10-28.jsonl (115 行)
  [PASS] 校准基线             肩线基准 4.5°, 样本 23 帧, 建立于 2026-10-28T17:19:03.297
  [PASS] 近 10 分钟无 ERROR   0 条
  [PASS] 桌面界面             /opt/posture-guard/ui/posture_guard_ui.py

================ 结果：PASS=16  FAIL=0  WARN=0 ================
```

## 8. 还没做（诚实清单）

* **24 小时连续运行**（TC-12）：需要排期跑一夜，看 `journalctl` 与 RSS 曲线。
* **误报率统计**（每小时的误提醒次数）：需要至少几小时的真实工作场景录像与人工标注。
* **多用户**：当前只保存单个用户的基准。
* **准确率指标**（Precision/Recall）：没有标注数据集，不做数字承诺（§15.3 明确反对）。
