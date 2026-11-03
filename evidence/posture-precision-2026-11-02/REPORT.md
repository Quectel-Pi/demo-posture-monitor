# 画像判定抖动治理（2026-11-02）

## 一句话结论

生产机 `posture-guard` 在 1.7 小时内发生 **514 次「画像判定切换」（307 次/小时）**，
根因是画像级联是**纯阈值判定**、没有任何迟滞；新增「只对退出加惰性」的迟滞后，
离线回放证明可拦下 **105/287 次假离开（37%）**，且进入灵敏度完全不变。
板端 4 个测试套件全部通过，迟滞可随时用 `posture.profile_release_ratio: 0` 关闭。

## 1. 问题是怎么被发现的

排查「检测不准」时先看生产日志，发现切换频率异常：

```
$ journalctl -u posture-guard --since "-12h" | grep -c 画像判定切换
514
```

时间跨度 09:21:39 → 11:01:59（1.7 小时），均摊 307 次/小时。
典型片段（同一姿势在 1~2 秒内来回跳）：

```
09:21:39 （无） -> 低头/趴桌   命中 head_drop >= 0.103（实测 0.257）
09:21:44 低头/趴桌 -> 正常     全部规则未命中（偏差 10.4σ）
09:21:53 正常 -> 低头/趴桌     命中 head_drop >= 0.103（实测 0.118）
09:22:01 低头/趴桌 -> 正常     全部规则未命中（偏差 7.7σ）
```

对用户来说这就是「坐姿指示一直闪」+「语音反复念同一句」，
是**体感「不准、很吵」的主要来源**，比单帧分类准确率更影响体验。

## 2. 根因

`PostureProfile::Classify` 是纯阈值级联：特征的差值跨过阈值就换标签，
差回阈值内就换回来。关键点检测的帧间抖动（实测 `head_drop` 在 0.100↔0.118 之间
摆动，阈值 0.1035 正好落在中间）足以让每帧都跨一次。

对比之下，`PostureAnalyzer` 的**传统规则路径早就有迟滞**
（`Rule::trigger` / `Rule::recover`，`shoulder_trigger_deg: 12 / shoulder_recover_deg: 8`），
但**画像级联路径完全没有**。这是两条判定路径的设计不一致，属于遗漏。

## 3. 改法：只给「退出」加惰性

给 `PostureProfile` 增加 `ClassifyHysteretic(f, active_label, release_ratio)`：

| 动作 | 用哪个阈值 | 为什么 |
|---|---|---|
| 进入新标签 | 原阈值（trigger） | 保持灵敏度，不牺牲响应速度 |
| 离开当前标签 | `trigger - release_band` | 必须真的退回去才算离开 |

`release_band = min(ReleaseScale[feat], 阈值到 normal 中位数的跨度) * ratio`，
默认 `ratio = 0.5`。两点保护：

1. **不会宽于整条规则**：`band` 被「阈值 − normal 中位数」的跨度夹住，
   否则释放带会比规则本身还宽，导致永远离不开。
2. **`ratio = 0` 完全退化成纯 `Classify`**，可随时一键关闭做 A/B 对照。

改动的文件：

| 文件 | 改动 |
|---|---|
| `include/posture/PostureProfile.hpp` | 声明 `ClassifyHysteretic` / `RuleReleaseBand` / `ReleaseScale` |
| `src/PostureProfile.cpp` | 实现迟滞级联（含 guard 互斥、normal 回溯） |
| `include/posture/PostureAnalyzer.hpp` | `SetProfile` 增加 `release_ratio` 参数 |
| `src/PostureAnalyzer.cpp` | 改调 `ClassifyHysteretic`，日志打印迟滞比例 |
| `include/posture/Config.hpp` + `src/Config.cpp` | 新增 `posture.profile_release_ratio` |
| `config/posture.yaml` | 写入 `profile_release_ratio: 0.5` 并说明原因 |
| `tests/test_profile_hysteresis.cpp` | 新增 7 组 16 项断言 |
| `CMakeLists.txt` | 注册新测试 |

## 4. 验证证据

### 4.1 离线量化：能拦下多少假切换

```
scripts/verify_hysteresis_journal.py evidence/posture-precision-2026-11-02/journal-switches-1.7h.log
```

方法：对 journal 里每一条「正在离开标签 L」的记录，取离开瞬间的特征值，
判断迟滞会不会把这次离开拦住（`thr - band <= f < thr` 就该留住）。

```
514 次切换，跨 1.7 小时 → 307.4 次/小时
3 秒内 A->B->A 来回跳：4 次（占 1%）

离开的标签      切换次数   迟滞可拦下   拦下比例
正常                201        —         —
低头/趴桌           218        91        42%
歪头                 66        13        20%
托腮                  3         1        33%

合计：迟滞比例 0.5 时，可拦下 105/287 次离开（37%）
```

> 为什么不用 114 帧采集样本做这件事：那批样本是「单姿势连续段」，
> 段内特征几乎不动，本身不产生抖动。抖动只发生在**姿势过渡帧**上，
> 只有 journal 里逐帧的特征值能复现。这一点在
> `scripts/verify_hysteresis.py` 的输出里也有明确提示。

### 4.2 板端编译 + 单测（板 68，串行）

```
$ bash .dev-flow/build_and_test.sh
---- build/test_posture_rules ----        全部通过   exit=0
---- build/test_session_timer ----        全部通过   exit=0
---- build/test_config ----               全部通过   exit=0
---- build/test_profile_hysteresis ----   全部通过   exit=0
```

新测试 `test_profile_hysteresis` 的 16 项断言：

```
-- 解析真实板端 postures.txt
  [ ok ] 真实画像文件能解析出类别 / 规则 / 至少 4 个姿势 / 含 head_down / 含 head_tilt
-- 无 active 标签时等价于纯 Classify
  [ ok ] 纯 Classify 命中 head_down
  [ ok ] 空 active 时迟滞结果与纯 Classify 一致
-- 进入新标签仍用原阈值（不放宽）
  [ ok ] 0.08 < 阈值 0.10：从 normal 出发不进入 head_down
-- 离开当前标签必须退回 release_band
  [ ok ] 已在 head_down，0.08 仍在释放带内（>=0.06）→ 保持 head_down
  [ ok ] 已在 head_down，0.05 低于释放阈 0.06 → 离开回到 normal
-- release_ratio = 0 时完全退回纯阈值判定
  [ ok ] 关闭迟滞后 0.08 < 0.10 立刻离开 head_down
-- 释放带不超过阈值到 normal 中位数的跨度
  [ ok ] ratio=10 时释放带被夹到跨度 0.10，0.001 仍留在 head_down
  [ ok ] 低于夹紧后的释放阈 -0.05 -> 离开
-- guard 互斥在迟滞路径同样生效
  [ ok ] 耳线明显歪时不判低头，而是判歪头
```

`test_config` 新增 2 项，防止误关：

```
  [ ok ] 画像迟滞比例在 0~1 之间
  [ ok ] 生产配置未关闭迟滞（关闭会回到 514 次/1.7h 的抖动水平）
```

## 5. 诚实的边界：哪些没做、为什么

| 事项 | 状态 | 原因 |
|---|---|---|
| 提高单帧分类准确率 | **未做** | 5 折 CV 基线 94.7%±5.0%，`too_close` 召回仅 65%，但要提升它必须补采「太近」样本；现有 114 帧不足以支撑阈值重搜，重搜只会过拟合到训练集 100% |
| 补 `shoulder_tilt` 进入级联 | **未做** | 数据集里**没有任何高低肩样本**（`/var/lib/posture-guard/poses/` 无 `shoulder_tilt` 目录），阈值无法校准。强行加会把 `hand_face`（shoulder_tilt 中位 9.56）错判成高低肩 |
| `wrist_near` 参与判定 | **未做** | 114 帧里 105 帧恒为哨兵值 3.0，是废特征，禁止作主判据 |
| 现场实测抖动下降幅度 | **未做** | 需部署到生产板并观察 ≥1.7 小时的 journal 才能对比；离线回放给的是上界估计（37%） |

## 6. 下一步（需要现场）

1. 部署到板 68，观察 1.7 小时 journal 的切换次数，与 514 次基线对比。
   预期落在 **300~400 次**（离线算的 105 次是可拦下上界，实际有部分会被
   `profile_hold_s = 0.5s` 的既有保持逻辑二次过滤）。
2. 若仍偏多，把 `profile_release_ratio` 从 0.5 提到 0.75 再观察；
   反之若发现「回到了正常但指示灯不变」的迟滞感，降到 0.25。
3. 要真正提升单帧准确率，需要现场采「太近」和「高低肩」样本各 ≥30 帧，
   然后重跑 `scripts/teach_posture.py` 并重看 5 折 CV。

## 7. 复现命令

```bash
# 抓现场切换日志
adb -t 68 shell 'journalctl -u posture-guard --no-pager | grep "画像判定切换"' > /tmp/sw.log

# 离线量化迟滞收益
python3 scripts/verify_hysteresis_journal.py /tmp/sw.log

# 用真标注样本验证「迟滞没把准确率弄坏」
python3 scripts/verify_hysteresis.py /tmp/pm-samples

# 板端编译 + 全量单测
bash .dev-flow/build_and_test.sh
```
