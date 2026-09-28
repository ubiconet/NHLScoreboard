# Generates src/sports/nhl/assets/nhlscoreboard.png — the boot-splash
# artwork: extruded 3D collegiate (slab-serif) lettering — "NHL" white with
# navy depth, "SCOREBOARD" orange with amber depth, front faces keyed with
# a dark outline — and a hockey puck ABOVE the wordmark, foreshortened
# toward the viewer as if flying out over the logo (open top face, deep
# tilt), with a radial speed burst trailing behind it. Rays render behind
# the letters and the puck never overlaps the type.
# Drawn at 4x and LANCZOS-downscaled; canvas stays transparent where the
# splash background (COLOR_BG) should show through; auto-cropped to the
# artwork so on-screen centering is exact. tools/gen_boot_logo.py then
# converts the PNG to boot_logo.h.

import math
import os
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4                     # supersample factor
W, H = 232, 292           # working canvas (px) — cropped to art at the end

ORANGE = (255, 184, 28)
WHITE = (255, 255, 255)
SILVER = (176, 186, 199)
NAVY_DEPTH = (16, 24, 44)
AMBER_DEPTH = (86, 50, 4)

PUCK_WALL = (13, 16, 23)
PUCK_TOP = (48, 54, 66)
PUCK_SHEEN = (70, 79, 94)
PUCK_SHEEN2 = (96, 107, 125)
PUCK_RIM = (168, 178, 194)
PUCK_OUTLINE = (10, 16, 28)

# Rockwell ExtraBold: chunky slab serif — collegiate look, and the closest
# system approximation of the NHL shield's serifed letterforms.
FONT = "C:/Windows/Fonts/ROCKEB.TTF"

img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)


def sc(v):
    return int(round(v * S))


def tinted(layer, rgb):
    solid = Image.new("RGBA", layer.size, tuple(rgb) + (255,))
    solid.putalpha(layer.split()[3])
    return solid


def glyph_masks(text, size, tracking=0, stroke=1.5):
    """Two white-glyph masks: fat (stroked — used for the depth sides and
    the dark keyline around the front face) and thin (the front face)."""
    f = ImageFont.truetype(FONT, sc(size))
    widths = [f.getlength(c) for c in text]
    total = sum(widths) + sc(tracking) * (len(text) - 1)
    asc, desc = f.getmetrics()
    pad = sc(stroke) + sc(10)
    thin = Image.new("RGBA", (int(total) + 2 * pad, asc + desc + 2 * pad))
    td = ImageDraw.Draw(thin)
    fat = thin.copy()
    fd = ImageDraw.Draw(fat)
    x = pad
    for ch, w in zip(text, widths):
        bb = td.textbbox((0, 0), ch, font=f)
        td.text((x - bb[0], pad - bb[1]), ch, font=f, fill=(255, 255, 255, 255))
        bb = fd.textbbox((0, 0), ch, font=f, stroke_width=sc(stroke))
        fd.text((x - bb[0], pad - bb[1]), ch, font=f, fill=(255, 255, 255, 255),
                stroke_width=sc(stroke), stroke_fill=(255, 255, 255, 255))
        x += w + sc(tracking)
    bb = fat.getbbox()
    return thin.crop(bb), fat.crop(bb)


def extrude(fat, thin, front, back, steps, dx, dy):
    """3D block letters: fat-mask depth slices in a flat dark side color
    offset down-right (viewer slightly below-left), then the dark keyline
    face, then the front face on top. A gradient here smears into a
    'drop shadow' read; flat sides read as solid blocks."""
    dx, dy = sc(dx), sc(dy)
    out = Image.new("RGBA", (fat.width + steps * dx, fat.height + steps * dy))
    for i in range(steps, 0, -1):
        out.alpha_composite(tinted(fat, back), (i * dx, i * dy))
    out.alpha_composite(tinted(fat, back), (0, 0))       # keyline face
    out.alpha_composite(tinted(thin, front), (0, 0))     # front face
    return out


def fit_size(text, target_w, tracking=0, lo=8, hi=160):
    best = lo
    for size in range(hi, lo - 1, -1):
        f = ImageFont.truetype(FONT, sc(size))
        w = sum(f.getlength(c) for c in text) + sc(tracking) * (len(text) - 1)
        if w <= sc(target_w):
            best = size
            break
    return best


# --- wordmark layers ------------------------------------------------------------
nhl_size = fit_size("NHL", 150, tracking=1)
thin, fat = glyph_masks("NHL", nhl_size, tracking=1)
nhl3 = extrude(fat, thin, WHITE, NAVY_DEPTH, 11, 2.0, 2.4)

sb_size = fit_size("SCOREBOARD", nhl3.width / S - 2)
f_sb = ImageFont.truetype(FONT, sc(sb_size))
natural = sum(f_sb.getlength(c) for c in "SCOREBOARD")
sb_track = max((nhl3.width - natural) / (10 - 1) / S, 0)
thin_s, fat_s = glyph_masks("SCOREBOARD", sb_size, tracking=sb_track)
sb3 = extrude(fat_s, thin_s, ORANGE, AMBER_DEPTH, 8, 1.6, 2.0)

# --- geometry: puck floats above the wordmark, flying out at the viewer ---------
rx, ry, wall = 54, 28, 15
pcx, pcy = W / 2, 92
nhl_y = 164                                  # clear gap below the puck
sb_y = nhl_y + nhl3.height / S + 8

# --- radial speed burst (drawn FIRST = behind the letters, never over them) -----
def rline(x1, y1, x2, y2, w, color):
    d.line([sc(x1), sc(y1), sc(x2), sc(y2)], fill=color + (255,), width=sc(w))
    r = sc(w) // 2
    for (xx, yy) in ((x1, y1), (x2, y2)):
        d.ellipse([sc(xx) - r, sc(yy) - r, sc(xx) + r, sc(yy) + r],
                  fill=color + (255,))


# y grows downward (270° = up): rays trail up and out from behind the puck
burst = [(270, 26, 4), (242, 22, 3.5), (298, 22, 3.5), (214, 20, 3),
         (326, 20, 3), (180, 28, 4), (0, 28, 4), (155, 20, 3), (25, 20, 3)]
for i, (deg, ln, w) in enumerate(burst):
    ang = math.radians(deg)
    r0 = rx + 6
    color = ORANGE if i % 2 == 0 else SILVER
    rline(pcx + r0 * math.cos(ang), pcy + r0 * math.sin(ang),
          pcx + (r0 + ln) * math.cos(ang), pcy + (r0 + ln) * math.sin(ang),
          w, color)

img.alpha_composite(nhl3, (int((W * S - nhl3.width) / 2), sc(nhl_y)))
img.alpha_composite(sb3, (int((W * S - sb3.width) / 2), sc(sb_y)))

# --- puck: open round top face (seen from above-front), short deep wall ---------
pw, ph = rx * 2 + 28, ry * 2 + wall + 28
pl = Image.new("RGBA", (sc(pw), sc(ph)))
pd = ImageDraw.Draw(pl)
cx, cy = pw / 2, ry + 10
o = 3  # separation outline
pd.rectangle([sc(cx - rx - o), sc(cy - o), sc(cx + rx + o), sc(cy + wall + o)],
             fill=PUCK_OUTLINE + (255,))
pd.ellipse([sc(cx - rx - o), sc(cy + wall - ry - o), sc(cx + rx + o),
            sc(cy + wall + ry + o)], fill=PUCK_OUTLINE + (255,))
pd.ellipse([sc(cx - rx - o), sc(cy - ry - o), sc(cx + rx + o), sc(cy + ry + o)],
           fill=PUCK_OUTLINE + (255,))
pd.rectangle([sc(cx - rx), sc(cy), sc(cx + rx), sc(cy + wall)],
             fill=PUCK_WALL + (255,))
pd.ellipse([sc(cx - rx), sc(cy + wall - ry), sc(cx + rx), sc(cy + wall + ry)],
           fill=PUCK_WALL + (255,))
pd.ellipse([sc(cx - rx), sc(cy - ry), sc(cx + rx), sc(cy + ry)],
           fill=PUCK_TOP + (255,))
pd.ellipse([sc(cx - rx + 12), sc(cy - ry + 6), sc(cx + rx - 12), sc(cy + ry - 5)],
           fill=PUCK_SHEEN + (255,))
pd.ellipse([sc(cx - rx + 21), sc(cy - ry + 10), sc(cx + rx - 21), sc(cy + ry - 8)],
           fill=PUCK_SHEEN2 + (255,))
pd.arc([sc(cx - rx + 2), sc(cy - ry + 2), sc(cx + rx - 2), sc(cy + ry - 2)],
       150, 340, fill=PUCK_RIM + (255,), width=sc(2.5))
# leading-edge highlight on the wall facing the viewer
pd.line([sc(cx - rx + 5), sc(cy + wall + 2), sc(cx + rx - 5), sc(cy + wall + 2)],
        fill=(122, 132, 148, 255), width=sc(2))
pl = pl.rotate(-14, resample=Image.BICUBIC, expand=True)

# soft shadow in the gap above the letters (depth cue; never covers glyphs)
shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
sd = ImageDraw.Draw(shadow)
sd.ellipse([sc(pcx - rx + 14), sc(nhl_y - 16), sc(pcx + rx + 6), sc(nhl_y - 2)],
           fill=(0, 0, 0, 120))
shadow = shadow.filter(ImageFilter.GaussianBlur(sc(4)))
img.alpha_composite(shadow)

img.alpha_composite(pl, (int(sc(pcx) - pl.width / 2), int(sc(pcy) - pl.height / 2)))

# --- crop to artwork + pad, so on-screen centering is exact ---------------------
final = img.resize((W, H), Image.LANCZOS)
bbox = final.getbbox()
pad = 8
final = final.crop((max(bbox[0] - pad, 0), max(bbox[1] - pad, 0),
                    min(bbox[2] + pad, W), min(bbox[3] + pad, H)))
final.save("src/sports/nhl/assets/nhlscoreboard.png")

preview = Image.alpha_composite(
    Image.new("RGBA", final.size, (8, 4, 8, 255)), final).convert("RGB")
preview_path = os.path.join(tempfile.gettempdir(), "boot_logo_preview_v6.png")
preview.save(preview_path)

print("size", final.size, "| NHL font", nhl_size, "| SB font", sb_size,
      "| sb_track", round(sb_track, 2), "| sb_y", round(sb_y, 1))
