# 板 2（RK3576）姿势识别精度修复报告

日期：2026-11-03
设备：transport 2 / `Quectel PI M2 RK3576 Board` / `linaro-alip` / Debian 13
问题：用户反馈「姿势识别不准，有时会错误识别托腮」

---

## 1. 测试方法与口径

**排除了两批不可用的数据**（重要，避免得出错误结论）：

| 数据 | 为何不可用 |
|---|---|
| `/var/lib/posture-guard/poses/` 83 张图 | `away`（无人）目录里**明显有人**（YOLO conf 0.89）；且 10-29 采集，与今天 10:08 重建的校准基准机位不同。用它算出 16.9% 是失真数字，弃用。 |
| 板 68 的 `/tmp/pm-samples/` | 另一台设备、另一次校准，与板 2 不可比。 |

**采用的数据**：`/var/lib/posture-guard/posture-samples/*.json`
- 5 个姿势共 **114 帧**，全部由**同一校准基准**（`2026-10-29T14:35:29`）算出特征 → 可横向比较
- 特征已在板端算好，不受重放实现差异影响
- 评分脚本：`score_samples.py`（严格照抄 C++ 的级联规则）

---

## 2. 测试结果：托腮是唯一失效的类别

**修复前**（板上生效规则）：

```
真值        normal  head_down  head_tilt  too_close  hand_face   召回
normal          29                                              100%
head_down            24                                        100%
head_tilt                    22                                100%
too_close                            17                        100%
hand_face        9                       5          7    ← 7/22 = 31.8%
总体 99/114 = 86.8%
```

22 帧真实托腮里，**9 帧被判成「正常」、5 帧判成「离屏太近」**，只有 7 帧正确。
其余四类全 100% —— 问题精确落在托腮。

---

## 3. 根因（两个，叠加）

### 根因 A：`wrist_near` guard 把托腮全挡掉

板上规则是：

```
rule 4  hand_face  wrist_rise >= 0.286   guard: wrist_near < 0.55
```

逐帧看这 22 帧：**`wrist_rise` 全部通过**（0.296~0.372，阈值 0.286），
但有 **13 帧的 `wrist_near` 是哨兵值 3.0** → 被 `< 0.55` 全部挡掉。

哨兵的含义是「**测不到**」，不是「手离脸很远」。guard 把它读反了。

来源：板上跑的 `teach_posture.py` 是老版（6 维），托腮候选特征是
`["wrist_near", "wrist_rise"]`，于是把它生成为互斥 guard。

### 根因 B：`too_close` 阈值 1.20 抢走托腮

托腮时人前倾、肩宽变大 → `size` 撞上 1.20 的 `too_close` 阈值（排在托腮之前）。
22 帧里 5 帧被判成「离屏太近」即此因。

**数据给出的分隔区间**：
- `hand_face` 的 size：min 1.121 / med 1.162 / **max 1.210**
- `too_close` 的 size：**min 1.407** / med 1.438 / max 1.482

两类之间有 1.21~1.40 的干净空隙。

### 附带发现的真实代码缺陷

`m.wrist_to_nose_raw = m.wrist_valid ? best : 99.f` 可能出现
`wrist_valid=true` 但 `wrist_to_nose_raw` 停在哨兵 99 的帧（历史数据 13/22 帧如此）。
两个量由不同条件决定，就会出现不一致。已在代码层修掉。

---

## 4. 修改内容

### 4.1 规则文件 `/var/lib/posture-guard/postures.txt`

| 项 | 改前 | 改后 |
|---|---|---|
| rule 4 hand_face | `wrist_rise>=0.286` + guard `wrist_near<0.55` | `wrist_rise>=0.286`（去掉 guard） |
| rule 3 too_close | `size>=1.2000` | `size>=1.3000` |

阈值 1.30 的依据：满分区间是 1.21~1.40（扫描 1.21/1.22/1.25/1.28/1.30/1.32/1.35/1.38/1.40 全部 100%），
取区间中值，不贴边，抗过拟合。

**为什么没有移动规则顺序**：把 `hand_face` 提到 `too_close` 之前会让
11/17 帧「凑太近」被错判成托腮（总体掉到 89.5%）。改阈值是唯一无副作用的解。

### 4.2 `src/PostureAnalyzer.cpp`

1. **手腕哨兵一致性**：`wrist_valid` 与 `wrist_to_nose_raw` 绑到同一条件，
   并剔除坐标外推的垃圾值（`kMaxWristToNose = 8.0`）。
2. **托腮「手确实贴脸」约束**：`wrist_to_nose_raw > hand_face_near_max` 时拒绝托腮。
   - **只在测量有效时生效**：哨兵（>50）表示测不到，必须放行。
   - 这条修的是实测发现的真问题：手已离开脸（`wrist_near=1.09`）但因腕高仍高于
     基准被钉在「托腮」不退（实测持续 **62s**）。

### 4.3 `scripts/teach_posture.py`

1. `hand_face` 候选特征改为只有 `wrist_rise`，不再生成 `wrist_near` guard。
2. `best_threshold()` 新增 `prior_rules` 参数：算阈值时剔除会被前序规则抢走的帧，
   避免搜出「单看完美、级联全错」的阈值（`too_close` 就是这个坑）。

### 4.4 配置

`hand_face_near_max: 0.85`（实测：托腮 0.44~0.49 | 手离开脸 1.09 | 正常坐 1.05~1.74）

---

## 5. 修复验证

### 5.1 量化回放（114 帧）

```
真值        normal  head_down  head_tilt  too_close  hand_face   召回
normal          29                                              100%
head_down            24                                        100%
head_tilt                    22                                100%
too_close                            17                        100%
hand_face                               0        22            100%
总体 114/114 = 100.0%
```

| 阶段 | 总体 | 托腮召回 |
|---|---|---|
| 修复前 | 86.8% | 31.8% |
| 去掉 wrist_near guard | 95.6% | 77.3% |
| 再修 too_close 阈值 | **100%** | **100%** |

**其余四类零退化**（normal 仍 29/29，无误报）。

### 5.2 真人端到端实测（板 2，2026-11-03 10:38~10:46）

| 动作 | 结果 |
|---|---|
| 正常坐姿 20s | 稳定 `NORMAL`，六次采样全一致，零抖动 ✅ |
| 托腮保持 | 正确判 `HAND_ON_FACE 托腮`，`wrist_rise` 实测 0.48~0.52（阈值 0.236）✅ |
| 手放下 | 正确退回 `NORMAL`，五次采样稳定 ✅ |

实测记录：
```
10:38:22 正常 -> 托腮  wrist_rise=0.506  wrist_near=0.463  hold=2.5s
10:39:32 托腮 -> 正常  wrist_rise=0.187  wrist_near=1.088
```

### 5.3 单元测试

- 全量 4/4 测试文件 exit=0
- 新增 `TestWristSentinelConsistency`（5 断言）：手腕不可信/坐标外推/正常有效
- 新增 `TestHandFaceReleasesWhenArmLeavesFace`（6 断言）：手贴脸放行、手离开拒绝、
  **测不到必须放行**（这条是根因 A 的回归保护）

---

## 6. 遗留问题（未修，需决策）

### 6.1 取景不合格（物理摆放）

`frame_check.py` 判定 ❌：

```
人体框占画面高 99%（要求余量 ≥6%）
头顶出框 -24%
```

影响：
- 手腕经常不在画面内 → `wrist_to_nose=99`（测不到）→ 托腮无法判定
- 偶发「鼻子关键点跑到画面外」→ `UNKNOWN`
- 误检出 1~6 人（实际 1 人）

**这是物理问题，代码改不了**：需要把摄像头往后移 / 让人坐远 / 镜头往上仰。

### 6.2 `wrist_face` / `face_arm_raise` 特征未经标定

已加入代码和采样脚本，但历史 114 帧是 6 维、没有新特征，
所以这两个特征目前**没有校准数据**，默认阈值 0.95 是按物理尺度估的。
建议采一轮新样本后再启用。

### 6.3 板 68 与板 2 配置不同步

板 2 是当前主用设备，板 68 的规则/配置未同步本次修复。

---

## 7. 复现方式

```bash
# 1. 拉样本
adb -t 2 pull /var/lib/posture-guard/posture-samples /tmp/ps2
# 2. 评分（无需板端参与）
python3 evidence/accuracy-board2-2026-11-03/score_samples.py "/tmp/ps2/*.json"
# 3. 带标签图片的关键点导出（可选，用于诊断）
adb -t 2 shell 'bash /tmp/export_kp.sh'
adb -t 2 pull /tmp/kp_export.jsonl evidence/accuracy-board2-2026-11-03/kp_export.jsonl
python3 evidence/accuracy-board2-2026-11-03/replay.py
```

## 8. 回滚

```bash
adb -t 2 shell 'sudo systemctl stop posture-guard \
  && sudo cp /userdata/backup-board2-20261103-posture_guard /opt/posture-guard/bin/posture_guard \
  && sudo cp /userdata/postures-backup-20261103.txt /var/lib/posture-guard/postures.txt \
  && sudo systemctl start posture-guard'
```

- 旧二进制：`/userdata/backup-board2-20261103-posture_guard`（md5 `3e3c153dd15fc6124748846de930c83a`）
- 旧规则：`/userdata/postures-backup-20261103.txt`
- 新二进制：md5 `b96f32f3037b304c22037df347839b2b`

---

# 追加：播报延迟从 ~5 秒降到 ~1 秒（2026-11-03 11:00~11:30）

## 问题
用户反馈「判断姿势不正确时，播报提醒有 5 秒延迟」。

## 根因：三处延迟叠加，其中两处是同一类设计缺陷

### 延迟链（改前）
| 环节 | 耗时 |
|---|---|
| 画像标签确认（`profile_hold`） | 低头/歪头 1.5s、托腮 2.5s |
| 播报门限（`posture_hold_s`） | 2.0s |
| **合计** | **3.5~4.5s**（叠加人动作起势时间，用户感知约 5 秒）|

### 更深的问题：两处都在用「连续保持 N 秒」去抖
`profile_hold` 和 `posture_hold_s` 都是「同一标签**无间隔**连续保持 N 帧/秒」。
这个模型有个死结：**一个噪声帧就把计时清零**，所以 N 必须设得很大才稳。
延迟和精度被绑死，只能二选一。

实测证据：11:18:52.962 稳定低头已确认，但 11:18:57.848 才播报 —— **+4.89 秒**。

## 修改

### 1. 姿势层：连续保持 → 时间窗投票（`PostureAnalyzer`）
只看最近窗口内各标签占多少帧，占多数就采纳。偶发噪声只影响一票，不会清零。

- 进入：窗口 **0.6s**，≥80% 帧支持即切换
- 退出：窗口 **1.2s**，≥70% 帧支持才切回（退出仍稳，防抖不降级）
- 本帧算不出特征 / 丢点 / 多人歧义时清空投票（不让过期票维持旧状态）

**实测效果（单测断言）**：
- 纯低头 → **0.20s** 内确认
- 每 5 帧插一个噪声 → **0.33s** 内确认（旧机制会一直卡住）

### 2. 播报层：`posture_hold_s` 2.0 → 0.5
姿势层已去抖，这里再等 2 秒是重复去抖，纯加延迟。

### 3. 播报层：修「噪声清零」缺陷（`ReminderManager`）
`ConsiderPosture` 同样用连续计数，且把「姿势正常」和「指标无效」混为一谈
（正常姿势没有提醒 key → 走无效分支 → 直接清空计时）。

改为：
- 区分「数据无效」（真重置）和「姿势正常」（占位键参与容忍）
- 候选键单独计时，0.2s 内回到主键视为抖动，主键计时不受影响
- **播报永远按当前帧的键用其自身起始时间计**，所以刚切换的姿势
  绝不会蹭用上一个姿势攒下的时长（这条踩过：一度导致立即误播）

### 4. UI 语音层：节流 10s → 2s（`ui/posture_guard_ui.py`）
旧值 10 秒会把「先报低头、3 秒后改报托腮」中的托腮直接吞掉，用户以为没反应。
现在同一条语音不重播，换姿势则立即打断旧语音播新语音（旧行为是直接丢）。

## 结果

| 指标 | 改前 | 改后 |
|---|---|---|
| 姿势确认（画像层） | 1.5~2.5s | **0.20s**（噪声下 0.33s）|
| 切换 → 播报 日志间隔 | +2.02s / +4.89s | **+0.52s** |
| 用户感知总延迟 | 约 5s | **约 1s** |
| 单帧噪声导致漏报 | 会（计时清零） | 不会（投票制） |
| 0.13s 瞬时误判被报出 | 可能 | 不会（有单测）|

## 测试
- 全量 4/4 测试文件 exit=0
- 新增 `TestVoteAcceptsFastButStable`（4 断言）：确认速度、抗噪、挡毛刺、能退出
- 新增 `TestNoiseDoesNotDelayReminder`（5 断言）：噪声不拖慢播报
- 新增 `TestRealPoseSwitchStillWaits`（4 断言）：真换姿势必须重新计时
- 改写 2 条固化了旧慢行为的断言（`画像稳定 2 秒`、`语音提醒等待至少 2 秒`）

## 部署
- 二进制 md5 `5bf9f42bec7f5f74a18d0996aaecb80e`
- 备份 `取决于环节`：`/userdata/backup-board2-20261103-vote-pre.bak`
- 配置：`posture_hold_s: 0.5`（板上 + 仓库同步）
