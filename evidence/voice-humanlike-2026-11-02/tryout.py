#!/usr/bin/env python3
"""候选音色试听包：每个候选只合成 head_down 一句，走和正式产物完全相同的
归一化链路（acompressor → 去静音 → 测 loudness → 增益 → limiter），
输出 24kHz/mono/16bit 到 tryout/<name>.wav，便于板上 paplay 平听对比。

烘焙期需要联网（代理 http://127.0.0.1:7897），产物本身是纯 WAV。
"""
import asyncio, os, subprocess, sys, tempfile
from pathlib import Path

import edge_tts

HERE = Path(__file__).resolve().parent
OUT = HERE / 'tryout'
PROXY = os.environ.get('https_proxy') or 'http://127.0.0.1:7897'
TEXT = '诶，头有点低啦，抬一抬嘛。'
TARGET_LUFS = -14.0
COMPRESSOR = 'acompressor=threshold=0.125:ratio=1.6:attack=8:release=120'
TRIM = ('silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,'
        'areverse,'
        'silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.08,'
        'areverse')
LIMITER = 'alimiter=limit=0.89:level=false'

# name -> (voice, rate, pitch)
CANDS = {
    'A-xiaoxiao-warm':        ('zh-CN-XiaoxiaoNeural',       '-5%',  '-2Hz'),
    'B-xiaoyi-lively':        ('zh-CN-XiaoyiNeural',         '+0%',  '-3Hz'),
    'C-xiaoxuan-azure':       ('zh-CN-XiaoxuanNeural',       '-3%',  '-2Hz'),
    'D-xiaobei-liaoning':     ('zh-CN-liaoning-XiaobeiNeural', '-8%', '-2Hz'),
    'E-xiaoni-shaanxi':       ('zh-CN-shaanxi-XiaoniNeural', '-8%',  '-2Hz'),
    'F-hsiaoyu-tw':           ('zh-TW-HsiaoYuNeural',        '-5%',  '-2Hz'),
    'G-hsiaochen-tw':         ('zh-TW-HsiaoChenNeural',      '-5%',  '-2Hz'),
}


def synth(voice, rate, pitch, dest):
    async def run():
        await edge_tts.Communicate(TEXT, voice=voice, rate=rate, pitch=pitch,
                                  proxy=PROXY).save(str(dest))
    asyncio.run(run())


def loudness(src):
    import json, math, re
    p = subprocess.run(['ffmpeg', '-hide_banner', '-y', '-i', str(src), '-af',
                        f'{COMPRESSOR},{TRIM},loudnorm=I={TARGET_LUFS}:TP=-1.0:LRA=11:print_format=json',
                        '-f', 'null', '-'], capture_output=True, text=True)
    m = re.search(r'\{[^{}]*"input_i"[^{}]*\}', p.stderr, re.S)
    return float(json.loads(m.group(0))['input_i'])


def normalize(src, dest):
    g = TARGET_LUFS - loudness(src)
    subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', str(src), '-af',
                    f'{COMPRESSOR},{TRIM},volume={g:.2f}dB,{LIMITER}',
                    '-ar', '24000', '-ac', '1', '-c:a', 'pcm_s16le', str(dest)],
                   check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    only = sys.argv[1:]
    for name, (voice, rate, pitch) in CANDS.items():
        if only and name not in only:
            continue
        dest = OUT / f'{name}.wav'
        fd, tmp = tempfile.mkstemp(suffix='.mp3'); os.close(fd)
        tmp = Path(tmp)
        try:
            print(f'{name:24s} {voice:32s} rate={rate:5s} pitch={pitch:6s}', end=' ', flush=True)
            synth(voice, rate, pitch, tmp)
            if tmp.stat().st_size == 0:
                print('EMPTY'); continue
            normalize(tmp, dest)
            print(f'-> {dest.name} {dest.stat().st_size}B')
        except Exception as exc:
            print(f'FAIL {type(exc).__name__}: {exc}')
        finally:
            tmp.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
