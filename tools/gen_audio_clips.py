# Converts WAVs in src/sports/nhl/assets/ into src/sports/nhl/audio_clips.h
# — PROGMEM int16 PCM arrays the audio HAL streams over I2S. Sources are
# downmixed to mono, linearly resampled to 22050 Hz, and peak-normalized.
# The clip's array/symbol names derive from the file name (goal.wav ->
# GOAL_HORN_PCM? no: goal.wav -> GOAL_PCM / GOAL_SAMPLES / GOAL_RATE).

import glob
import os
import struct
import wave

TARGET_RATE = 22050
ASSETS = os.path.join(os.path.dirname(__file__), "..", "src", "sports",
                      "nlh" if False else "nhl", "assets")
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "sports", "nhl",
                   "audio_clips.h")


def load_wav(path):
    with wave.open(path, "rb") as w:
        ch, sw, rate, n = (w.getnchannels(), w.getsampwidth(),
                           w.getframerate(), w.getnframes())
        if sw != 2:
            raise SystemExit("%s: only 16-bit PCM supported (got %d-bit)" %
                             (os.path.basename(path), sw * 8))
        raw = w.readframes(n)
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch > 1:  # downmix
        mono = []
        for i in range(0, len(samples) - ch + 1, ch):
            mono.append(sum(samples[i:i + ch]) // ch)
        samples = mono
    return list(samples), rate


def resample(samples, rate):
    if rate == TARGET_RATE:
        return samples
    out = []
    step = rate / TARGET_RATE
    for i in range(int(len(samples) / step)):
        x = i * step
        i0 = int(x)
        i1 = min(i0 + 1, len(samples) - 1)
        frac = x - i0
        out.append(int(samples[i0] * (1 - frac) + samples[i1] * frac))
    return out


def main():
    wavs = sorted(glob.glob(os.path.join(ASSETS, "*.wav")))
    if not wavs:
        raise SystemExit("no .wav files in src/sports/nhl/assets/")
    lines = ["#pragma once", "#include <Arduino.h>",
             "// Auto-generated 16-bit mono 22050 Hz PCM clips from",
             "// src/sports/nhl/assets/*.wav (tools/gen_audio_clips.py).",
             ""]
    for path in wavs:
        name = os.path.basename(path)[:-4].upper()
        samples, rate = load_wav(path)
        samples = resample(samples, rate)
        peak = max(1, max(abs(s) for s in samples))
        gain = 0.90 * 32767 / peak
        samples = [max(-32768, min(32767, int(s * gain))) for s in samples]
        lines.append("const int16_t %s_PCM[%d] PROGMEM = {" %
                     (name, len(samples)))
        for i in range(0, len(samples), 20):
            lines.append("  " + ", ".join(str(v) for v in
                                          samples[i:i + 20]) + ",")
        lines.append("};")
        lines.append("const size_t %s_SAMPLES = %d;" % (name, len(samples)))
        lines.append("const int %s_RATE = %d;" % (name, TARGET_RATE))
        lines.append("")
        print("%s: %.2fs, %d samples, %d bytes flash" %
              (name, len(samples) / TARGET_RATE, len(samples),
               len(samples) * 2))
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
