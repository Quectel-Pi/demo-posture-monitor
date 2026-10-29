#!/usr/bin/env python3
"""Generate offline Mandarin WAV prompts using host libespeak-ng (no network).

★ 生成完必须做响度归一化。
  espeak 直出的 wav 峰值能到 90%+，但 RMS 只有 ~11%，听感比满刻度正弦低 9dB ——
  实测「喇叭明明在响，就是听不清」就是这么来的。
  这里用 ffmpeg 做 compress + loudnorm + limiter，把 RMS 提到 ~19%（+5dB）；
  没有 ffmpeg 时退化成纯 Python 的峰值归一化（提升有限但不会更差）。
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

PROMPTS = {
    'shoulder_tilt': '请坐正，放平双肩。',
    'head_down': '请抬头，保持正确坐姿。',
    'head_tilt': '请摆正头部，放松颈肩。',
    'too_close': '离屏幕太近了，请稍微坐远一些。',
    'hand_on_face': '请放下手，不要托腮。',
    'rest': '连续用眼时间较长，请看向六米以外，远眺二十秒，让眼睛休息一下。',
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
    for key, prompt in PROMPTS.items():
        chunks.clear()
        data = prompt.encode('utf-8') + b'\0'
        if lib.espeak_Synth(data, len(data), 0, 1, 0, 1, None, None) != 0:
            raise RuntimeError(f'synthesis failed: {key}')
        lib.espeak_Synchronize()
        with wave.open(str(output / (key + '.wav')), 'wb') as wav:
            wav.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
            wav.writeframes(b''.join(chunks))
        print(key, len(b''.join(chunks)))
    lib.espeak_Terminate()

def normalize(path: Path) -> None:
    """响度归一化 + 峰值限制。有 ffmpeg 就用它，没有就退化成峰值归一化。"""
    if shutil.which('ffmpeg'):
        with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tf:
            tmp = Path(tf.name)
        try:
            af = ('acompressor=threshold=0.125:ratio=1.6:attack=8:release=120,'
                  'loudnorm=I=-14:TP=-1.0:LRA=11,alimiter=limit=0.89')
            subprocess.run(
                ['ffmpeg', '-y', '-v', 'error', '-i', str(path), '-af', af,
                 '-ar', '22050', '-ac', '1', str(tmp)],
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
    for wav in sorted(voice_dir.glob('*.wav')):
        normalize(wav)
