#!/usr/bin/env python3
"""prosody 扫描：同一句文案 + 同一音色，试 rate/pitch 组合，量客观指标选参数。

目的：用数据挑「语速/音调」而不是靠猜。判据：
  * silence_pct 不能太高（>30% 就变「一句一停顿的播报腔」）
  * F0 cv 15~28%（低于 12% 偏平，高于 35% 偏夸张）
  * 时长 1.8~3.5s（提醒句舒适区）
"""
import asyncio, os, subprocess, tempfile
from pathlib import Path
import edge_tts
from measure import measure

HERE = Path(__file__).resolve().parent
OUT = HERE / 'raw' / 'sweep'
PROXY = os.environ.get('https_proxy') or 'http://127.0.0.1:7897'
TEXT = '诶，头有点低啦，抬一抬～'
VOICE = 'zh-CN-XiaoxiaoNeural'
COMPRESSOR = 'acompressor=threshold=0.125:ratio=1.6:attack=8:release=120'
TRIM = ('silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,'
        'areverse,silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.08,areverse')
LIMITER = 'alimiter=limit=0.89:level=false'
TARGET_LUFS = -14.0

GRID = [(r, p) for r in ('+0%', '-5%', '-10%', '-14%')
               for p in ('+0Hz', '-4Hz', '-8Hz')]


def synth(rate, pitch, dest):
    async def run():
        await edge_tts.Communicate(TEXT, voice=VOICE, rate=rate, pitch=pitch,
                                   proxy=PROXY).save(str(dest))
    asyncio.run(run())


def loudness(src):
    import json, re
    p = subprocess.run(['ffmpeg', '-hide_banner', '-y', '-i', str(src), '-af',
                        f'{COMPRESSOR},{TRIM},loudnorm=I={TARGET_LUFS}:TP=-1.0:LRA=11:print_format=json',
                        '-f', 'null', '-'], capture_output=True, text=True)
    m = re.search(r'\{[^{}]*"input_i"[^{}]*\}', p.stderr, re.S)
    return float(json.loads(m.group(0))['input_i'])


def normalize(src, dest):
    g = TARGET_LUFS - loudness(src)
    subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', str(src), '-af',
                    f'{COMPRESSOR},{TRIM},volume={g:.2f}dB,{LIMITER}',
                    '-ar', '24000', '-ac', '1', '-c:a', 'pcm_s16le', str(dest)], check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f'{"rate":>6s} {"pitch":>7s} {"时长":>6s} {"F0":>7s} {"cv":>6s} {"rng":>6s} '
          f'{"jit":>5s} {"cen":>6s} {"sil":>6s}')
    for rate, pitch in GRID:
        fd, tmp = tempfile.mkstemp(suffix='.mp3'); os.close(fd)
        tmp = Path(tmp)
        tag = f'r{rate.replace("%","").replace("+","p").replace("-","m")}_' \
              f'p{pitch.replace("Hz","").replace("+","p").replace("-","m")}'
        dest = OUT / f'{tag}.wav'
        try:
            synth(rate, pitch, tmp)
            normalize(tmp, dest)
            m = measure(dest)
            print(f'{rate:>6s} {pitch:>7s} {m["seconds"]:5.2f}s {m["f0_med"]:6.1f} '
                  f'{m["f0_cv_pct"]:5.1f}% {m["f0_range_st"]:5.1f} {m["f0_jitter_pct"]:4.1f}% '
                  f'{m["centroid_hz"]:6.0f} {m["silence_pct"]:5.1f}%')
        finally:
            tmp.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
