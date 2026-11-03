# 板 68 语音 + 坐姿精度修复（2026-11-02）

## 一句话结论

两件事都不是「从零做」，而是**成品没落地 + 落地后没验证**：
语音的神经女声**从未部署过**（板上一直是 git HEAD 的 espeak 素材）；
坐姿的托腮规则**只有半条**（只判手腕抬高，不判手是否靠近脸），导致托腮占用 41.7% 检测时间。

修完之后：**自检 PASS=17 / 单测 4/4 / 六条语音全部可播 / 五种姿势真人实测全部判对**。

---

## 一、语音：机械音 → 神经女声

### 根因：不是音色选得不好，是根本没换过

板 68 的 WAV md5 与 `git HEAD` **逐一完全相同**，与工作区的神经版**全部不同**：

| key | 板 68（= git HEAD，espeak） | 工作区（xiaoxiao 神经版） |
|---|---|---|
| hand_on_face | `64e87aa6…` | `435996b2…` |
| head_down | `4f97f191…` | `d73ab3ee…` |
| head_tilt | `e25148f5…` | `d7a022d2…` |
| rest | `4f566a63…` | `edfd9b76…` |
| shoulder_tilt | `c997c1c1…` | `3d47953c…` |
| too_close | `ec00134c…` | `8a3bc072…` |

→ 用户听到的机械味是 espeak 的共振峰拼接合成，不是神经女声参数问题。

### 客观指标（实测）

| 项 | 上板前（espeak） | 上板后（神经女声） |
|---|---|---|
| 采样率 | 22050 Hz | **24000 Hz** |
| head_down | 3.45 s / 峰值 **1.000** / 削顶 16 样本 | 2.69 s / 0.890 / **削顶 0** |
| hand_on_face | 3.09 s / 削顶 3 | 2.48 s / **削顶 0** |
| head_tilt | — | 1.92 s / 削顶 0 |
| shoulder_tilt | — | 2.57 s / 削顶 0 |
| too_close | — | 2.63 s / 削顶 0 |
| rest | 9.44 s / 削顶 29 | 4.00 s / **削顶 0** |
| 格式 | 16bit mono | 16bit mono（不变） |

**顺带修掉一个用户没提的问题**：旧素材峰值 1.000，实际有 16/3/29 个样本已削顶 → 除机械音外还叠了轻微破音。

### 为什么零风险
格式、路径、调用方式全不变：UI 仍是 `paplay assets/voice/<key>.wav`（`ui/posture_guard_ui.py:127`），
UI 代码零改动，运行期仍完全离线、零新依赖。

### 验证
六条全部 `paplay exit=0`：
```
head_down=0 head_tilt=0 shoulder_tilt=0 too_close=0 hand_on_face=0 rest=0
```

---

## 二、坐姿识别：三个真实缺陷

### 缺陷 1 ★ 托腮规则只有半条（本次最大问题）

生效画像（修复前）：
```
rule 4  hand_face  wrist_rise  +  0.286
```
**只判「手腕比基准抬高 0.286 肩宽」，完全没有「手要靠近脸」的约束。**

生产实测（近 3h，223 条切换日志）：

| 标签 | 累计停留占比 |
|---|---|
| **托腮** | **41.7%** ← 比「正常」还多 |
| 正常 | 32.3% |
| 离屏太近 | 18.1% |
| 低头/趴桌 | 4.9% |
| 歪头 | 3.0% |

- 77 次托腮触发中 **16 次（21%）`wrist_near > 1.0`** —— 手离鼻子超过一个肩宽，物理上不可能托腮。
- 短命误报（<5s）的 `wrist_rise` 中位 **0.522**，反倒高于长命真托腮（≥30s）的 **0.486**
  → 证明 `wrist_rise` 测的不是「手在脸边」，而是「前倾/打字时肩线下降」。

**修法**：加硬约束 `wrist_near < 0.55`。阈值依据（实测）：

```
真实托腮（教学样本有效帧 n=9）:  0.360 ─ 0.450
生产误报（77 次触发）        :  0.420 ─ 1.334，中位 0.835

thr=0.45  压掉 97%  误杀 0/9
thr=0.50  压掉 94%  误杀 0/9
thr=0.55  压掉 90%  误杀 0/9   ← 取这个，留安全边际
thr=0.60  压掉 86%  误杀 0/9
```

**离线回放效果**：托腮占用时间 **41.7% → 1.2%**，拦下 **69/77（90%）** 触发，其它标签零影响。

**真人实测**：托腮时 `wrist_near = 0.39–0.47`（全部通过硬约束），15/15 帧正确判为托腮。

### 缺陷 2 ★ 强度归一化除零（1e9σ）

日志出现 `强度 -1.9e9` / `离正常最近的特征偏差 1970633268.4σ`。

根因：`wrist_face` 是新特征，板上无采集数据 → 画像里 MAD = 0（哨兵）。
而 `Classify`/`ClassifyHysteretic` 的 normal 回溯用 `max(mad, floor)` 当 spread，
`wrist_face` 的 floor 也极小 → 除出 1e9 量级，污染迟滞判定与日志。

**修法**：MAD=0 标记为「该特征无采集数据」，判定时跳过（`ClassStat::mad_missing`）。
修复后正常报 `4.3σ`。

### 缺陷 3 来了就报，不看持续

保持门限全是 `0.5s`（全局默认），人的自然点头/转颈立刻成报警。
生产 12h 发出 **225 次坐姿提醒（≈19 次/小时）**。

**修法**：逐类保持门限 —— 低头/歪头 **1.5s**、凑近 **1.0s**、托腮 2.5s。

### 附带：凑近阈值过严

教学样本里真凑近最小 **1.407**、正常最大 **1.129**，中间空隙很宽；
但阈值取中点 **1.3084** 时，用户实际前倾只到 **1.247** → 漏报。

改 **1.2000**（离正常最大值 1.129 仍留 0.07 余量）。实测 `size=1.466` 正确触发。

---

## 三、真人实测验收（板 68，2026-11-02 15:30–15:55）

| # | 动作 | 期望 | 实测结果 | 关键数据 |
|---|---|---|---|---|
| ① | 正常坐 | NORMAL，不误报 | ✅ 6/6 采样 NORMAL | `head_offset_y` −0.42~−0.50，零切换 |
| ② | 明显低头 | HEAD_DOWN | ✅ 判对 | `head_drop` 0.134 → 后续 0.18–0.22 |
| ③ | 歪头（耳靠肩） | HEAD_TILT | ✅ 稳定 | `ear_tilt` 17–27°（阈值 13.35） |
| ④ | 手托下巴 | HAND_ON_FACE | ✅ 15/15 | `wrist_near` **0.39–0.47**（硬约束 0.55 生效） |
| ⑤ | 前倾凑近 | TOO_CLOSE | ✅ 修正阈值后触发 | `size` 1.466（阈值 1.20） |
| — | **扭头（yaw，非歪头）** | 不应报警 | ✅ **正确不报** | `ear_tilt` 仅 7–12° |

> 说明：第 6 行是一次重要的**判定纠错**。用户最初以为扭头=歪头，
> 系统不报被当成漏报；核实后确认扭头是 yaw、歪头是 roll，系统行为正确。
> 这条已单独记录，避免以后把「用户描述与算法定义不一致」误当成 bug。

---

## 四、回归测试

```
服务自检    PASS=17  FAIL=0  WARN=0
单元测试    4/4 passed (posture_rules / session_timer / config / profile_hysteresis)
语音播放    6/6 exit=0
推理性能    15.9 ms/帧 (NPU), 25.7 fps
摄像头      640x480@30fps MJPG
```

---

## 五、部署清单与回滚

### 改动文件（板 68）

| 文件 | 改动 |
|---|---|
| `/opt/posture-guard/bin/posture_guard` | `7002a294` → `30154cb5`（含 mad_missing 修复） |
| `/var/lib/posture-guard/postures.txt` | 6 维 → **7 维**；`rule 4` 加 `wrist_near 0.55`；`rule 3` 阈值 1.3084 → 1.2000 |
| `/etc/posture-guard/posture.yaml` | 加 `profile_hold_*` 四类门限；`hand_face_wrist_rise` 0.35 → 0.20；补 `hand_face_wrist_face_max` / `hand_face_below_ear_min` |
| `/opt/posture-guard/assets/voice/*.wav` | espeak → 神经女声 24kHz，六条全换 |

**保留不变的**：`capture_fps: 30` / `infer_fps: 25` / `npu_core_mask: "0_1"` / `profile_release_ratio: 0.5`
（生产性能调优值，逐项核对未覆盖）

### 备份位置（可整体回滚）

```
/userdata/backup-voice-20261102-152558/       语音（6 个 wav）
/userdata/backup-posture-20261102-153715/     二进制 + etc + postures.txt
```

### 必须同批上线的原因
源码特征已是 **7 维**（新增 `wrist_face`），而画像文件原为 **6 维**。
只换二进制不换画像 → `wrist_face` 解析成 0 → 依赖它的规则行为不可预测。**两者绑定。**

---

## 六、已知遗留（未在本次修复）

1. **画像模式没有「高低肩」类**。生效画像只有 5 类
   （normal / head_down / head_tilt / too_close / hand_face），
   而旧规则路径有 `shoulder_tilt`。**当前判定走画像，所以高低肩永远不会被判为异常。**
   需补采集 `shoulder_tilt` 样本并加规则。
2. **`wrist_near` 在教学样本里 59% 是哨兵值**（13/22）。
   原因：`teach_posture.py` 读门控后的 `wrist_to_nose`，而 C++ 特征用 `wrist_to_nose_raw`。
   运行时生产日志 `wrist_near` 100% 有效，故不影响本次判定；但采集工具应修。
3. **语音文案仍是播报腔**（「请抬头，保持正确坐姿。」）。
   换音色已解决"机械感"，但句式仍像通知。口语化版本需 UI 侧支持多句轮换（未做）。
4. `too_close` 阈值 1.20 是**本机位本坐距**标定，换人/换椅子需重标。

---

# 追加修复（2026-11-02 16:30–16:45）

## 一、护眼「我已休息好」按钮无效 —— 静默失败

### 根因：JSON 冒号后有空格，解析器只认无空格

```cpp
// src/Notifier.cpp（修复前）
bool ExtractString(const std::string& text, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\":\"";   // ← 冒号后必须紧跟引号
  const size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
```

UI 用 `json.dumps({"cmd": "ack"})` 生成消息，python 默认分隔符是 `", "` 和 `": "`，
实际发出的是 `{"cmd": "ack"}`（**冒号后有一个空格**）。

`find('"cmd":"')` → `npos` → 命令被丢弃。**既不报错也不记录**。

### 实测证据链

1. 12 小时生产日志里 `用户确认已休息` 出现 **0 次** —— 所有护眼会话都靠「离开 10 秒超时」重置。
2. 13:38–13:42 连续 5 分钟每分钟重复提醒「该远眺休息了」—— 遮罩挂着关不掉。
3. 直接向 socket 发 `{"cmd": "ack"}` → 服务端状态 **BREAK_DUE 不变**，无任何日志。
4. `--verbose` 重启后发命令 → `收到 UI 命令` 出现 **0 次**（`LOG_DEBUG` 默认被 `kInfo` 过滤，所以平时完全看不见）。
5. 定位：`'{"cmd": "ack"}'.find('"cmd":"')` = **-1**。

### 修复

`ExtractString` / `ExtractNumber` 都改为：找 `"key":` 后**跳过空白**再取引号/数字。

同时顺带修掉 `ExtractNumber` 的同类问题：`{"minutes": 15}` 中冒号后是空格，
原实现 `b` 指向空格、`e` 立刻等于 `b` → `substr` 空 → `stod` 抛异常 → **minutes 永远回落默认值**。

### 验证（修复后实测）

```
ack 前：BREAK_DUE  已用 25s  剩余 0s
发送：  {"cmd": "ack"}            ← UI 同款格式，含空格
ack 后：ACTIVE     已用 3s   剩余 22s
日志：  用户确认已休息，开始新一轮 25s 计时     ← 12h 来第一次

snooze：{"cmd": "snooze", "minutes": 15}
日志：  已进入免打扰 900s                       ← minutes 正确解析（900 = 15×60）
```

### 连带修复：UI 静默失败
`_on_ack` 原来只管调 `send()`、不看返回值。现在：
- 成功 → 关遮罩 + 显示「已确认休息，开始新一轮计时」
- 失败 → 遮罩上显示「⚠ 与后台服务断开，确认未发送」

### 回归保护
新增 `tests/test_command_parse.cpp`（`ctest` 第 5 个套件，全部通过），覆盖：
- `{"cmd": "ack"}`（json.dumps 默认，**曾经失败**）
- `{"cmd":"ack"}`（紧凑格式）
- 冒号后多个制表符
- 键名前缀不同（`xcmd`）不被误命中
- `{"minutes": 15}` / `{"minutes":15}` / 小数 / 缺键 / 非数字值

---

## 二、高低肩从来没有被判过 —— 两层原因叠加

### 原因 1：画像里没有 shoulder_tilt 类

生效画像只有 5 类：`normal / head_down / head_tilt / too_close / hand_face`。
而 `PostureAnalyzer::Update` 里，`ProfileUpdate()` 一旦返回 true 就 **直接 return**，
传统规则路径（含 `shoulder_rule_`）**完全不会执行**。

→ 高低肩在画像模式下**永远不会被报出来**。

### 原因 2：即使命中也被映射成「正常」

```cpp
} else if (active_label_ == "hand_face") {
  obs->state = PostureState::kHandOnFace;
} else {
  obs->state = PostureState::kNormal;   // ★ shoulder_tilt 落到这里
}
```

新增规则后若不加分支，会「规则命中了但界面显示正常」，排查时完全看不出问题。

### 修复

1. **新增 `rule 5 shoulder_tilt shoulder_tilt + 12.0000`（排序最后）**

   阈值 12° 依据（教学样本 `shoulder_tilt` 分布，度）：

   | 姿势 | min | med | max |
   |---|---|---|---|
   | normal | 5.86 | 7.56 | 10.06 |
   | head_down | 0.06 | 2.24 | 8.46 |
   | **head_tilt** | **11.86** | **14.26** | **16.46** |
   | too_close | 3.36 | 5.86 | 8.76 |
   | hand_face | 9.46 | 9.56 | 12.96 |

   12° 时正常样本误报 **0/29**。

2. **排序最后是设计选择，不是随手放**：歪头会连带抬肩线（`head_tilt` 样本的
   `shoulder_tilt` 中位 14.26°，与高低肩高度重叠）。级联先命中 `head_tilt` 就 return，
   **天然给高低肩做了互斥门控**，不必再加一个 guard 参数。

3. `PostureProfile::PostureLabelZh` 补 `shoulder_tilt → "高低肩"`。

4. `PostureAnalyzer` 的标签→状态映射补 `kShoulderTilt` 分支；
   **并把 `else` 从 `kNormal` 改成 `kUnknown` + 明确说明**，
   避免以后再出现「命中规则却显示正常」这种静默错误。

### 验证

```
shoulder_angle_deg=12.7–15.8  →  SHOULDER_TILT 高低肩（释放阈 11.000）
日志：正常 -> 高低肩  命中 shoulder_tilt >= 12.000（实测 15.068）
     高低肩 -> 正常（退回到 1.9σ）
     正常 -> 高低肩  命中 shoulder_tilt >= 12.000（实测 12.144）
```
迟滞正常双向工作。

---

## 三、回归测试（5 套件全过）

```
服务自检   PASS=17  FAIL=0  WARN=0
单元测试   5/5 passed
           posture_rules / session_timer / config / profile_hysteresis / command_parse
语音播放   6/6 exit=0
```

## 四、本次改动文件

| 文件 | 改动 |
|---|---|
| `src/Notifier.cpp` | `ExtractString` / `ExtractNumber` 容忍冒号后空白 |
| `src/PostureProfile.cpp` | `PostureLabelZh` 补 shoulder_tilt |
| `src/PostureAnalyzer.cpp` | 标签→状态映射补 kShoulderTilt；else 改 kUnknown |
| `ui/posture_guard_ui.py` | `_on_ack` 给成功/失败反馈 |
| `tests/test_command_parse.cpp` | **新增**，19 条断言 |
| `CMakeLists.txt` | 注册 command_parse |
| `.dev-flow/regen_profile_v4.py` | 追加 rule 5 高低肩 |
| `/var/lib/posture-guard/postures.txt` | 加 `rule 5 shoulder_tilt + 12.0000` |

备份：`/userdata/backup-shoulder-ack-20261102-163508/`

## 五、遗留

1. 高低肩强度可能贴着阈值（实测 12.144 vs 12.000，强度 0.072）。若现场觉得太灵敏，
   调到 12.5–13。**当前不动**：它抓到的是用户真实抬肩。
2. 因为画像里没有 `shoulder_tilt` 的训练样本，rule 5 的中位参考值（7.5570 / 14.2570）
   是借 normal / head_tilt 的中位填的，**只影响日志显示，不影响判定**（判定只看阈值和方向）。
3. `teaching` 工具仍未支持采集 `shoulder_tilt` 样本。要真正标定，需补采集。
