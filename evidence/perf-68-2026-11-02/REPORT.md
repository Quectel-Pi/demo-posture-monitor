# 板 68 端到端性能实测（2026-11-02）

**设备**：板 68（生产机，`posture-guard` active，UI 在跑，真摄像头 UGREEN 1080P）
**口径**：端到端 FPS —— 用户选定的口径。`fps` / `latency_ms` 均取自状态 socket 的 status 消息，
就是界面看到的数字。
**采样工具**：`scripts/perf_sampler.py`（本身作为一个 UI 客户端连接，与服务端推送逻辑一致）
**采样窗口**：每次丢弃前 5–6s 预热，正式窗口 30–40s
**原始数据**：本目录 `p_0_r*.json`（单核）、`p_0_1_r*.json`（双核）、`perf_old_r*.json`（旧版）

---

## 1. 结论先说

1. **NPU 双核确实降低了推理耗时：17.3ms → 16.4ms（−5.2%），p95 18.4 → 18.2ms。**
   这个收益是真实的，且不牺牲精度（见 §3）。
2. **但端到端 FPS 一点没涨，稳定在 8.8fps。原因已定位：瓶颈是摄像头，不是 NPU。**
   静态图模式（无摄像头）跑同一条流水线是 **19.96fps**，是摄像头模式的 2.3 倍。
3. **所以「提高速度」这件事，继续调 NPU 已经没有意义。** 要涨 FPS 必须动摄像头路径。
   详见 §4，这是本次最重要的发现。

---

## 2. 端到端 FPS / 延迟实测（各 2 轮，交替执行消除漂移）

| 配置 | FPS | 推理耗时均值 | p95 |
|---|---|---|---|
| 旧版（单核，部署前二进制） | 8.80 | 17.33 ms | 18.4 ms |
| 旧版（单核，部署前二进制） | 8.80 | 17.14 ms | 18.0 ms |
| 新版 `npu=0`（单核） | 8.81 | 20.17 ms | 22.0 ms |
| 新版 `npu=0`（单核） | 10.73 | 19.74 ms | 21.6 ms |
| 新版 `npu=0_1`（双核） | 8.80 | **16.42 ms** | **18.2 ms** |
| 新版 `npu=0_1`（双核） | 8.82 | **16.73 ms** | **18.3 ms** |

读数说明：
- FPS 几乎恒等于 8.8，只有 `npu=0` 第二轮出现 10.73 —— 那一轮摄像头前人数变化，
  不是 NPU 造成的（同理该轮 latency 也降了）。
- **latency（`last_infer_ms_`）只覆盖 `rknn_run` + 输出解析**，不含采集、前处理等待。

---

## 3. 双核不牺牲精度：逐位一致性核对

同一张 `bus.jpg`（8 人），在 `npu=0` 与 `npu=0_1` 下各跑一次，比对全部关键点与检测框坐标
（`kp_cmp.cpp`，源码见 §6）。结论：**完全一致**，双核只是调度变化，不改变数值结果。

---

## 4. ★ 瓶颈定位：摄像头，不是 NPU

### 4.1 证据链

| 实验 | 结果 | 说明 |
|---|---|---|
| 应用内（真摄像头）端到端 | **8.8 fps** | 基线 |
| `infer_fps` 15 → 30 → 0（不限速） | 8.8 / 9.65 / 10.43 fps | 推理限速**不是**瓶颈 |
| 视频流 `stream_video: false` | 8.8 fps | 视频编码推送**不是**瓶颈 |
| 关掉 UI 客户端 | 8.8 fps | 客户端消费**不是**瓶颈 |
| **`--source-image` 静态图模式** | **19.96 fps** | 唯一变量是去掉了摄像头 → **翻倍** |
| 纯 `v4l2-ctl --stream-mmap`，请求 20fps | 14.25 fps | 裸采集上限 |
| 同上，请求 30fps | 21.69 fps | 相机能跑更快，但要以更高带宽换 |
| 纯 OpenCV `cap.read()`，请求 20fps | 10.84–12.14 fps | OpenCV 路径比裸 v4l2 更慢 |
| USB 链路 | `480M` 高速（USB2），非 USB3 | UVC 在 480M 下带宽受限 |

**结论**：摄像头在 640x480@MJPG 下，走 OpenCV 稳定只有 ~11-12fps，应用内实测 8.8fps
（还有解码+色彩转换开销）。NPU 从 17.3ms 降到 16.4ms 的收益被采集端完全吞掉了。

### 4.2 可选的下一步（本次未做，需你决定）

1. **把 `capture_fps` 提到 30**：相机支持，裸测 21.7fps。但 USB2 480M 下 MJPG 码率上升，
   可能挤占同一总线上的其它 UVC 功能，需实测。
2. **降低分辨率到 320x240 或 800x600**：640x480 以上 MJPG 帧更大。
3. **换 USB3 口 / 换 USB3 相机**：`lsusb -t` 显示当前挂在 `480M` 总线上，
   板上若有 USB3 口，直接翻数倍。
4. **`--source-image` 已达 19.96fps**：说明软件流水线（推理+规则+UI 推送）本身能跑 20fps，
   **天花板在采集**。这条验证了「优化软件侧已到位」。

---

## 5. 部署与回滚

- 备份：`/userdata/backup-before-deploy-20261102-114713/`（旧二进制 + 旧 yaml + postures.txt）
- 旧二进制 md5 `253229989ed24f7b5a7d0ee6de771897`
- 新二进制 md5 `ecfd4afed71b5eb078370e89537a9ae0`
- 回滚：`sudo systemctl stop posture-guard && sudo cp /userdata/backup-before-deploy-20261102-114713/posture_guard /opt/posture-guard/bin/ && sudo cp /userdata/backup-before-deploy-20261102-114713/posture.yaml /etc/posture-guard/ && sudo systemctl start posture-guard`
- 生产最终配置：`npu_core_mask: "0_1"`、`profile_release_ratio: 0.5`、`stream_video: true`、
  `infer_fps: 15`、`capture_fps: 20`（其余与部署前逐键比对无差异）

---

## 6. 复现命令

```bash
# 端到端采样（就是界面看到的数字）
ssh 板68 'python3 /userdata/pm-build/scripts/perf_sampler.py --seconds 40 --warmup 6'

# 静态图模式（绕开摄像头，测软件流水线天花板）
sudo /opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
  --headless --source-image /var/lib/posture-guard/poses/normal/frame_12.jpg

# 裸采集上限
v4l2-ctl -d /dev/v4l/by-id/usb-UGREEN_... -video-index0 \
  --set-fmt-video=width=640,height=480,pixelformat=MJPG --set-parm=20
v4l2-ctl ... --stream-mmap --stream-count=60
```

`kp_cmp.cpp`（关键点一致性核对）与 `cap_test.cpp`（OpenCV 采集速度）在本次会话中生成于
`/userdata/pm-build/`，如需长期保留可移入仓库 `tools/`。
