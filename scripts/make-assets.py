#!/usr/bin/env python3
"""生成提示音 assets/beep.wav（纯标准库，板端也能跑）。"""
import math
import os
import struct
import sys
import wave

RATE = 22050


def tone(freq, dur, vol=0.35, fade=0.012):
    n = int(RATE * dur)
    nf = max(1, int(RATE * fade))
    out = []
    for i in range(n):
        env = 1.0
        if i < nf:
            env = i / nf
        elif i > n - nf:
            env = max(0.0, (n - i) / nf)
        out.append(vol * env * math.sin(2 * math.pi * freq * i / RATE))
    return out


def main(path=None):
    path = path or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "assets", "beep.wav")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    samples = []
    samples += [0.0] * int(RATE * 0.02)
    samples += tone(880, 0.11)
    samples += [0.0] * int(RATE * 0.05)
    samples += tone(1175, 0.13)
    samples += [0.0] * int(RATE * 0.03)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767))
                               for s in samples))
    print(path, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
