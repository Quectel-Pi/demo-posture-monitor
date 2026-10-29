# 排障手册

> 全部来自本项目实际踩过的坑，按「症状 → 根因 → 修法」写。
> 每条都标了是代码已修、还是需要现场注意。

---

## 1. 关键点只有鼻子/眼睛有值，其余 16 个点全是 0

**症状**：`--image` 模式 dump 出来，`nose/eye/ear` 有值，`shoulder/elbow/hip…` 全是
`(-135.0, 0.0)`（`-135` 是 `(0 - pad_x)/ratio`，即原始值 0）。

**根因**：读关键点张量时边界判断写错了：

```cpp
// 错：out_sizes 是「元素个数」，又除了一次 sizeof(float)
if (base + 2 < out_sizes[3] / sizeof(float)) { ... }
```

`out_sizes[3]` 已经是 428400（float 个数），再除 4 变 107100。
于是 `base = (j*3)*8400 + anchor` 在 `j >= 4` 时全部越界，被兜底成 0。

**修法**：边界用元素个数比，并且要把整个 3 通道跨度算进去：

```cpp
if (static_cast<long long>(base) + 2LL * kpt_plane < out_sizes[3]) { ... }
```

**教训**：`rknn_output.size` 的语义在不同 runtime 版本上不一致（有时是字节，有时是
元素数），**元素个数一律用 `rknn_query` 到的 dims 乘积算**，别用 `size` 推。
`kpt_plane` 也要用 `元素数 / (17*3)`，不要用 `size/4` 反推。

---

## 2. 摄像头打不开：`can't be used to capture by index` / `by name`

**症状 A**：

```
[ WARN] open VIDEOIO(V4L2): backend is generally available but can't be used to capture by name
摄像头打开失败: 无法打开摄像头 /dev/video73
```

**根因**：OpenCV 的 V4L2 后端**不认设备路径**，只认索引。直接传
`cap.open("/dev/video73", cv::CAP_V4L2)` 会失败。

**修法**：从路径（或 `by-id` 链接）解析出尾部数字，用索引打开：

```cpp
cap.open(index, cv::CAP_V4L2);   // index ← realpath("/dev/v4l/by-id/...") 的数字后缀
```

**症状 B**：改成索引后报 `can't open camera by index`，但 `v4l2-ctl` 又正常。

**根因**：摄像头被**别的进程独占**了（同一 UVC 只能一个进程打开）。
本机就是这样：旧的 Python 版 demo 还在跑，`fuser -v /dev/video73` 能看到它。

**修法**：`fuser -v /dev/video73` 找占用者并停掉。切换实例前先
`systemctl stop posture-guard`。

---

## 3. 改了代码、编译「成功」，但行为完全没变

**症状**：明明改了源码，`cmake --build` 没有报错，运行结果却还是旧的。

**根因**：用 tar 把源码传到板端时，tar **保留了原 mtime**。如果文件时间戳比
`build/` 里的对象文件还旧，`make` 判定「不需要重编」，只重新链接甚至什么都不做。

**验证方法**：

```bash
md5sum build/posture_guard   # 编译前后对比，一样就说明没重编
```

**修法**：编译前 touch 一遍源码。

```bash
find src include tests -type f | xargs touch
cmake --build build -j8
```

`scripts/build_board.sh` 已内置这一步。这个坑害我排查了「关键点全 0」很久，
因为改的修复根本没进二进制。

---

## 4. RKNN 输出全是垃圾 / 只有最粗尺度有检出

**症状**：置信度峰值只出现在 stride 32（20×20）那一路，stride 8/16 一个都没有；
框能出来但中心偏几十像素；同一个物体被检出 6–8 次。

**根因（三个叠加）**：

1. 输出张量是 **NCHW**：`reshape(65, gs, gs)`，第 64 通道是 logits，要 sigmoid；
2. 关键点张量是 `(1,17,3,8400)`，取值为 `(j*3+c)*8400 + anchor`，
   **anchor 必须跟框的 flat index 对齐**（`(y*gs+x) + {0, 6400, 8000}`）；
3. `rknn_input.type` 必须设 `RKNN_TENSOR_UINT8`（枚举值 3）而不是 `INT8`（2）：
   写 2 不报错，但输出会变成一团糟。

**修法**：见 `src/RknnPoseEngine.cpp` 的 `Decode()`。校验方式：与 ONNX fp32 逐点比，
平均误差 12–17px 就是正常的（见 `docs/testing.md` §2）。

---

## 5. 关键点自带的置信度恒为 0

**症状**：`keypoints[j].confidence` 全是 0。

**根因**：w8a8 量化把整个 `(17,3,8400)` 张量用**同一个** scale（本模型 3.19）和
zero-point（−105）量化，置信度通道只有 0~1 一个量化级，反量化后恒为 0。

**修法**：不要用它过滤。本实现用**人物框置信度**作为关键点可信度代理，
并在 `PoseEngine::joint_confidence_reliable()` 里明确声明 `false`，
启动日志里也打一条 WARN。真正的可信度靠三道几何闸门（`docs/architecture.md` §4.1）。

---

## 6. 同一个人被检出 6–8 次，「多人在场」误判

**症状**：画面里只有一个人，`persons` 报 5–8；把两框判成「多人」而不做个体判定。

**根因**：w8a8 的 conf 通道量化很粗（scale≈0.14），相邻 anchor 的置信度几乎一样，
argmax 会在邻居间跳；DFL 量化误差让同一个人的框中心抖动几十像素，
IoU 达不到全局 NMS 阈值（0.45），于是 NMS 之后还剩一堆近似重复框。

**修法（两层）**：

1. `PostureAnalyzer::SelectPrimary` 里先做**聚类去重**：IoU > 0.35 或
   中心距 < 0.35×框宽 视为同一个人；
2. 去重之后如果确实还有两个人尺寸接近，才判 `ambiguous` 并停用个体判定。

不要靠调高 `pose_confidence` 解决 —— 实测把阈值提到 0.75 仍然有 6 个重复框。

---

## 7. 机位不对时几何量完全失真（本机现场就遇到了）

**症状**：校准出来的 `head_offset_y` 只有 −0.17（正常应该 −0.6 ~ −1.1），
肩线角度 12.8°，之后每次都误报。

**根因**：UVC 摄像头装得太低 / 人坐得太近，**头被画面上边缘裁掉了**。
模型对出框的关节会「猜」一个位置，得到的肩宽、头肩偏移全是畸形值。
用它做基准，之后每次坐正都会被判异常。

**修法**：`PostureAnalyzer::ComputeMetrics` 加了取景门：

| 检查 | 门限 |
|---|---|
| 鼻子不能贴画面上沿 | `nose.y >= 0.03 × 高度` |
| 双肩不能贴边/出框 | 距四边 ≥ 2% |
| 鼻子到肩线距离 | `>= 0.22 × 肩宽` |

不满足就 `UNKNOWN` + 人话原因（「头顶贴到画面上边缘…请把摄像头抬高」），
**不校准、不报警**。

**现场动作**：把摄像头抬到屏幕上沿，让头和双肩完整入画，再点「重新校准」。

---

## 8. 服务起来了但没有界面

**症状**：`systemctl status posture-guard` 正常，屏幕上什么都没有。

**根因**：本项目的后台服务是 **systemd 系统服务**，运行在无图形会话的上下文里，
拿不到 `WAYLAND_DISPLAY`（这是**设计如此**，见实施方案 §13.3）。
界面是另一个进程。

**修法**：

```bash
# 确认 UI 进程在跑
ps -eo pid,user,cmd | grep posture_guard_ui | grep -v grep
# 没有就手动拉起来
sudo -u quectel env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  GSK_RENDERER=ngl nohup python3 /opt/posture-guard/ui/posture_guard_ui.py >/tmp/ui.log 2>&1 &
```

开机自启用 `scripts/install_ui_autostart.sh`（装到桌面用户家目录，不要用 root）。

---

## 9. `/var/log` 里没有日志 / 开机 Exec 整条不执行

**症状**：autostart 里写了 `Exec=/bin/bash -lc '/usr/local/bin/xxx.sh >> /var/log/foo/app.log 2>&1'`，
重启后既没有日志也没有程序。

**根因**：板端 `/var/log` 是 **软链 → `/var/volatile/log`（tmpfs）**，
重启清空、目录不存在。**bash 的重定向失败会让整条命令直接不执行**。

**修法**（本项目已按这个设计）：

* 日志走 journald（`StandardOutput=journal`），不自己往 `/var/log` 写；
* 需要文件日志就写到 `/var/lib/posture-guard/`（持久分区）；
* 自启项的 `Exec=` 里**不要做重定向**；
* `/run` 下的目录用 `RuntimeDirectory=`（或 `systemd-tmpfiles`）让 systemd 建，
  否则非 root 进程建不了。

---

## 10. `verify.sh` 误报 librknnrt 找不到

**症状**：`ldconfig -p | grep librknnrt` 没输出，但程序跑得好好的。

**根因**：`/usr/lib/librknnrt.so` 是**裸 .so（没有 soname 版本号）**，
ldconfig 缓存里查不到。

**修法**：`verify.sh` 里既查 ldconfig 也直接 `find` 文件。已修。

---

## 11. `taskset`/`pkill -f` 把自己杀掉了

**症状**：在 `adb shell` 里跑 `pkill -f "python3 -m posturemon"`，命令返回 137，
什么都没干成。

**根因**：`pkill -f` 匹配的是**整条命令行**，而 adb shell 执行的命令串里
就包含这个字符串 → 匹配到自己。

**修法**：锚定开头或用进程遍历：

```bash
pkill -f "^python3 -m posturemon"
# 或
for p in $(ps -eo pid,cmd | grep -E "bin/posture_guard" | grep -v grep | awk '{print $1}'); do kill $p; done
```

---

## 12. 界面上看不到摄像头画面（只有黑底几根线）

**症状**：后台服务日志一切正常（`摄像头已打开`、`frame_id` 在涨、`persons` 有值），
但屏幕上只有深色背景 + 骨架线条，看不到真实画面。

**根因（本项目的真实原因）**：后台服务确实在抓帧，但**从来没有把像素发给界面**。
架构上只传了「17 个归一化关键点」用来画骨架（当时按 §18.2 隐私要求做的减法），
界面自然没有画面可显示。

**修法**：加了一条独立的视频通道（`ui.stream_video`，默认开）：

```
posture-guard (每帧 cv::imencode JPEG → base64 → unix socket)
   {"type":"video","seq":N,"w":640,"h":480,"jpeg":"<base64>"}
        ▼
posture_guard_ui.py  →  Gtk.Picture(content_fit=CONTAIN)
                         + 透明 Gtk.DrawingArea 叠骨架
```

配置项：

```yaml
ui:
  stream_video: true     # false = 只显示骨架（更隐私，但没法调机位）
  video_width: 640       # 下采样宽度
  video_fps: 10          # 视频帧率（与推理帧率无关）
  jpeg_quality: 70
```

带宽：640×480 q70 ≈ 40KB/帧，base64 后 ~54KB，10fps ≈ 540KB/s，走本机 unix socket 无压力。
图像**只在内存与本地 socket 里流转，不落盘、不出网**（`privacy.*` 仍强制为 false）。

**这个改动不只是「好看」**：看不到画面就没法调机位，而机位不对正是之前校准失败的
根本原因（见 §7）。有了画面，用户能立刻看出「头被裁掉了」。

### 踩到的一个 GTK 坑

最初想用 `Gdk.cairo_set_source_paintable(cr, texture, ...)` 直接在 DrawingArea 里画
texture，本机 GTK 报：

```
AttributeError: 'gi.repository.Gdk' object has no attribute 'cairo_set_source_paintable'
```

改成 GTK4 原生组合：`Gtk.Overlay` 里塞 `Gtk.Picture` 做底、透明 `DrawingArea` 做叠加层。
注意叠加层**不能**填充背景色，否则会把画面盖住。

---

## 13. 摄像头参数被硬编码，换摄像头就开不了

**症状**：日志报 `无法打开摄像头`，或者 `期望 MJPEG 但实际是 YUYV` 一直 WARN，
或者设了 640×480 出来却是 1280×720。

**根因**：代码里直接 `cap.set(FRAME_WIDTH, 640)` 之类硬编码，没有先问设备支持什么。
不同 UVC 差别很大 —— 本项目这块 icSpring 只有：

```
[0]: 'YUYV' (YUYV 4:2:2)
     Size: Discrete 640x480
        30 / 25 / 20 / 15 / 10 / 5 fps
```

**只有一个格式、只有一个分辨率**，连 MJPG 都没有。硬编码 MJPG 就是自找麻烦。

**修法**：先枚举再协商（`src/V4l2Probe.cpp`）：

1. `VIDIOC_QUERYCAP` → 确认是采集节点（`/dev/video74` 是 metadata 节点，会被拒）；
2. `VIDIOC_ENUM_FMT` → 所有像素格式；
3. `VIDIOC_ENUM_FRAMESIZES` → 每个格式支持的尺寸；
4. `VIDIOC_ENUM_FRAMEINTERVALS` → 每个尺寸支持的帧率；
5. `ChooseCapture()` 挑最接近的一组：格式优先压缩格式（省 USB 带宽），
   尺寸取最接近面积，帧率取「不超过期望的最大值」；
6. **设置顺序必须先 FOURCC 再分辨率** —— 反了驱动会按默认格式重新协商，
   出现「明明设了 640×480 出来 1280×720」；
7. 最后把「请求值 vs 实际协商值」一起打进日志，只在**真的不一致**时才 WARN。

本机日志长这样：

```
摄像头能力：icspring camera: icspring camer [YUYV(640x480) ]
摄像头已打开 /dev/video73 实际 640x480@20fps 格式 YUYV（请求 640x480 YUYV）
```

`preferred_pixel_format: "auto"` 表示「用设备能给的最好格式」；也可以写死
`MJPG` / `YUYV`，不支持时会自动回退并说明原因。

**为什么重要**：YUYV 640×480@30 的原始码率是 18MB/s，MJPG 通常只要 1–2MB/s。
如果换一个支持 MJPG 的摄像头，`auto` 会自动选 MJPG，不用改代码。

---

## 14. 画面看着"糊"，或者关键点老是抖

**症状**：画面能出来，但边缘发软、细节不清；关键点坐标帧间抖动明显。

**根因 A：摄像头在跑「连续自动对焦」**。UVC 的
`focus_automatic_continuous=1` 会让镜头每帧微调，逐帧看就是持续轻微失焦。
实测同一位置，AF 开着时清晰度只有锁死时的 1/8。

**根因 B：焦点根本没见过你的位置**。很多自动对焦 UVC 的出厂默认焦点并不在
工位距离上。本项目这块 UGREEN 1080P 默认 `focus_absolute=160`，实测清晰度 341；
改成实测最佳的 70 后是 637（**1.87×**）。

**修法**：

```bash
bash /opt/posture-guard/scripts/focus.sh --scan     # 扫最佳值
# 写进配置
#   camera: { focus_autofocus: false, focus_absolute: 70 }
sudo systemctl restart posture-guard
# 确认：
journalctl -u posture-guard | grep 对焦
```

**根因 C：清晰度测出来自相矛盾** → 人在动。扫描必须多轮交错 + 每档取最优帧 +
设完等马达停稳，否则姿态漂移会盖过焦距差异（这条踩过，见
`docs/calibration.md` §4.5 末尾）。

**注意**：定焦摄像头没有 `focus_absolute`，脚本会直接告诉你调不了；
这种情况只能整体挪动机位，或手拧镜头前端的对焦环（部分型号有）。

## 15. 排障常用命令

```bash
# 环境体检（编译器/OpenCV/NPU/摄像头/部署产物）
bash /opt/posture-guard/scripts/check_board.sh

# 摄像头单独体检（不启动服务）
systemctl stop posture-guard
bash /opt/posture-guard/scripts/smoke_camera.sh

# 静态图推理 + 骨架图（不依赖摄像头）
/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
  --image /path/x.jpg --dump-keypoints /tmp/kp.json
# 会生成 posture_guard_out.png（带骨架的图）

# 实时指标
bash /opt/posture-guard/scripts/status.sh --watch

# 服务日志
journalctl -u posture-guard -f
journalctl -u posture-guard --since '-10min' | grep -E "WARN|ERROR"

# 谁占着摄像头
fuser -v /dev/video73
```

---
