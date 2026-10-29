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

## 15. 俯视机位（摄像头装在显示器上方）引起的三连问题

**场景**：摄像头架在屏幕上沿往下拍，人坐在椅子上，距离约 40cm。
这个机位本身没问题，但踩了三个与「几何投影」有关的坑，全都表现为误报/漏报。

### 15.1 校准永远失败：「校准样本不足（6 帧）」

**根因**：取景门里有一条 `head_gap = (肩线y − 鼻子y) / 肩宽 >= 0.22`。
**俯视机位下这个量天然就小**（实测合格帧只有 0.20~0.35，直视机位会是 0.6+），
人再一低头直接压到 0.22 以下 —— 于是大量帧被判「几何不可用」丢掉，
5 秒校准窗口里收不到足够样本。

更糟的是：**被丢掉的正是「低头」帧**，也就是系统最该检测的那批。

**修法**：门限从 0.22 降到 **0.08**（改为可配 `posture.min_head_gap`）。
0.08 只用来挡「模型把鼻子检到肩线上」这种明显错检，不再用作机位判据。

### 15.1b 低头永远触发不了（阈值超出几何可能范围）

**根因**：低头判据是 `head_offset_y − 基准head_offset_y > 0.35`（固定值）。
俯视机位基准是 −0.20，而鼻子压到肩线上 `head_offset_y` 最多到 0，
**能产生的最大下降量只有 0.20 < 0.35** → 物理上不可能触发。

**修法**：阈值改为按基准缩放 —— `基准距离 × 0.55`，钳在 `[0.10, 0.45]`
（`posture.head_down_drop_ratio / _min / _max`）。俯视机位得 0.11，直视机位得 0.45。
单元测试断言这两个值。

### 15.2 一直误报「托腮」

**根因**：判据是「手腕到鼻子的距离 / 肩宽 < 0.95」。但**坐在桌前打字时手在桌面上**，
俯视投影下手离胸/脸并不远，这个比值很小 → 每帧都判托腮。

**修法**：加硬门控 —— **手腕必须高于肩线**（`wrist.y < 肩线y − 0.1×肩宽`）。
实测手放桌面时手腕比肩线低 **95~109% 肩宽**，托腮时一定高于肩线，两者分得很干净。
不满足门控就把 `wrist_to_nose` 置 99（永不触发）。

### 15.3 「歪头」角度乱跳（−48° ~ +9°）

**根因**：歪头用的是 `atan2(nose.x − 肩线中点x, 肩线中点y − nose.y)`。
俯视机位下分母 `肩线中点y − nose.y` 很小、趋近于零，
横向几个像素的关键点噪声就能把角度放大几十度。这不是调阈值能救的，
是这个公式在这种几何下**数值上就不成立**。

**修法**：改用**左右耳连线**相对水平的角度。耳线是横跨头部的基线，
和肩线一样稳定（实测同一坐姿摆动 6° vs 原来的 ±50°）。
基准里同时记录 `ear_angle_deg` + `ear_angle_valid`；耳朵不可信时自动退回旧公式。

**验证**：`bash scripts/status.sh --watch`，正常坐姿下
`耳线°` 应稳定在基准 ±10° 内，`腕鼻 = 99`（手在桌面，正确不触发）。

### 15.4 顺带修掉：挪了摄像头不重新校准

**根因**：`BaselineMatches()` 只比摄像头 ID / 分辨率 / 模型 SHA256。
**同一个摄像头换了位置**这三项都不变，基准被判有效，所有阈值算偏。

**修法**：启动后收 20 个有效帧，用中位数的 `width_ratio` 与 `head_offset_y`
和基准比对，变化超过 1.8 倍 / 0.45 个肩宽就作废基准并自动重校准：

```
[WARN] 校准基准已失效：画面里肩宽占比 0.40，校准基准是 0.11（差 3.7 倍）——摄像头或人就位变了
```

---

## 16. 「离屏过近」到底判的是什么（本机没有 ToF）

**先说结论**：这条规则**不测距**，判的是「取景相对校准基准变了多少」。
摄像头没有 ToF / 深度传感器，单目图像给不出可靠厘米数，所以代码里也没有任何
厘米输出（早期版本曾显示过估算距离，因误差 ±20~30% 且语义易误解，已移除）。

### 16.1 早期 bug：阈值写死绝对值 → 换机位就失效

`width_ratio`（肩宽/画面宽）的绝对值强依赖摄像头 FOV：

| 机位 | 正常坐姿时的值 |
|---|---|
| 旧 icSpring 直视 | 0.11 ~ 0.78 |
| 现 UGREEN 俯拍 | 0.35 ~ 0.50 |

早期阈值写死 `0.75`（来自第一台摄像头"坐很近"的观测）。换到俯视机位后，
**正常坐着就 0.42**，要触发 0.75 需要占比再涨 2.1 倍 —— 等于要求脸贴屏幕，规则失效。

**修法**：`posture.too_close_ratio_rel`（默认 1.35），阈值 = 校准基准 × 1.35，
与机位/摄像头无关。本机基准 0.354 → 阈值 0.478。

### 16.2 早期 bug：width_ratio 抖动被当成距离变化

人没动，`width_ratio` 却在 0.377 ~ 0.524 之间跳（±39%），`too_close`
反复触发又解除。根因是肩宽 = 两点之差，某帧把肩点检偏 64 像素就翻倍。

**修法**：7 帧滑动**中值**滤波。平滑后极差 0.147 → 0.016。
`status` 同时输出 `width_ratio`（平滑）与 `width_ratio_raw`（原始）便于对照。

### 16.3 想真正知道距离怎么办

| 方案 | 精度 | 成本 |
|---|---|---|
| ToF（VL53L1X 等，I²C） | ±2cm | 几十元 + 驱动 |
| 相机标定 + 头部姿态解算 | ±5cm 量级 | 高，仍受坐姿影响 |
| 单目估距（已移除） | ±20~30%，且量的是「到摄像头」 | — |

⚠️ 即使加了 ToF，也要注意量的是**到传感器**的距离，不是「眼睛到屏幕」——
摄像头在显示器上方俯拍时两者差一截。

## 17. 喇叭不出声 / 声音很小 / 只有杂音

本机外接喇叭排查完整走过一遍，**四个独立问题叠在一起**，逐个说清。

### 17.1 先确认不是软件把人绕晕了

```bash
# ① adb 进去默认是 root，pactl/paplay 会报
#    "Connection refused" 或 "Host is down"
#    —— PipeWire 是桌面用户 quectel 的会话服务，root 连不上
su quectel -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/1000 pactl list short sinks"

# ② 别把 pkill 写进同一条 shell 命令
#    pkill -f aplay  会把包含 "aplay" 字样的这条 sh 也杀掉（exit 137）
```

### 17.2 `amixer` 改 DAC 音量无效 —— WirePlumber 每次播放会覆盖回去

实测：`amixer -c 0 sset DACL 255` 之后读寄存器是 `0xff`，
但一开始播放就变回 `0xb3`（=179=-6dB）。

```bash
R=/sys/kernel/debug/regmap/2-0010/registers
sed -n '71,72p' $R          # 0x46/0x47 = DACL/DACR

amixer -c 0 sset DACL 255
sed -n '71p' $R             # 0xff  ← 改成功了
aplay -D hw:0,0 /tmp/x.wav &
sleep 1; sed -n '71p' $R    # 0xb3  ← 被写回去了
```

原因：`/usr/share/alsa-card-profile/mixer/paths/analog-output-es8389-m2.conf`
里写死了 `[Element DACL] volume = 179`，WirePlumber 按它工作。

**正确做法是改这个配置文件**（`scripts/tune_speaker.sh` 做的就是这件事）：

```bash
sh /opt/posture-guard/scripts/tune_speaker.sh 205   # 179=-6dB → 205≈+1dB
```

改完必须重启 wireplumber 才生效（脚本会自动做）。
`179 → 205` 实测从「听不清」变成「够大听得清」。

### 17.3 语音素材 RMS 只有 11%，峰值再高也没用

六个提示语音峰值 73~99%，但 **RMS 只有 10~11%**。
响度看的是 RMS 不是峰值，所以听感比满刻度正弦低约 9dB。

`assets/voice/*.wav` 已经用 ffmpeg 做过响度归一化：

```bash
ffmpeg -i in.wav -af "acompressor=threshold=0.125:ratio=1.6:attack=8:release=120,\
loudnorm=I=-14:TP=-1.0:LRA=11,alimiter=limit=0.89" -ar 22050 -ac 1 out.wav
```

RMS 从 11% 提到 19~20%（+5dB）。自己换素材也要做这一步，否则「换了句更清楚的录音反而更轻」。

### 17.4 功放是 BTL 差分，喇叭必须接 VOP/VON 两端

网表核对结果（`docs/sch/M2` 的 allegro 网表）：

```
U0903 = AW8010BFCR   单声道 Class-D 功放
  C3 → R0934 → VOP_1 → J0901 pin1     ← 喇叭 +
  A3 → R0915 → VON_1 → J0901 pin2     ← 喇叭 -
  B1/B2 → PA_VDD_5V                    （常供 5V）
  C2    → SPK_SD_3V3 = DTS 的 spk-con-gpio（播放时 DAPM 拉高）
  A1/C1 → 经 150K + 33nF 接 ES8389 的 LINEOUT_P/N
J0901（喇叭座，4pin）: pin1=VOP  pin2=VON  pin3=GND  pin4=GND
J0902（3.5mm 耳机）:   pin3=HPH_R  pin4=HPH_L  pin2=GND
```

**BTL 输出的一端如果接到 GND（pin3/pin4），功放会被保护，
只剩极小的失真声。** 现场就踩了这个：从「一点声都没有」重接到
pin1/pin2 之后立刻变成「有声」，再调增益就正常了。

判断接线对不对，不用拆机：

```bash
sh /opt/posture-guard/scripts/speaker_check.sh
```

它会依次打印声卡、`spk switch`/`Speaker Switch` 状态、`spk-con` GPIO、
DAC 寄存器、PipeWire sink，最后放一段测试音。

### 17.5 用麦克风客观测喇叭（避免「我觉得好像响了」）

排查过程中靠人耳判断来回反复，很不可靠。板载麦克风可以当测量工具，
**但默认采集音量是 8（-91.5dB，等于关着）**：

```bash
amixer -c 0 sget ADCL          # 默认 8 → 录出来全是 0
amixer -c 0 sset ADCL 167      # -12dB，留余量
amixer -c 0 sset ADCR 167
```

然后录音 + Goertzel 算 440Hz 幅值就能量化比较（`scripts/speaker_check.sh`
里有完整实现）。实测本机：环境本底 440Hz≈0.4，正常出声 ≈15，满刻度 ≈22。

---

## 18. 排障常用命令

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
