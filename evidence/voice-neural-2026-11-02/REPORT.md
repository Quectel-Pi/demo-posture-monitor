# 语音素材：机械音 → 离线神经女声（2026-11-02）

用户诉求：「语音播报的提示有点 ai 语音味道，帮我使用女生类人的音效提醒」。

## 结论

`assets/voice/*.wav` 六条全部由 **edge-tts 神经女声 `zh-CN-XiaoxiaoNeural`**
重新烘焙，运行期仍然 100% 离线（板端 `paplay` 直放 WAV，零新依赖）。
`libespeak-ng` 脚本保留为无网兜底。

## 客观指标（新 vs 旧）

| key | 新 时长 | 新 峰值 | 新 RMS | 新 F0 | 旧 时长 | 旧 峰值 | 旧 RMS | 旧 F0 |
|---|---|---|---|---|---|---|---|---|
| shoulder_tilt | 2.08s | 89.0% | 20.1% | 296.3Hz | 3.25s | 100.0% | 19.3% | 91.5Hz |
| head_down | 2.35s | 89.0% | 19.8% | 230.8Hz | 3.45s | 100.0% | 19.1% | 87.8Hz |
| head_tilt | 2.45s | 89.0% | 20.2% | 259.5Hz | 3.68s | 100.0% | 19.1% | 90.4Hz |
| too_close | 2.99s | 79.4% | 19.8% | 244.9Hz | 4.27s | 100.0% | 19.9% | 88.7Hz |
| hand_on_face | 2.14s | 89.0% | 19.7% | 240.0Hz | 3.09s | 100.0% | 19.5% | 87.8Hz |
| rest | 5.85s | 89.0% | 21.0% | 244.9Hz | 9.44s | 100.0% | 20.1% | 87.5Hz |

格式：全部 24000Hz / mono / 16bit。

F0 = 浊音段自相关中位基频。旧素材 ~88Hz（偏低，男声区 + 拼接式机械音），
新素材 230~296Hz（典型成年女声区）。这是「AI 味」消失的客观依据之一：
不再是单一固定基频 + 无共振峰过渡的拼接音。

其他改进：

* 峰值从 100%（espeak 直出必然顶满，实际已在削波边缘）降到 89%（-1dBFS）。
* RMS 19.7~21.0%，与旧素材持平或略高 —— **不会出现「换了更好听的女声反而更轻」**。
* 时长全部缩短（去掉首尾静音 + 语速调整），`rest` 从 9.44s → 5.85s，
  播报不再拖沓。

复现：`python3 evidence/voice-neural-2026-11-02/measure.py`

## 无网复现方法

```bash
python3 scripts/make_voice_assets.py   # libespeak-ng，完全离线，机械音兜底
```

产物同名同格式（24kHz/mono/16bit），`python3 scripts/make_voice_assets_neural.py --check`
照样全绿，UI 侧不需要任何改动。

## 试听建议

在板端 `su quectel -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/1000 paplay assets/voice/<key>.wav"`，
或点界面「试听语音」（播的是 `head_down`）。

* **最自然**：`head_down`（「请抬头，保持正确坐姿。」）—— 句长 2.35s，
  有完整的陈述句语调回落，最接近真人提醒。
* **次选**：`shoulder_tilt`（「请坐正，放平双肩。」）—— 短促、干脆，
  适合高频提醒，F0 296Hz 是六条里最亮的。
* `rest` 是唯一的长复合句（5.85s），信息量最大，语气最柔和，
  但试听时最好确认一下喇叭对 5 秒以上播放不中断（VoicePlayer 允许 `rest` 打断别的播报）。
* `too_close` 的峰值只有 79.4%（句内有明显重音起伏，limiter 只在个别音节介入），
  听感动态更大，但和其它几条并排比会显得稍微轻一点点 —— 属于正常表现，不建议再压。

## 踩到的坑（已在脚本里修掉，别再踩回去）

1. **单遍 `loudnorm` 对短句会算错增益**。ffmpeg 4.4 在片段 <3s、LRA 算成 0.00
   时给出错误结果：实测把 2.76s 的「请坐正，放平双肩」从 RMS 19% 压到 **3.6%**
   （小了 14dB，反而听不见）。两遍法（`print_format=json` 回填 `measured_I`）
   依然复现。最终改成「测 integrated loudness → 换算增益 → limiter」的确定性链路。
2. **`alimiter` 默认 `level=true` 会把信号拉回满刻度**，等于白限幅。必须
   `level=false`。旧脚本漏了这个参数，所以六条峰值都是 100.0%。
3. **神经 TTS 首尾各留 0.2~0.4s 静音**，不去掉会白占播放时长（`rest` 光尾部就有 0.62s）。

## 文件清单

| 文件 | 变更 |
|---|---|
| `scripts/make_voice_assets_neural.py` | 新增，edge-tts 神经女声烘焙 + 校验 |
| `scripts/make_voice_assets.py` | docstring 说明兜底关系；`alimiter level=false`；输出改 24kHz mono 16bit |
| `assets/voice/*.wav`（6 个） | 重新生成，24000Hz/mono/16bit |
| `tests/test_voice.py` | 新增 `VoiceAssetTests`：6 个 key 存在、格式、时长、峰值、RMS、无孤儿 wav |
| `README.md` | 「中文语音」行 + 语音生成方式/兜底方案 |
| `docs/architecture.md` | 新增 §7.1 语音播报（进程归属、白名单、两条生成路径、loudnorm 坑） |
| `docs/troubleshooting.md` | §17.3 重写（确定性归一化链路）；新增 §17.4「提示音听起来像 AI」；原 §17.4/17.5 顺延 |
| `.gitignore` | 忽略 `assets/voice/.neural_mp3/` |
