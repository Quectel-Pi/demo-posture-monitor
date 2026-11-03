#!/usr/bin/env python3
"""生成中文提示语音（主方案）：edge-tts 神经女声 → 响度/峰值归一化 → 24kHz mono WAV。

════════════════════════════════════════════════════════════════════
两个脚本的关系
════════════════════════════════════════════════════════════════════
  make_voice_assets_neural.py  ← 主方案，本文件
      edge-tts（微软 Edge 在线神经网络 TTS）+ zh-CN-XiaoxiaoNeural 女声。
      需要联网「烘焙」一次，产物是普通 WAV；**运行期完全离线**，
      板端零新依赖（板端仍然只是 paplay 放 WAV）。
      听感自然、有真人语气，替代原先 espeak-ng 的机械音。

  make_voice_assets.py         ← 兜底方案
      libespeak-ng 本机合成，完全无网，但音色是机械音。
      只有在没网 / 装不上 edge-tts 时才用。

两者写的是同一批文件、同一批 key、同一条归一化链路，可以随时互换：

    python3 scripts/make_voice_assets_neural.py   # 有网，女声（推荐）
    python3 scripts/make_voice_assets.py          # 无网，espeak 兜底

════════════════════════════════════════════════════════════════════
「AI 味」的三层，本脚本逐层处理
════════════════════════════════════════════════════════════════════
1. 音色层   选一个本来就口语化的音色 + 慢一点、稳一点的基频。
            实测（evidence/voice-humanlike-2026-11-02/log/sweep-pitch.txt）
            音色 F0 中位数 240Hz（xiaoxiao）最接近成年女声的日常说话区；
            pitch 再往下压到 -20Hz 以下是靠共振峰硬拉，会出现「假低沉」，
            所以只降 4Hz。
2. 韵律层   rate -8%（提醒句舒适区，实测句内停顿率 33~35%，比 -0% 更像
            「边说边想」）；pitch -4Hz（去掉播报腔的高亮尾音）。
3. 文案层   PROMPTS 改成口语（「诶，头有点低啦，抬一抬～」而不是
            「请抬头，保持正确坐姿。」），并给每个 key 备 2 条轮换变体。
            变体需要 UI 侧支持，见 REPORT.md。

════════════════════════════════════════════════════════════════════
为什么必须归一化，为什么是「响度 + 峰值」两级
════════════════════════════════════════════════════════════════════
TTS 直出的音频峰值可能接近满刻度，RMS 却只有 10% 出头，听感比满刻度
正弦低 9dB —— 实测「喇叭明明在响，就是听不清」就是这么来的。
这里沿用 espeak 脚本的 ffmpeg 链路（acompressor → 响度 → alimiter），
把 RMS 提到 ~19-20%（+5dB）。

只做响度匹配还不够：不同句子的波峰因数不同，同一条链跑出来
too_close 峰值 79.4%、其余 89.0%，并排听会显得一条轻一条重。
所以再加一级 MATCH_PEAK：把峰值收敛到 0.89（-1dBFS），偏差 >1.5% 才动手，
最多迭代 4 次。收敛后 6 条（含变体）峰谷差 <2%，RMS 落在 19~22%。

⚠ 注意不要直接用单遍 `loudnorm` 做归一化：
    ffmpeg 4.4 的 loudnorm 对 3 秒以内的短句会算错增益，
    实测把 RMS 从 19% 压到 3.6%（小了 14dB，反而听不见）；
    只有片段长到 ~3s 且 LRA>0 时它才给出正确结果。
    所以这里用「测量 integrated loudness → 换算增益 → limiter」这套
    确定性链路：任何长度、任何来源都得到同一个目标响度。

用法：
    export https_proxy=http://127.0.0.1:7897     # 需要代理翻墙时
    python3 scripts/make_voice_assets_neural.py
    python3 scripts/make_voice_assets_neural.py --proxy http://127.0.0.1:7897
    python3 scripts/make_voice_assets_neural.py --force        # 强制重烤
    python3 scripts/make_voice_assets_neural.py --check        # 只校验现有 wav
    python3 scripts/make_voice_assets_neural.py --variants     # 连变体一起烤

产物：
    assets/voice/{shoulder_tilt,head_down,head_tilt,too_close,hand_on_face,rest}.wav
    assets/voice/variants/<key>_{2,3}.wav      （--variants 时）
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

# ── 音色与韵律（想换音色/语气只改这里）─────────────────────────────
# 试听包见 evidence/voice-humanlike-2026-11-02/tryout/，客观对比见同目录 REPORT.md。
VOICE = 'zh-CN-XiaoxiaoNeural'   # 温柔女声；备选 zh-CN-XiaoyiNeural（更活泼更年轻）
RATE = '-8%'                    # 语速，慢一点更像「提醒」而不是「播报」
PITCH = '-4Hz'                  # 音调，略降去掉播报腔的高亮尾音（压太多会假低沉）
VOLUME = '+0%'                  # 音量交给后面的响度归一化，这里不动
# rest 是一整句复合句（用眼时长 + 远眺 + 时长 + 目的），
# -8% 时语音本身就接近校验上限；长句稍微提一点速到 +0%。
RATE_OVERRIDES = {'rest': '+0%'}

# ── 输出格式 ────────────────────────────────────────────────────
SAMPLE_RATE = 24000             # 神经女声原生 24kHz，不降采样（板端 paplay 直出）
CHANNELS = 1
SAMPLE_WIDTH = 2                # 16bit

# ── 归一化参数（与 make_voice_assets.py 同一套数值）──────────────
TARGET_LUFS = -14.0             # 目标综合响度（第一级，粗调）
TARGET_PEAK = 0.89              # -1dBFS 峰值上限，与旧素材的 100% 削顶拉开距离
TARGET_RMS = 0.20               # 20%：六条之间必须一致（旧版踩过「两条只有 5 倍偏轻」的坑）
PEAK_TOLERANCE = 0.015          # 峰值差超过 1.5% 才再迭代
RMS_TOLERANCE = 0.005           # RMS 差超过 0.5 个百分点才再迭代
PEAK_MAX_ITER = 8
# alimiter 的 level=false 很关键：默认 level=true 会把限幅后的信号
# 又拉回满刻度，等于白限幅（峰值照样 100%，有削波风险）。
LIMITER = f'alimiter=limit={TARGET_PEAK}:level=false'
COMPRESSOR = 'acompressor=threshold=0.125:ratio=1.6:attack=8:release=120'
# 去掉首尾静音：神经 TTS 前后各留 ~0.2-0.4s 空白，白占播放时长
TRIM_SILENCE = (
    'silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,'
    'areverse,'
    'silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.08,'
    'areverse'
)

# ── 文案：主文案与变体都必须与 make_voice_assets.py 完全一致 ─────────
# 直接从兜底脚本导入，避免两处文案漂移。
sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_voice_assets import PROMPTS, PROMPT_VARIANTS  # noqa: E402

VOICE_DIR = Path(__file__).resolve().parent.parent / 'assets' / 'voice'
VARIANT_DIR = VOICE_DIR / 'variants'

# ── 校验阈值 ────────────────────────────────────────────────────
# 常规短句必须落在 1.5~6s。rest 例外放宽到 10s：它是一整句复合句，
# 神经女声 ~6s，espeak 兜底 ~9.4s（机械音语速慢），两者都必须能在
# 同一份 wav 文件名下通过校验。
DURATION_LIMITS = {'rest': (1.5, 10.0)}
DEFAULT_LIMITS = (1.5, 6.0)
MIN_RMS_PCT = 18.0              # 六条之间要一致，下限比只判「听不见」更严
MAX_RMS_PCT = 23.0
MAX_PEAK_PCT = 97.0             # 峰值留 3% 余量，避免削波


def _fail(msg: str, hint: bool = True) -> 'NoReturn':  # noqa: F821
    print(f'\n[ERROR] {msg}', file=sys.stderr)
    if hint:
        print('        无网或装不上 edge-tts 时，请回退到机械音兜底方案：\n'
              '            python3 scripts/make_voice_assets.py', file=sys.stderr)
    raise SystemExit(1)


def _import_edge_tts():
    try:
        import edge_tts
    except ImportError:
        _fail('未安装 edge-tts（pip install edge-tts）。')
    return edge_tts


def resolve_proxy(cli_proxy: str | None) -> str | None:
    """--proxy 优先，其次 https_proxy / HTTPS_PROXY / http_proxy / HTTP_PROXY。"""
    if cli_proxy:
        return cli_proxy
    for var in ('https_proxy', 'HTTPS_PROXY', 'http_proxy', 'HTTP_PROXY'):
        if os.environ.get(var):
            return os.environ[var]
    return None


def synth(edge_tts, text: str, dest: Path, proxy: str | None,
          rate: str = RATE) -> None:
    """合成单条到 mp3。失败抛异常，由调用方转成清晰报错。"""
    import asyncio

    async def run() -> None:
        kwargs = dict(voice=VOICE, rate=rate, pitch=PITCH, volume=VOLUME)
        if proxy:
            kwargs['proxy'] = proxy
        await edge_tts.Communicate(text, **kwargs).save(str(dest))

    asyncio.run(run())


def _ffmpeg() -> str:
    exe = shutil.which('ffmpeg')
    if not exe:
        _fail('找不到 ffmpeg，无法做响度归一化（apt install ffmpeg）。', hint=False)
    return exe


def measure_loudness(src: Path) -> float:
    """回放整条滤镜链（压缩 + 去静音 + 分析），取 integrated loudness (LUFS)。"""
    proc = subprocess.run(
        [_ffmpeg(), '-hide_banner', '-y', '-i', str(src), '-af',
         f'{COMPRESSOR},{TRIM_SILENCE},'
         f'loudnorm=I={TARGET_LUFS}:TP=-1.0:LRA=11:print_format=json',
         '-f', 'null', '-'],
        capture_output=True, text=True)
    match = re.search(r'\{[^{}]*"input_i"[^{}]*\}', proc.stderr, re.S)
    if not match:
        _fail(f'响度测量失败（{src.name}）：\n{proc.stderr.strip()[-400:]}', hint=False)
    data = json.loads(match.group(0))
    level = float(data['input_i'])
    if not math.isfinite(level) or level < -70:
        _fail(f'响度测量异常（{src.name}: input_i={data["input_i"]}）。', hint=False)
    return level


def _mktemp_wav() -> Path:
    fd, name = tempfile.mkstemp(suffix='.wav')
    os.close(fd)
    return Path(name)


def _apply_filter(src: Path, dest: Path, af: str) -> None:
    subprocess.run(
        [_ffmpeg(), '-y', '-v', 'error', '-i', str(src), '-af', af,
         '-ar', str(SAMPLE_RATE), '-ac', str(CHANNELS), '-c:a', 'pcm_s16le',
         str(dest)],
        check=True)


def normalize(src: Path, dest: Path) -> None:
    """压缩 → 去首尾静音 → 响度增益 → limiter → RMS/峰值联合收敛 → 24k mono 16bit。

    两级目标：
      integrated loudness ≈ TARGET_LUFS（粗调，一次性算出增益）
      RMS ≈ TARGET_RMS 且 peak ≤ TARGET_PEAK（细调，循环若干次）

    为什么不能只匹配一个：峰值被 limiter 压在 0.89 之后，RMS 就由波峰因数
    决定，而不同句子的波峰因数天然不同（实测 rest 比 head_tilt 响 2 个百分点，
    并排听就是「一条响一条轻」）。这里每一轮把 RMS 拉向目标、再重新限幅，
    限幅只会削峰不会抬 RMS，因此迭代单调收敛，最多 8 轮。
    """
    gain_db = TARGET_LUFS - measure_loudness(src)
    work = _mktemp_wav()
    nxt = _mktemp_wav()
    try:
        _apply_filter(src, work, f'{COMPRESSOR},{TRIM_SILENCE},'
                                 f'volume={gain_db:.2f}dB,{LIMITER}')
        for _ in range(PEAK_MAX_ITER):
            m = analyze(work)
            peak, rms = m['peak_pct'] / 100.0, m['rms_pct'] / 100.0
            if peak <= 0 or rms <= 0:
                break
            # RMS 偏差优先修正；RMS 已达标时再把峰值往 0.89 推
            if abs(rms - TARGET_RMS) > RMS_TOLERANCE:
                step = 20 * math.log10(TARGET_RMS / rms)
            elif abs(peak - TARGET_PEAK) > PEAK_TOLERANCE:
                step = 20 * math.log10(TARGET_PEAK / peak)
            else:
                break
            # ffmpeg 不允许原地读写，必须换一个临时文件再换回来
            _apply_filter(work, nxt, f'volume={step:+.2f}dB,{LIMITER}')
            work, nxt = nxt, work
        shutil.copyfile(work, dest)
    finally:
        work.unlink(missing_ok=True)
        nxt.unlink(missing_ok=True)


def analyze(path: Path) -> dict:
    """读 PCM 统计格式 / 时长 / 峰值 / RMS / 满刻度削顶样本数。"""
    with wave.open(str(path), 'rb') as w:
        frames, rate = w.getnframes(), w.getframerate()
        channels, width = w.getnchannels(), w.getsampwidth()
        raw = w.readframes(frames)
    vals = struct.unpack('<%dh' % (len(raw) // 2), raw) if raw else ()
    peak = max((abs(v) for v in vals), default=0)
    rms = math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0
    return {
        'seconds': frames / rate if rate else 0.0,
        'rate': rate,
        'channels': channels,
        'bits': width * 8,
        'peak_pct': peak / 32767 * 100,
        'rms_pct': rms / 327.67,
        'clipped': sum(1 for v in vals if abs(v) >= 32767),
    }


def check_all(items: list[tuple[str, Path]]) -> bool:
    print('\n校验结果：')
    print(f'  {"key":18s} {"时长":>7s} {"采样率":>7s} {"声道":>4s} {"位深":>4s} '
          f'{"峰值":>7s} {"RMS":>7s}  状态')
    ok = True
    for key, path in items:
        if not path.is_file() or path.stat().st_size == 0:
            print(f'  {key:18s} —— 缺失或空文件 ✗')
            ok = False
            continue
        m = analyze(path)
        lo, hi = DURATION_LIMITS.get(key.split('_')[0] if key.startswith('variants/') else key,
                                     DEFAULT_LIMITS)
        problems = []
        if (m['rate'], m['channels'], m['bits']) != (SAMPLE_RATE, CHANNELS, 16):
            problems.append('格式')
        if not (lo <= m['seconds'] <= hi):
            problems.append(f'时长不在 {lo}~{hi}s')
        if m['peak_pct'] > MAX_PEAK_PCT or m['clipped']:
            problems.append('削波')
        if m['rms_pct'] < MIN_RMS_PCT:
            problems.append(f'过轻(<{MIN_RMS_PCT}%)')
        if m['rms_pct'] > MAX_RMS_PCT:
            problems.append(f'过响(>{MAX_RMS_PCT}%)')
        status = '✓' if not problems else '✗ ' + ','.join(problems)
        print(f'  {key:18s} {m["seconds"]:6.2f}s {m["rate"]:7d} {m["channels"]:4d} '
              f'{m["bits"]:4d} {m["peak_pct"]:6.1f}% {m["rms_pct"]:6.1f}%  {status}')
        ok = ok and not problems
    return ok


def build_jobs(with_variants: bool) -> list[tuple[str, str, Path, str]]:
    """(校验用 key, 文案, 目标路径, rate) 列表。"""
    jobs = []
    for key, text in PROMPTS.items():
        jobs.append((key, text, VOICE_DIR / f'{key}.wav',
                     RATE_OVERRIDES.get(key, RATE)))
    if with_variants:
        for key, texts in PROMPT_VARIANTS.items():
            for i, text in enumerate(texts, start=2):
                jobs.append((f'variants/{key}_{i}',
                             text, VARIANT_DIR / f'{key}_{i}.wav',
                             RATE_OVERRIDES.get(key, RATE)))
    return jobs


def main() -> int:
    ap = argparse.ArgumentParser(
        description='生成中文提示语音（神经女声，需联网烘焙一次）')
    ap.add_argument('--proxy', help='TTS 请求使用的代理，如 http://127.0.0.1:7897')
    ap.add_argument('--force', action='store_true', help='即使 wav 已存在也重新合成')
    ap.add_argument('--check', action='store_true', help='只校验现有 wav，不合成')
    ap.add_argument('--variants', action='store_true',
                    help='连轮换变体一起烤到 assets/voice/variants/')
    ap.add_argument('--keep-mp3', action='store_true', help='保留中间 mp3（默认删除）')
    args = ap.parse_args()

    VOICE_DIR.mkdir(parents=True, exist_ok=True)
    jobs = build_jobs(args.variants)
    targets = [(key, path) for key, _, path, _ in jobs]

    if args.check:
        return 0 if check_all(targets) else 1

    edge_tts = _import_edge_tts()
    proxy = resolve_proxy(args.proxy)
    print(f'音色 {VOICE}  rate {RATE}  pitch {PITCH}  代理 {proxy or "直连"}')

    keep_dir = VOICE_DIR / '.neural_mp3'
    if args.keep_mp3:
        keep_dir.mkdir(exist_ok=True)

    for key, text, dest, rate in jobs:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.is_file() and not args.force:
            print(f'  {key:18s} 已存在，跳过（--force 可重烤）')
            continue
        if args.keep_mp3:
            mp3 = keep_dir / (key.replace('/', '_') + '.mp3')
            tmp_mp3 = False
        else:
            fd, name = tempfile.mkstemp(suffix='.mp3')
            os.close(fd)
            mp3, tmp_mp3 = Path(name), True
        try:
            print(f'  {key:18s} 合成 “{text}” …')
            try:
                synth(edge_tts, text, mp3, proxy, rate=rate)
            except Exception as exc:  # noqa: BLE001  网络/鉴权/超时都落这里
                _fail(f'{key} 合成失败：{type(exc).__name__}: {exc}')
            if not mp3.is_file() or mp3.stat().st_size == 0:
                _fail(f'{key} 合成产物为空（多半是网络被拦或代理没生效）。')
            normalize(mp3, dest)
        finally:
            if tmp_mp3:
                mp3.unlink(missing_ok=True)

    if not check_all(targets):
        print('\n有文件未通过校验，见上面标记。', file=sys.stderr)
        return 1
    print(f'\n完成：{len(targets)} 条语音 → {VOICE_DIR}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
