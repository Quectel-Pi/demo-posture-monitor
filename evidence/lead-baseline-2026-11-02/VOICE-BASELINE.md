# 语音基线（lead 实测，2026-11-02 15:07）

## 核心结论
**板 68 从未部署过神经女声。** 板上 6 个 WAV 的 md5 与 `git HEAD`（espeak 版）
**逐一完全相同**，与工作区（xiaoxiao 神经版）**全部不同**。

即：用户听到的"机械味"是 espeak-ng，不是神经女声选型失败。

## md5 证据

| key | 板 68 `/opt/posture-guard/assets/voice/` | `git HEAD` | 工作区（神经版） |
|---|---|---|---|
| hand_on_face | `64e87aa67ef85f268df894e6d55ee1d4` | `64e87aa6…` ✅同 | `435996b2…` ❌异 |
| head_down | `4f97f191a38f6f171d37ddad7ce15485` | `4f97f191…` ✅同 | `d73ab3ee…` ❌异 |
| head_tilt | `e25148f5f8682f857098612a88fd1369` | `e25148f5…` ✅同 | `d7a022d2…` ❌异 |
| rest | `4f566a6313c68bf450e849f2bda0ec81` | `4f566a63…` ✅同 | `edfd9b76…` ❌异 |
| shoulder_tilt | `c997c1c10b36ee3da2002d815f0bb19e` | `c997c1c1…` ✅同 | `3d47953c…` ❌异 |
| too_close | `ec00134c05b26d31bdb0c87ac41e8bed` | `ec00134c…` ✅同 | `8a3bc072…` ❌异 |

## 客观指标对比

| 项 | 板上（espeak-ng） | 工作区（zh-CN-XiaoxiaoNeural） |
|---|---|---|
| 采样率 | **22050 Hz** | 24000 Hz |
| head_down 时长 | 3.45 s | 2.35 s |
| hand_on_face 时长 | 3.09 s | 2.14 s |
| rest 时长 | 9.44 s | 5.85 s |
| 峰值 | **1.000（全刻度）** | 0.89 |
| 削顶样本数 (head_down / hand_on_face / rest) | **16 / 3 / 29** | **0 / 0 / 0** |
| 过零率 head_down | 2613 /s | 4222 /s |
| 格式 | 16bit mono | 16bit mono |

## 由此得出的判断
1. espeak 是共振峰拼接合成，逐字音素级拼接 → 这就是"机械味"的物理来源。
2. 板上素材**已经削顶**（峰值 1.000，削顶 16/3/29 个样本）→ 除机械音外还叠加了轻微破音。
3. 换神经女声是**净收益、零风险**（格式相同，`paplay` 路径不变，UI 零改动）。
4. 但"换音色"这一步在**用户听过神经女声之前**是无法定论的 —— 必须先上板试听，
   再决定是否换 xiaoxiao 以外的音色。
