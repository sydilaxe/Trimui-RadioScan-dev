#!/usr/bin/env python3
"""Write a short test .wav (a tone with a pulsing level) in a chosen encoding, for testing RadioScan's file voices.

    gen_wav.py out.wav [rate] [channels] [pcm8|pcm16|pcm24|pcm32|float32|float64] [hz] [seconds]
"""
import math, struct, sys

out = sys.argv[1]
rate = int(sys.argv[2]) if len(sys.argv) > 2 else 44100
ch = int(sys.argv[3]) if len(sys.argv) > 3 else 2
enc = sys.argv[4] if len(sys.argv) > 4 else "pcm16"
hz = float(sys.argv[5]) if len(sys.argv) > 5 else 220.0
secs = float(sys.argv[6]) if len(sys.argv) > 6 else 2.0

bits = {"pcm8": 8, "pcm16": 16, "pcm24": 24, "pcm32": 32, "float32": 32, "float64": 64}[enc]
fmt = 3 if enc.startswith("float") else 1


def sample(v):
    if enc == "pcm8": return struct.pack("<B", int(128 + v * 127))
    if enc == "pcm16": return struct.pack("<h", int(v * 32767))
    if enc == "pcm24": return struct.pack("<i", int(v * 8388607))[:3]
    if enc == "pcm32": return struct.pack("<i", int(v * 2147483647))
    if enc == "float32": return struct.pack("<f", v)
    return struct.pack("<d", v)


data = bytearray()
for i in range(int(rate * secs)):
    t = i / rate
    v = 0.5 * math.sin(2 * math.pi * hz * t) * (0.6 + 0.4 * math.sin(2 * math.pi * 2 * t))
    for c in range(ch):
        data += sample(v if c % 2 == 0 else -v)
if len(data) & 1: data += b"\0"
block = ch * bits // 8
fmt_chunk = struct.pack("<HHIIHH", fmt, ch, rate, rate * block, block, bits)
with open(out, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt_chunk) + 8 + 8 + 4 + 8 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk)
    f.write(b"LIST" + struct.pack("<I", 4) + b"INFO")  # an extra chunk the reader must skip
    f.write(b"data" + struct.pack("<I", len(data)) + data)
