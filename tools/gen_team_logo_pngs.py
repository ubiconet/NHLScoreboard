# Converts logos/svg/{ABBREV}.svg -> logos/png/{ABBREV}.png — the 64x64
# RGBA transparent set the team-logo pipeline consumes (same convention as
# the MLB template's logos/png).
#
# Rasterizer: tools/bin/resvg.exe (vendored, v0.47.0). The original
# svglib+reportlab rasterizer smeared the right side of many crests
# (dragged-pixel artifacts on ~19 of the 32 logos; the SVGs were always
# fine). resvg renders with native alpha at 4x zoom, then the crest is
# alpha-cropped, uniformly fitted (longest side = 64) and centered so no
# logo is stretched. Also regenerates logos/contact-sheet.png as an
# at-a-glance check sheet.

import glob
import os
import subprocess

from PIL import Image

SIZE = 64
ZOOM = 4  # supersample factor for smooth edges
RESVG = os.path.join(os.path.dirname(__file__), "bin", "resvg.exe")


def svg_to_png64(svg_path, tmp_dir):
    raw = os.path.join(tmp_dir, "raw.png")
    subprocess.run([RESVG, "--zoom", str(ZOOM), svg_path, raw],
                   check=True, capture_output=True)
    im = Image.open(raw).convert("RGBA")
    bb = im.split()[3].getbbox()
    if bb is None:
        raise RuntimeError("no visible content: %s" % svg_path)
    im = im.crop(bb)
    im.thumbnail((SIZE, SIZE), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
    return canvas


def main():
    src_dir = os.path.join(os.path.dirname(__file__), "..", "logos")
    svgs = sorted(glob.glob(os.path.join(src_dir, "svg", "*.svg")))
    if not svgs:
        raise SystemExit("no SVGs in logos/svg/ — run the download step first")
    if not os.path.exists(RESVG):
        raise SystemExit("missing %s — re-vendor the resvg binary "
                         "(see the header comment)" % RESVG)

    import tempfile
    pngs = []
    os.makedirs(os.path.join(src_dir, "png"), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for svg in svgs:
            base = os.path.basename(svg)[:-4]
            abbrev = base.split("_")[0]  # tolerate {ABBREV}_light.svg naming
            out = os.path.join(src_dir, "png", abbrev + ".png")
            png = svg_to_png64(svg, tmp)
            png.save(out)
            pngs.append((abbrev, png))
            print(abbrev, png.size)

    # Contact sheet: 8 per row on the dark splash background.
    cols = 8
    cell, pad = 72, 8
    rows = (len(pngs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell + pad, rows * cell + pad),
                      (8, 4, 8, 255))
    for i, (abbrev, png) in enumerate(pngs):
        x = pad + (i % cols) * cell + (cell - SIZE) // 2
        y = pad + (i // cols) * cell + (cell - SIZE) // 2
        sheet.paste(png, (x, y), png)
    sheet.convert("RGB").save(os.path.join(src_dir, "contact-sheet.png"))
    print("wrote %d logos + contact-sheet.png" % len(pngs))


if __name__ == "__main__":
    main()
