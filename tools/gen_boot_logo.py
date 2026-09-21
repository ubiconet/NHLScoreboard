from PIL import Image

src = r"c:\projects\MLBScoreboard\src\sports\mlb\assets\mlbscoreboard.png"
out = r"c:\projects\MLBScoreboard\src\sports\mlb\boot_logo.h"

im = Image.open(src).convert("RGBA")
w, h = im.size
print("size", w, h)

# Composite onto the app's COLOR_BG (0x0821 RGB565 ~= (8,4,8) RGB888) so edges
# blend seamlessly with the splash screen's fillScreen(COLOR_BG) background.
bg = (8, 4, 8, 255)
bg_layer = Image.new("RGBA", im.size, bg)
composited = Image.alpha_composite(bg_layer, im).convert("RGB")

pixels = composited.load()

def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

lines = []
lines.append("#pragma once")
lines.append("#include <Arduino.h>")
lines.append("// Auto-generated from src/mlbscoreboard.png (alpha-composited onto COLOR_BG 0x0821)")
lines.append("static const int BOOT_LOGO_WIDTH = {};".format(w))
lines.append("static const int BOOT_LOGO_HEIGHT = {};".format(h))
lines.append("const uint16_t BOOT_LOGO_MLB[{}] PROGMEM = {{".format(w * h))
for y in range(h):
    row_vals = []
    for x in range(w):
        r, g, b = pixels[x, y]
        row_vals.append("0x{:04X}".format(to_rgb565(r, g, b)))
    lines.append("  " + ", ".join(row_vals) + ",")
lines.append("};")

with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")

print("wrote", out, "bytes~", w * h * 2)
