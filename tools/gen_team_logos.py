# Generates src/sports/nhl/team_logos.h from logos/png/*.png — 64x64
# RGB565 PROGMEM bitmaps with 0x1909 as the transparent sentinel (the
# convention nhl_logos.cpp's draw helpers expect). Source PNGs come from
# tools/gen_team_logo_pngs.py (NHL SVGs -> 64x64 RGBA transparent).

import glob
import os

from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "src", "sports", "nlh" if False else "nhl",
                   "team_logos.h")

def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

lines = ["#pragma once", "#include <Arduino.h>",
         "// Auto-generated 64x64 RGB565 NHL team logos "
         "(0x1909 = transparent). Source: logos/png via tools/gen_team_logos.py",
         ""]
count = 0
for path in sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "logos", "png", "*.png"))):
    abbrev = os.path.basename(path)[:-4]
    im = Image.open(path).convert("RGBA").resize((64, 64), Image.LANCZOS)
    lines.append("const uint16_t LOGO_64_%s[4096] PROGMEM = {" % abbrev)
    px = im.load()
    for y in range(64):
        row = []
        for x in range(64):
            r, g, b, a = px[x, y]
            row.append("0x1909" if a < 128 else "0x%04X" % to_rgb565(r, g, b))
        lines.append("  " + ", ".join(row) + ",")
    lines.append("};")
    count += 1
lines.append("")
lines.append("static const int NHL_LOGO_COUNT = %d;" % count)
with open(OUT, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", OUT, "logos:", count)
