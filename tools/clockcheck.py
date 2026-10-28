#!/usr/bin/env python3
"""Measure the clock pulses in a mono + clock render.

    clockcheck.py OUT.wav right|left

Prints the pulse count, mean gap and first pulse's width on the clock side, how many clock-side samples are
neither silence nor a pulse, and the peak on the sound side."""
import struct
import sys
import wave

w = wave.open(sys.argv[1], "rb")
side = 1 if sys.argv[2] == "right" else 0
rate, n = w.getframerate(), w.getnframes()
data = struct.unpack("<%dh" % (n * 2), w.readframes(n))
clk, snd = data[side::2], data[1 - side::2]
edges, prev, width, other, gaps, last = 0, False, 0, 0, 0, None
for i, c in enumerate(clk):
    hi = c > 16000
    other += c not in (0, 31129)  # 0.95 of full scale
    if hi and not prev:
        if last is not None:
            gaps += i - last
        last, edges = i, edges + 1
    if hi and edges == 1:
        width += 1
    prev = hi
print("clock out on the %s: %d pulses, mean gap %.2f ms, width %.2f ms, other values %d, sound peak %.2f"
      % (sys.argv[2], edges, gaps / max(1, edges - 1) * 1000 / rate, width * 1000 / rate, other,
         max(abs(s) for s in snd) / 32768))
