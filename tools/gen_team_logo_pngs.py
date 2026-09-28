# Converts logos/svg/{ABBREV}.svg -> logos/png/{ABBREV}.png — the 64x64
# RGBA transparent set the team-logo pipeline consumes (same convention as
# the MLB template's logos/png). reportlab's rasterizer cannot produce an
# alpha channel (bg is 24-bit RGB only), so each crest is rendered twice —
# once on pure white, once on pure black — and the true color+alpha are
# solved per pixel (Cwhite - Cblack = (1-a)*255). Rendering is supersampled
# 4x, alpha-cropped to the crest, uniformly fitted (longest side = 64) and
# centered so no logo is stretched. Also regenerates
# logos/contact-sheet.png as an at-a-glance check sheet.

import contextlib
import glob
import io
import os

from PIL import Image
from reportlab.graphics import renderPM
from svglib.svglib import svg2rlg

SIZE = 64
SS = 4  # supersample factor for smooth edges


@contextlib.contextmanager
def muted_stderr():
    # reportlab's curve flattener prints "colinear!" notes for degenerate
    # bezier control points straight to fd 2 — noise, not errors.
    devnull = os.open(os.devnull, os.O_WRONLY)
    saved = os.dup(2)
    os.dup2(devnull, 2)
    try:
        yield
    finally:
        os.dup2(saved, 2)
        os.close(saved)
        os.close(devnull)


def render_on(svg_path, bg):
    drawing = svg2rlg(svg_path)
    w, h = drawing.width, drawing.height
    scale = (SIZE * SS) / max(w, h)
    drawing.scale(scale, scale)
    drawing.width, drawing.height = w * scale, h * scale
    with muted_stderr():
        raw = renderPM.drawToString(drawing, fmt="PNG", bg=bg)
    return Image.open(io.BytesIO(raw)).convert("RGB")


def svg_to_png64(svg_path):
    white, black = render_on(svg_path, 0xFFFFFF), render_on(svg_path, 0x000000)
    cw, ck = white.load(), black.load()
    out = Image.new("RGBA", white.size)
    po = out.load()
    for y in range(out.height):
        for x in range(out.width):
            w, k = cw[x, y], ck[x, y]
            alpha = 255 - max(w[c] - k[c] for c in range(3))
            if alpha <= 0:
                po[x, y] = (0, 0, 0, 0)
            elif alpha == 255:
                po[x, y] = (w[0], w[1], w[2], 255)
            else:
                po[x, y] = tuple(
                    max(0, min(255, round((w[c] - (255 - alpha)) * 255 / alpha)))
                    for c in range(3)) + (alpha,)
    bb = out.split()[3].getbbox()
    if bb is None:
        raise RuntimeError("no visible content: %s" % svg_path)
    im = out.crop(bb)
    im.thumbnail((SIZE, SIZE), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
    return canvas


def main():
    src_dir = os.path.join(os.path.dirname(__file__), "..", "logos")
    svgs = sorted(glob.glob(os.path.join(src_dir, "svg", "*.svg")))
    if not svgs:
        raise SystemExit("no SVGs in logos/svg/ — run the download step first")

    pngs = []
    os.makedirs(os.path.join(src_dir, "png"), exist_ok=True)
    for svg in svgs:
        base = os.path.basename(svg)[:-4]
        abbrev = base.split("_")[0]  # tolerate {ABBREV}_light.svg naming
        out = os.path.join(src_dir, "png", abbrev + ".png")
        png = svg_to_png64(svg)
        png.save(out)
        pngs.append((abbrev, png))
        print(abbrev, png.size, "content %dx%d" % (png.width, png.height))

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
