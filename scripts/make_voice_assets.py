#!/usr/bin/env python3
"""无网兜底：用主机 libespeak-ng 合成中文 WAV（机械音）。

════════════════════════════════════════════════════════════════════
和 make_voice_assets_neural.py 的关系
════════════════════════════════════════════════════════════════════
  正式方案（默认，推荐）：scripts/make_voice_assets_neural.py
      edge-tts 神经女声 zh-CN-XiaoxiaoNeural，听感接近真人，没有 AI 机械音。
      需要联网烘焙一次，产物是普通 WAV，**运行期完全离线**，板端零新依赖。

  本文件 = 兜底方案（无网 / 装不上 edge-tts 时用）
      libespeak-ng 本机合成，完全离线，但音色是机械音。

两者写同一批文件（assets/voice/*.wav）、同一批 key、同一个采样率与
归一化目标，所以可以随时互换，UI 侧（paplay）不需要任何改动：

    python3 scripts/make_voice_assets_neural.py   # 有网，女声
    python3 scripts/make_voice_assets.py          # 无网，espeak 兜底

文案 PROMPTS 定义在**本文件**里，神经脚本直接 import 复用，避免两处漂移。

════════════════════════════════════════════════════════════════════
文案为什么是口语化的
════════════════════════════════════════════════════════════════════
旧文案是播报体（「请抬头，保持正确坐姿。」「请坐正，放平双肩。」），
即使换成神经女声，念出来仍像车站广播 —— 这是独立于音色的第二层「AI 味」。
现在主文案改成旁边有人轻声提醒的口语（「诶，头有点低啦，抬一抬～」），
并且每个 key 另有 2 个轮换变体，见 PROMPT_VARIANTS。

★ 变体是**可选**的：UI（ui/posture_guard_ui.py 的 VoicePlayer）只按
  assets/voice/<key>.wav 取文件，所以变体写在 assets/voice/variants/
  子目录里，不改变现有 UI 契约，也不会被 tests/test_voice.py 的
  「无孤儿 wav」检查误伤。要真正启用轮换需要 UI 侧改动，见
  evidence/voice-humanlike-2026-11-02/REPORT.md 的「需要 UI 侧配合」一节。

★ 生成完必须做响度归一化。
  espeak 直出的 wav 峰值能到 90%+，但 RMS 只有 ~11%，听感比满刻度正弦低 9dB ——
  实测「喇叭明明在响，就是听不清」就是这么来的。
  这里用 ffmpeg 做 compress + loudnorm + limiter，把 RMS 提到 ~19%（+5dB）；
  没有 ffmpeg 时退化成纯 Python 的峰值归一化（提升有限但不会更差）。
  输出统一 resample 到 24kHz mono 16bit，和神经女声产物格式一致
  （tests/test_voice.py 对格式做静态校验，两条生成路径都要过）。
"""
import ctypes
import ctypes.util
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import wave

# ── 主文案（UI 直接播放的 6 条）──────────────────────────────────
# 口语提醒体：句首轻起音（诶/嗯/哎）、句尾语气词（啦/哈/～）、
# 不使用「请……，保持……」这种书面祈使句。
PROMPTS = {
    'shoulder_tilt': '肩膀歪啦，两边放平一点～',
    'head_down': '诶，头有点低啦，抬一抬～',
    'head_tilt': '头歪啦，正一正哈～',
    'too_close': '有点太近啦，往后坐一点点～',
    'hand_on_face': '手放下来吧，别托着腮啦～',
    'rest': '看屏幕挺久啦，抬头看看远处，让眼睛歇一会儿～',
}

# ── 轮换变体（可选，需 UI 侧支持；写在 assets/voice/variants/）──────
# 目的：同一个姿势反复触发时不会每次都是同一句话（真人不会一字不差重复）。
# 每个 key 2 条，加上主文案共 3 条可轮换。
PROMPT_VARIANTS = {
    'shoulder_tilt': ['两个肩膀端平哈，别一边高一边低。',
                      '坐正一点，肩膀放松放平～'],
    'head_down': ['头低下去啦，抬起来一点哈。',
                  '稍微抬抬头吧，脖子别一直勾着～'],
    'head_tilt': ['脑袋有点偏，摆正一点～',
                  '头回来一点点，别一直歪着。'],
    'too_close': ['离屏幕太近啦，往后退一点。',
                  '眼睛离屏幕远一点哈，别凑那么近～'],
    'hand_on_face': ['手拿开哈，托腮容易歪脖子。',
                     '托着腮呢，把手放下来吧。'],
    'rest': ['该歇歇眼睛啦，往窗外看看，二十秒就行。',
             '眼睛累了哈，看远处放松一下，二十秒再回来～'],
}


def main():
    lib = ctypes.CDLL(ctypes.util.find_library('espeak-ng') or 'libespeak-ng.so.1')
    callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int, ctypes.c_void_p)
    chunks = []
    @callback_type
    def callback(samples, count, events):
        if samples and count:
            chunks.append(ctypes.string_at(samples, count * 2))
        return 0
    rate = lib.espeak_Initialize(2, 0, None, 0)
    if rate <= 0:
        raise RuntimeError('espeak initialization failed')
    lib.espeak_SetSynthCallback(callback)
    if lib.espeak_SetVoiceByName(b'cmn') != 0:
        raise RuntimeError('Mandarin voice unavailable')
    lib.espeak_SetParameter(1, 155, 0)
    output = Path(__file__).resolve().parent.parent / 'assets' / 'voice'
    output.mkdir(parents=True, exist_ok=True)
    jobs = [(output / (key + '.wav'), prompt) for key, prompt in PROMPTS.items()]
    variant_dir = output / 'variants'
    variant_dir.mkdir(exist_ok=True)
    for key, texts in PROMPT_VARIANTS.items():
        for i, text in enumerate(texts, start=2):
            jobs.append((variant_dir / f'{key}_{i}.wav', text))
    for path, prompt in jobs:
        chunks.clear()
        data = prompt.encode('utf-8') + b'\0'
        if lib.espeak_Synth(data, len(data), 0, 1, 0, 1, None, None) != 0:
            raise RuntimeError(f'synthesis failed: {path.stem}')
        lib.espeak_Synchronize()
        with wave.open(str(path), 'wb') as wav:
            wav.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
            wav.writeframes(b''.join(chunks))
        print(path.stem, len(b''.join(chunks)))
    lib.espeak_Terminate()


def normalize(path: Path) -> None:
    """响度归一化 + 峰值限制。有 ffmpeg 就用它，没有就退化成峰值归一化。"""
    if shutil.which('ffmpeg'):
        with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tf:
            tmp = Path(tf.name)
        try:
            # alimiter 的 level=false：默认 level=true 会把限幅后的信号拉回
            # 满刻度，等于白限幅（峰值照样 100%）。0.89 → 峰值上限 -1dBFS。
            af = ('acompressor=threshold=0.125:ratio=1.6:attack=8:release=120,'
                  'loudnorm=I=-14:TP=-1.0:LRA=11,alimiter=limit=0.89:level=false')
            subprocess.run(
                ['ffmpeg', '-y', '-v', 'error', '-i', str(path), '-af', af,
                 '-ar', '24000', '-ac', '1', '-c:a', 'pcm_s16le', str(tmp)],
                check=True)
            shutil.move(str(tmp), str(path))
            return
        except (subprocess.CalledProcessError, OSError) as exc:
            print(f'  ffmpeg 归一化失败({exc})，退回峰值归一化', file=sys.stderr)
        finally:
            tmp.unlink(missing_ok=True)

    with wave.open(str(path), 'rb') as w:
        params = w.getparams()
        raw = w.readframes(w.getnframes())
    if params.sampwidth != 2:
        return
    vals = struct.unpack('<%dh' % (len(raw) // 2), raw)
    peak = max((abs(v) for v in vals), default=0)
    rms = math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0
    if peak == 0:
        return
    # 峰值留 1% 余量，最多 4 倍（+12dB），避免把噪声放大到爆音
    gain = min(32767 * 0.99 / peak, 4.0)
    out = struct.pack('<%dh' % len(vals),
                      *(max(-32767, min(32767, int(v * gain))) for v in vals))
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(params.nchannels)
        w.setsampwidth(params.sampwidth)
        w.setframerate(params.framerate)
        w.writeframes(out)
    print(f'  峰值归一化 x{gain:.2f}（RMS {rms/327.67:.1f}% → '
          f'{min(rms*gain, 32767)/327.67:.1f}%）')


if __name__ == '__main__':
    main()
    voice_dir = Path(__file__).resolve().parent.parent / 'assets' / 'voice'
    for wav in sorted(voice_dir.glob('*.wav')) + sorted((voice_dir / 'variants').glob('*.wav')):
        normalize(wav)
