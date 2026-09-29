from PIL import Image

src = r"c:\projects\NHLScoreboard\src\sports\nhl\assets\nhlscoreboard.png"
out = r"c:\projects\NHLScoreboard\src\sports\nhl\boot_logo.h"

# The splash draws centered on the 320x240 canvas with FIRMWARE_VERSION at
# y=226; the logo is scaled to fit 320x200 so it never collides with it.
# (The old 208x288 portrait logo predated the landscape canvas and got
# center-cropped top/bottom.)
MAX_W, MAX_H = 320, 200

im = Image.open(src)  # PIL sniffs content, so a mislabeled file still opens
print("source", im.size, im.mode)
if im.format != "PNG":
    # Re-encode the asset as a genuine PNG so the extension tells the truth.
    im.save(src, "PNG")
    print("re-encoded asset to real PNG")
im = im.convert("RGBA")

scale = min(MAX_W / im.width, MAX_H / im.height, 1.0)
if scale < 1.0:
    im = im.resize((round(im.width * scale), round(im.height * scale)),
                   Image.LANCZOS)
w, h = im.size
print("logo", w, h)

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
lines.append("// Auto-generated from src/sports/nhl/assets/nhlscoreboard.png")
lines.append("// (alpha-composited onto COLOR_BG 0x0821, scaled to fit {}x{})".format(MAX_W, MAX_H))
lines.append("static const int BOOT_LOGO_WIDTH = {};".format(w))
lines.append("static const int BOOT_LOGO_HEIGHT = {};".format(h))
lines.append("const uint16_t BOOT_LOGO_NHL[{}] PROGMEM = {{".format(w * h))
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
