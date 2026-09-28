# Renders docs/pcb-layout.svg — a 1:1 (mm) FRONT-view reference of the
# backplane PCB described in docs/pcb.md: connector zones, parts, mounting
# holes, the DevKit header grid, and a ROUGH trace plan (shared 5V/GND
# rails + point-to-point signal runs). Rough means exactly that: final
# routing happens in KiCad; this plan proves the nets fit single-sided.
# LED series resistors are 220k per spec (see docs/pcb.md note).

import os

W, H = 220, 114  # board mm

C = {
    "board": "#20344e", "zone": "#8FB3D9", "hdr": "#F2C14E",
    "part": "#7ED09B", "led": "#E4572E", "hole": "#FFFFFF",
    "label": "#FFFFFF", "note": "#9FB8D4",
    "v5": "#D43A3A", "gnd": "#111111",
    "tft": "#3D7BD9", "tm": "#E08A00", "mtx": "#2E9E5B",
    "chain": "#17A2B8", "i2s": "#8E5BD9", "ledsig": "#F0A830",
}

# (color, width, dashed, [(x, y), ...]) — rough Manhattan plan.
TRACES = [
    # shared GND rail: perimeter ring + taps
    (C["gnd"], 2, False, [(-107, -54), (107, -54), (107, 51), (-107, 51),
                          (-107, -54)]),
    (C["gnd"], 2, False, [(0, 51), (0, 49)]),          # J9 GND
    (C["gnd"], 2, False, [(-68, 12), (-68, 51)]),      # J4
    (C["gnd"], 2, False, [(68, 12), (68, 51)]),        # J6
    (C["gnd"], 1.2, False, [(23, -24), (23, -54)]),    # J5
    (C["gnd"], 1.2, False, [(6, -6), (6, -54)]),       # J2 TFT
    (C["gnd"], 1.2, False, [(-4, -48), (-4, -54)]),    # J3 TM1637
    (C["gnd"], 1.2, False, [(70, 42), (70, 51)]),      # J7 amp
    (C["gnd"], 1.2, False, [(30, 10), (30, 51)]),      # DevKit GND
    (C["gnd"], 1.2, False, [(-90, -46), (-90, -54)]),  # LED cathodes
    (C["gnd"], 1.2, False, [(-45, -46), (-45, -54)]),
    (C["gnd"], 1.2, False, [(45, -46), (45, -54)]),
    (C["gnd"], 1.2, False, [(90, -46), (90, -54)]),
    # shared 5V rail: J9 splits left/right with branches
    (C["v5"], 2, False, [(-2.6, 48), (-45, 48), (-45, 26), (-68, 26),
                         (-68, 12)]),                   # to J4
    (C["v5"], 2, False, [(2.6, 48), (45, 48), (45, 26), (68, 26),
                         (68, 12)]),                    # to J6
    (C["v5"], 1.6, False, [(45, 44), (62, 42)]),       # J7 VIN
    (C["v5"], 1.6, False, [(45, 30), (33, 30), (33, -14), (25, -16)]),  # J5
    (C["v5"], 1.2, False, [(33, 6), (26, 6)]),         # DevKit 5V
    # TFT bundle: J2 straight down to the DevKit's top row
    (C["tft"], 0.9, False, [(-10, -2.5), (-10, -6)]),
    (C["tft"], 0.9, False, [(-6, -2.5), (-6, -6)]),
    (C["tft"], 0.9, False, [(-2, -2.5), (-2, -6)]),
    (C["tft"], 0.9, False, [(2, -2.5), (2, -6)]),
    (C["tft"], 0.9, False, [(6, -2.5), (6, -6)]),
    (C["tft"], 0.9, False, [(10, -2.5), (10, -6)]),
    (C["tft"], 0.9, False, [(14, -2.5), (14, -6)]),
    # TM1637: two signals down and around to the DevKit left row
    (C["tm"], 0.9, False, [(-6, -48), (-6, -24), (-31, -24), (-31, -6)]),
    (C["tm"], 0.9, False, [(-9, -48), (-9, -21), (-34, -21), (-34, -6)]),
    # matrix CLK/CS bus: right row up, then a y=-32/-35 corridor to all
    (C["mtx"], 0.9, False, [(26, 2), (38, 2), (38, -32)]),
    (C["mtx"], 0.9, False, [(26, -1), (41, -1), (41, -35)]),
    (C["mtx"], 0.9, False, [(38, -32), (27, -32), (27, -24)]),    # J5
    (C["mtx"], 0.9, False, [(41, -35), (78, -35), (78, 6),
                            (72, 6)]),                             # J6
    (C["mtx"], 0.9, False, [(41, -35), (-60, -35), (-60, 6),
                            (-66, 6)]),                            # J4
    # DIN: GPIO40 to J4; chain jumpers J4->J5->J6 (dashed)
    (C["chain"], 0.9, False, [(-26, 16), (-62, 16), (-62, 8),
                              (-66, 8)]),
    (C["chain"], 0.9, True, [(-66, 12), (-66, 24), (20, 24),
                             (20, -16)]),                          # J4->J5
    (C["chain"], 0.9, True, [(27, -18), (58, -18), (58, 8),
                             (62, 8)]),                            # J5->J6
    # I2S to the amp
    (C["i2s"], 0.9, False, [(26, 10), (40, 10), (40, 38), (62, 38)]),
    (C["i2s"], 0.9, False, [(26, 13), (43, 13), (43, 41), (62, 41)]),
    (C["i2s"], 0.9, False, [(26, 16), (46, 16), (46, 44), (62, 44)]),
    # penalty LED drives to the top band
    (C["ledsig"], 0.9, False, [(-26, 0), (-34, 0), (-34, -34),
                               (-45, -38)]),
    (C["ledsig"], 0.9, False, [(-26, 4), (-38, 4), (-38, -34),
                               (-90, -38)]),
    (C["ledsig"], 0.9, False, [(26, 0), (34, 0), (34, -34),
                               (45, -38)]),
    (C["ledsig"], 0.9, False, [(26, 4), (37, 4), (37, -34),
                               (90, -38)]),
]


def svg():
    o = []
    o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" '
             f'height="{H}mm" viewBox="{-W/2} {-H/2} {W} {H}">')
    o.append(f'<rect x="{-W/2}" y="{-H/2}" width="{W}" height="{H}" rx="3" '
             f'fill="{C["board"]}"/>')

    # trace layer (drawn first, under the parts)
    for color, w, dashed, pts in TRACES:
        dash = ' stroke-dasharray="3,1.5"' if dashed else ""
        path = " ".join(f"M {x:g} {y:g} L {nx:g} {ny:g}"
                        for (x, y), (nx, ny) in zip(pts, pts[1:]))
        o.append(f'<path d="{path}" fill="none" stroke="{color}" '
                 f'stroke-width="{w}"{dash} stroke-linejoin="round"/>')

    def zone(x, y, w, h, label, sub=""):
        o.append(f'<rect x="{x-w/2}" y="{y-h/2}" width="{w}" height="{h}" '
                 f'fill="none" stroke="{C["zone"]}" stroke-width="0.4" '
                 f'stroke-dasharray="2,1.2"/>')
        if label:
            o.append(f'<text x="{x}" y="{y - h/2 - 1.5}" font-size="4" '
                     f'fill="{C["zone"]}" text-anchor="middle" '
                     f'font-family="sans-serif">{label}</text>')
        if sub:
            o.append(f'<text x="{x}" y="{y - h/2 + 5}" font-size="3" '
                     f'fill="{C["zone"]}" text-anchor="middle" '
                     f'font-family="sans-serif">{sub}</text>')

    zone(-68, -12, 32, 32, "MATRIX LEFT", "(guest)")
    zone(68, -12, 32, 32, "MATRIX RIGHT", "(home)")
    zone(0, -45.8, 31, 15, "TM1637", "")
    zone(23, -20.5, 20, 20, "PERIOD", "(window)")
    zone(0, 18, 48, 38, "TFT", "")

    # DevKit socket: 56x28 board outline + 2x20 pads
    o.append('<rect x="-28" y="-8" width="56" height="28" rx="1.5" '
             'fill="none" stroke="#F2C14E" stroke-width="0.3" '
             'stroke-dasharray="1.5,1"/>')
    for row in (-6.05, 18.05):
        for i in range(20):
            o.append(f'<circle cx="{(i - 9.5) * 2.54:.2f}" cy="{row:.2f}" '
                     f'r="0.8" fill="{C["hdr"]}" opacity="0.85"/>')

    def hdr(x, y, n, pitch, label):
        w = (n - 1) * pitch + 3
        o.append(f'<rect x="{x-w/2}" y="{y-1.5}" width="{w}" height="3" '
                 f'rx="0.8" fill="{C["hdr"]}" opacity="0.95"/>')
        o.append(f'<text x="{x}" y="{y + 5}" font-size="3.2" '
                 f'fill="{C["label"]}" text-anchor="middle" '
                 f'font-family="sans-serif">{label}</text>')

    hdr(-68, 10, 5, 2.54, "J4 matrix1")
    hdr(23, -20.5, 5, 2.54, "J5 PERIOD")
    hdr(68, 10, 5, 2.54, "J6 matrix3")
    hdr(0, -50, 4, 2.54, "J3 TM1637")
    hdr(0, -4, 7, 2.54, "J2 TFT")
    hdr(66, 40, 5, 2.54, "J7 amp")
    hdr(84, 40, 2, 2.54, "J8 spk")
    hdr(0, 48, 2, 5.08, "J9 5V")

    # penalty LEDs + 220k series resistors on the top band
    for x, name in ((-90, "H1"), (-45, "H2"), (45, "G1"), (90, "G2")):
        o.append(f'<rect x="{x-1}" y="-46" width="2" height="2" '
                 f'fill="{C["led"]}"/>')
        o.append(f'<rect x="{x-1.6}" y="-40.7" width="3.2" height="1.4" '
                 f'fill="{C["part"]}"/>')
        o.append(f'<text x="{x}" y="-35.5" font-size="3" '
                 f'fill="{C["label"]}" text-anchor="middle" '
                 f'font-family="sans-serif">{name} 220k</text>')

    # mounting holes
    for x, y in ((-63.9, 28.5), (-73.4, 28.5), (63.9, 28.5), (73.4, 28.5)):
        o.append(f'<circle cx="{x}" cy="{y}" r="1.6" fill="{C["hole"]}" '
                 f'stroke="#000" stroke-width="0.2"/>')

    # legend
    o.append('<rect x="-106" y="-52" width="48" height="15" rx="2" '
             'fill="#0b1526" opacity="0.85"/>')
    lx, ly = -104, -48
    for color, txt, w in ((C["v5"], "5V rail", 2), (C["gnd"], "GND rail", 2),
                          (C["tft"], "signals", 0.9),
                          (C["chain"], "chain jumper", 0.9)):
        o.append(f'<path d="M {lx} {ly} L {lx+7} {ly}" stroke="{color}" '
                 f'stroke-width="{w}"/>')
        o.append(f'<text x="{lx+9}" y="{ly+1}" font-size="2.6" '
                 f'fill="{C["note"]}" font-family="sans-serif">{txt}</text>')
        ly += 3.4

    o.append(f'<text x="0" y="{-H/2 + 4}" font-size="4" fill="{C["label"]}" '
             f'text-anchor="middle" font-family="sans-serif">'
             f'NHLScoreboard backplane + rough trace plan (docs/pcb.md)'
             f'</text>')
    o.append('</svg>')
    return "\n".join(o)


out = os.path.join(os.path.dirname(__file__), "..", "docs", "pcb-layout.svg")
with open(out, "w", newline="\n") as f:
    f.write(svg())
print("wrote", out)
