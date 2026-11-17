#!/usr/bin/env python3
"""Write a synthetic two-bar drum break (160 BPM, 44.1 kHz, mono 16-bit) for CI tests.

Usage: make_test_break.py OUT.wav
Not shipped with the plugin: it only gives the host simulator something to slice.
Standard library only.
"""
import math
import random
import struct
import sys
import wave

SR = 44100
BPM = 160.0
BEAT = 60.0 / BPM
random.seed(7)
n = int(SR * BEAT * 8)                      # two bars of 4/4
out = [0.0] * n


def add(start_s, samples):
    i = int(start_s * SR)
    for k, v in enumerate(samples):
        if i + k < n:
            out[i + k] += v


def kick():
    return [math.sin(2 * math.pi * (55 + 120 * math.exp(-t / 0.03)) * t) * math.exp(-t / 0.12)
            for t in (k / SR for k in range(int(SR * 0.3)))]


def snare():
    return [(random.uniform(-1, 1) * 0.7 + math.sin(2 * math.pi * 190 * t) * 0.3) * math.exp(-t / 0.09)
            for t in (k / SR for k in range(int(SR * 0.25)))]


def hat():
    prev = 0.0
    res = []
    for k in range(int(SR * 0.05)):
        x = random.uniform(-1, 1)
        res.append((x - prev) * 0.5 * math.exp(-(k / SR) / 0.015))
        prev = x
    return res


for bar in range(2):
    base = bar * 4 * BEAT
    for b in (0.0, 2.5):
        add(base + b * BEAT, kick())
    for b in (1.0, 3.0):
        add(base + b * BEAT, snare())
    for e in range(8):
        add(base + e * 0.5 * BEAT, hat())

peak = max(abs(v) for v in out) or 1.0
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.8 * 32767)) for v in out))
