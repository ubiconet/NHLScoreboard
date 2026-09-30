# Synthesizes the end-of-period buzzer into
# src/sports/nhl/assets/buzzer.wav — a harsh arena klaxon (~300 Hz with
# strong odd harmonics, ~1.8 s, hard start/stop), the classic rink
# period-ending sound. Consumed by tools/gen_audio_clips.py like any
# other clip asset (BUZZER_PCM / BUZZER_SAMPLES / BUZZER_RATE).

import math
import struct
import wave

RATE = 22050
DUR = 1.8
F0 = 300  # fundamental

out = []
n = int(RATE * DUR)
for i in range(n):
    t = i / RATE
    # square-ish buzzer: fundamental + odd harmonics, slight detune growl
    v = (math.sin(2 * math.pi * F0 * t) * 1.00 +
         math.sin(2 * math.pi * 3 * F0 * t) * 0.45 +
         math.sin(2 * math.pi * 5 * F0 * t) * 0.22 +
         math.sin(2 * math.pi * 7 * F0 * t) * 0.10 +
         math.sin(2 * math.pi * (F0 + 6) * t) * 0.15)
    # 10 ms attack so it doesn't click; hard stop at the end
    env = min(1.0, t / 0.010)
    out.append(v * env)

peak = max(abs(s) for s in out) or 1.0
scale = 0.85 * 32767 / peak

import os
dst = os.path.join(os.path.dirname(__file__), "..", "src", "sports", "nhl",
                   "assets", "buzzer.wav")
with wave.open(dst, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(struct.pack("<%dh" % len(out),
                              *(int(s * scale) for s in out)))
print("wrote", dst, "(%.1f s)" % DUR)
