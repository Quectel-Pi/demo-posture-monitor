# 个人校准与阈值标定

> 依据：实施方案 §4.6（个人校准）、§12.1（阈值校准流程）、§12.2（初始阈值）
>
> **为什么必须校准**：肩线在图像上的倾角取决于摄像头安装角度、用户身高坐姿、
> 镜头畸变。把所有人统一成某个「标准角度」是错的（§4.6 明确写了）。
> 校准就是给每个「机位 + 人」记一份正常坐姿的基准。

## 1. 什么时候需要校准

| 情况 | 是否需要 |
|---|---|
| 第一次部署 | 需要（程序在检测到有人坐下时会自动开始） |
| 挪动摄像头、换支架高度 | 需要 |
| 换分辨率 / 换摄像头 / 换模型 | 需要（程序会自动判定基准失效并提示） |
| 换一个身高差别很大的用户 | 需要（当前只支持单个用户基准） |
| 只是想收紧/放宽阈值 | 不算校准，改 `posture.yaml` 阈值即可 |

程序把「摄像头 ID + 分辨率 + 模型 SHA256」一起写进基准里。任何一项变了，
`UserCalibration::BaselineMatches` 会给出明确原因，例如：

```
校准基准与当前环境不匹配（分辨率变了（640x480 -> 1280x720）），请在界面上重新校准
```

## 2. 怎么做

### 2.1 自动（推荐）

不用做任何事：没有基准时，程序检测到有人坐在镜头前就会自动进入 5 秒校准，
界面会显示蓝色的「校准中 xx%」和「请保持正常坐姿…」。

完成后 journald 会有一条：

```
校准完成：肩线基准 4.484604°，头肩纵向基准 -0.896764，样本 23 帧；基准已保存到 /var/lib/posture-guard/baseline.json
```

### 2.2 手动触发

```bash
bash /opt/posture-guard/scripts/calibrate.sh          # 让正在跑的服务重新校准
bash /opt/posture-guard/scripts/calibrate.sh --reset   # 删掉旧基准
bash /opt/posture-guard/scripts/calibrate.sh --only    # 停服务、前台校准、完成后恢复
```

界面上也有「重新校准」按钮。

### 2.3 校准的四个动作要领

1. **把摄像头抬到能看见头和双肩**（屏幕上沿中部，镜头略高于眼睛）。
2. 坐到你平时坐的位置，**双手放桌面**，不要托腮、不要歪头。
3. 保持 5 秒不动（说话、拿东西、转身会让样本被判为不稳定）。
4. 看到状态变成「正常」就成功了。

## 3. 校准质量门限（程序强制）

`src/UserCalibration.cpp` 里做了三项检查，任意一项不过就**拒绝保存**并提示：

| 检查 | 门限 | 不过时的提示 |
|---|---|---|
| 样本数 | ≥ 15 帧 | 「校准样本不足（N 帧），请保持坐姿并确认摄像头画面清晰」 |
| 一致性 | 保留帧 ≥ 55%，且 ≥ 10 帧 | 「校准样本不稳定（N/M 帧一致），请坐正并保持不动后重试」 |
| 取景 | 鼻子不贴上沿、双肩不贴边、头肩比例正常 | 「头顶贴到画面上边缘（摄像头偏低或人太近），请把摄像头抬高」 |

一致性检查用的是「偏离中位数超过门限就丢弃」：肩线 ±4°、头肩纵向 ±0.35 肩宽、
头肩横向 ±0.30 肩宽、头轴 ±20°。这是为了避免一次伸手就把基准带歪 —— 一旦带歪，
之后每次坐正都会被判成异常。

## 4. 基准长什么样

`/var/lib/posture-guard/baseline.json`：

```json
{"valid":true,"shoulder_angle_deg":4.485,"head_offset_x":-0.11,
 "head_offset_y":-0.897,"head_axis_deg":-3.2,"width_ratio":0.42,
 "samples":23,"frame_width":640,"frame_height":480,
 "camera_id":"/dev/v4l/by-id/usb-icSpring_icspring_camera-video-index0",
 "model_sha256":"5c25db4f...","created_at":"2026-10-28T17:19:03.297"}
```

**合理的量级参考**（本机实测）：

| 字段 | 正常范围 | 说明 |
|---|---|---|
| `shoulder_angle_deg` | −15° ~ +15° | 不为 0 很正常，说明摄像头有安装倾角 |
| `head_offset_y` | −0.55 ~ −1.10 | 鼻子在肩线上方约 0.6~1.1 个肩宽 |
| `head_axis_deg` | −20° ~ +20° | 人略偏左/右坐 |
| `width_ratio` | 0.25 ~ 0.65 | 肩宽占画面宽的比例（坐得越近越大） |

如果 `head_offset_y` 只有 −0.2 左右，几乎肯定是头被裁掉了（鼻子贴画面顶），
请先调机位再校准。

## 5. 阈值怎么调

需要动手的只有 `posture.yaml` 里的这些键：

```yaml
posture:
  shoulder_trigger_deg: 12      # 偏离基准多少度算高低肩
  shoulder_hold_s: 8            # 需要持续多久
  head_down_drop: 0.35          # 头肩纵向比基准低多少（单位：肩宽）
  too_close_ratio: 0.75         # 肩宽/画面宽
  hand_face_ratio: 0.95         # 手腕-鼻子/肩宽
```

调参的纪律：

1. 一次只改一个值；
2. 改完 `sudo systemctl restart posture-guard`；
3. 用 `bash scripts/status.sh --watch` 看实时指标，别靠猜；
4. 误报 → 把阈值往「更难触发」方向调；漏报 → 往反方向调；
5. 记录改前改后的值（`docs/testing.md` 的表就是干这个的）。

### 现场看指标

```bash
bash /opt/posture-guard/scripts/status.sh --watch
```

```
后端      : rknn / 2.3.2 (429f97ae6b@2025-04-09T09:09:27) / drv 0.9.8
姿态      : SHOULDER_TILT   高低肩   高低肩
指标      : shoulder_angle_deg=16.5 head_offset_y=-0.812 head_axis_deg=-4.2
            width_ratio=0.38 wrist_to_nose=1.92  (valid=True)
校准      : baseline=True calibrating=False 100%  (2026-10-28T17:19:03)
```

`shoulder_angle_deg` 与基准的差就是 `shoulder_tilt_dev`；`head_offset_y` 比基准低
多少就是 `head_down` 的输入量。

## 6. 已知的能力边界

* 单目正面摄像头**测不出脊柱曲度**，也测不出真实低头角度。`head_down` / `head_tilt`
  是肩宽归一化的近似量，界面和文档都按「近似」标注（§16.7）。
* `width_ratio` 是趋势指标，换 FOV 不同的摄像头就变，不是厘米距离。
* 坐得离屏幕很远（肩宽占比 < 0.15）时关键点精度下降，误报会增加。
* 弱光、逆光、口罩、侧身时关键点抖动明显；这时程序倾向输出 `UNKNOWN` 而不是乱判。
