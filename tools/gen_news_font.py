# Generates src/sports/nhl/news_font.h from the Adafruit GFX classic 5x7
# font (glcdfont.c in the PlatformIO libdeps) — the news ticker draws its
# own glyphs with a custom vertical stretch (see drawTickerText in
# nhl_renderer_waiting.cpp), so it needs the font bytes at hand instead of
# the library's integer-only setTextSize scaling.
#
# Re-run after a GFX library major bump (the classic font is stable).

import os
import re

SRC = os.path.join(".pio", "libdeps", "esp32-s3-devkitc-1",
                   "Adafruit GFX Library", "glcdfont.c")
OUT = os.path.join("src", "sports", "nhl", "news_font.h")

with open(SRC, "r", encoding="utf-8", errors="replace") as f:
    body = f.read()

# The table is a run of hex bytes: 0x00, 0x00, ... (256 chars x 5 bytes)
bytes_hex = re.findall(r"0x[0-9A-Fa-f]{2}", body)
if len(bytes_hex) < 256 * 5:
    raise SystemExit("expected >= %d font bytes, found %d" % (256 * 5, len(bytes_hex)))
vals = [int(b, 16) for b in bytes_hex[:256 * 5]]

# Printable ASCII 0x20..0x7E only — the ticker never draws anything else.
FIRST, LAST = 0x20, 0x7E
rows = []
for c in range(FIRST, LAST + 1):
    five = vals[c * 5:(c + 1) * 5]
    rows.append("    {" + ", ".join("0x%02X" % v for v in five) + "},"
                + ("  // %r" % chr(c) if c in (0x20, 0x41, 0x7A, 0x7E) else ""))

header = """#pragma once

#include <Arduino.h>

// Auto-generated from the Adafruit GFX classic 5x7 font by
// tools/gen_news_font.py — printable ASCII only (0x20..0x7E). Column
// format matches glcdfont: 5 bytes per char, bit 0 of each byte is the
// top pixel row. Consumed by the news ticker's stretched glyph renderer.
static const uint8_t NEWS_FONT[%d][5] PROGMEM = {
""" % (LAST - FIRST + 1)

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write(header)
    f.write("\n".join(rows))
    f.write("\n};\n")

print("wrote", OUT, "with", LAST - FIRST + 1, "glyphs")
