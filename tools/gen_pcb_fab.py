#!/usr/bin/env python3
# Generates the FULL single-sided backplane fab package for CNC isolation
# milling: gerbers (copper + outline), Excellon drill, and a preview SVG.
# Single source of truth = PADS + TRACES + JUMPERS below.
# Board frame matches the Tinkercad panel SVG exactly: x in [-110,110],
# y in [-66,48] (y=-66 top edge, y=+48 bottom edge), all values in mm.
#
# Jumpers = insulated wires added after assembly (single-layer routing
# escape hatches); dashed in preview, NOT in copper.
# Wire-to-pin additions at assembly: J5 GND pin -> GND ring, J7 Vin -> 5V.
#
# Component pinouts verified from docs/components/*.jpg photos:
#   TFT 7-pin: GND VCC SCL SDA RST DC CS  (VCC jumpered to DevKit 3V3)
#   Matrix 6-pin: VCC GND CS CLK DIN (DOUT spare)
#   Amp HW-792 7-pin: LRC BCLK DIN GAIN SD GND Vin (+ screw terminal)
#   TM1637 4-pin: CLK DIO VCC GND
# LEDs: 5mm THT centered in the four panel round holes.
# FIRMWARE PIN CHANGES required (see docs/pcb.md): I2S DIN=1, BCLK=43,
# LRC=44; home penalty LED pins swapped (HOME1=5, HOME2=4).

import os

OUT = os.path.join(os.path.dirname(__file__), "..", "fab")
os.makedirs(OUT, exist_ok=True)

W_SIG, W_PWR, W_RING = 0.8, 1.5, 2.0
PAD_R, PAD_DRILL = 1.0, 0.9

P = []
def pad(net, x, y): P.append((net, x, y))

LEFT = ["3V3","3V3","RST","GPIO4","GPIO5","GPIO6","GPIO7","15","GPIO16",
        "GPIO17","18","8","3","46","GPIO9","GPIO10","GPIO11","GPIO12",
        "GPIO13","NC14","5V","GND"]
RIGHT = ["GND","GPIO43","GPIO44","GPIO1","GPIO2","GPIO42","GPIO41","GPIO40",
         "GPIO39","GPIO38","GPIO37","GPIO36","GPIO35","GPIO0","GPIO45",
         "GPIO48","GPIO47","GPIO21","GPIO20","NC19","GND","GND"]
for i, n in enumerate(LEFT):  pad(n, -24.13, -24.67 + 2.54*i)
for i, n in enumerate(RIGHT): pad(n,  24.13, -24.67 + 2.54*i)

for i, n in enumerate(["GND","5VT","GPIO13","GPIO12","GPIO11","GPIO10","GPIO9"]):
    pad(n, -7.62 + 2.54*i, -8)
for i, n in enumerate(["GPIO16","GPIO17","5V","GND"]):
    pad(n, -5.715 + 2.54*i, -45.8)
for i, n in enumerate(["5V","GND","GPIO38","GPIO39","GPIO40","NC"]):
    pad(n, -81, -18.57 + 2.54*i)
for i, n in enumerate(["5V","GND","GPIO38","GPIO39","GPIO40","NC"]):
    pad(n, 30, -14.15 + 2.54*i)
for i, n in enumerate(["NC","GPIO40","GPIO39","GPIO38","GND","5V"]):
    pad(n, 81, -18.57 + 2.54*i)
for i, n in enumerate(["GPIO44","GPIO43","GPIO1","NC","NC","GND","5V"]):
    pad(n, 58.42 + 2.54*i, 36)
pad("5V", -2.54, 42); pad("GND", 2.54, 42)
LEDS = [("H1A","H1K",-74.645,-72.105), ("H2A","H2K",-65.145,-62.605),
        ("G1A","G1K", 62.605, 65.145), ("G2A","G2K", 72.105, 74.645)]
for a,k,x1,x2 in LEDS: pad(a,x1,28.5); pad(k,x2,28.5)
def resistor(nA, nB, x, y1, y2): pad(nA,x,y1); pad(nB,x,y2)
resistor("GPIO4","H1A", -77, 25, 27)
resistor("GPIO5","H2A", -67, 25, 27)
resistor("GPIO6W","G1A", 60, 25, 27)
resistor("GPIO7W","G2A", 70, 24, 26)
pad("5VW1", 8, 41);  pad("5VW2", 98, -16)
pad("GNDT", -7.62, -11)
pad("5VT", -5.08, -11)
pad("5VW3", -29.5, -2)
pad("5VW5", -74, -16)
pad("CSW", -92, -15)
pad("CLKW", -87, -12)
pad("GNDW1", 22.5, -42)
pad("5VW4", 4, -44)
pad("GPIO6W", -20.5, -7);  pad("GPIO6W", 56, 24)
pad("GPIO7W", -22, -8);    pad("GPIO7W", 56, 20)

T = []
def tr(net, w, *pts): T.append((net, w, list(pts)))

# GND perimeter ring + taps
tr("GND", W_RING, (-106,-62),(106,-62),(106,44),(-106,44),(-106,-62))
for x in (-72.105,-62.605,65.145,74.645): tr("GND", W_PWR, (x,28.5),(x,44))
tr("GND", W_PWR, (-81,-16.03),(-80.6,-16.03),(-80.6,44))
tr("GND", W_PWR, (81,-8.41),(81,44))
tr("GND", W_PWR, (71.12,36),(71.12,44))
tr("GND", W_PWR, (2.54,42),(2.54,43.2))

tr("GND", W_SIG, (24.13,-24.67),(22.5,-24.67),(22.5,-42))
tr("GND", W_PWR, (24.13,26.13),(22,26.13),(22,44))
tr("GND", W_PWR, (24.13,28.67),(22,28.67))
# J2 GND stub to jumper pad GNDT
tr("GND", W_SIG, (-7.62,-8),(-7.62,-6))
# 5V: left trunk (J9 -> J4); right side fed by jumper 5VW1->5VW2;
# TM/J5 VCC fed by jumper 5VW3->5VW4 (wire-to-pins for J5 VCC, J7 Vin)
tr("5V", W_PWR, (-2.54,42),(-2.54,39),(-29.5,39),(-29.5,-2))
tr("5V", W_SIG, (-74,-16),(-74,-18.57),(-80.1,-18.57))
tr("5V", W_SIG, (-2.54,42),(8,42),(8,41))
tr("5V", W_PWR, (98,-16),(98,26),(84.5,26),(84.5,-5.87),(82.1,-5.87))
tr("5V", W_SIG, (1.905,-45.8),(1.905,-41),(4,-41),(4,-44))
# TFT signals: interleaved lanes west of the header (deepest target =
# westmost lane); exit horizontals stack above the pin row.
tr("GPIO13",W_SIG,(-2.54,-8),(-2.54,-10),(-11,-10),(-11,23.59),(-25.5,23.59))
tr("GPIO12",W_SIG,(0,-8),(0,-11.5),(-13,-11.5),(-13,21.05),(-25.5,21.05))
tr("GPIO11",W_SIG,(2.54,-8),(2.54,-13),(-15,-13),(-15,18.51),(-25.5,18.51))
tr("GPIO10",W_SIG,(5.08,-8),(5.08,-14.5),(-17.5,-14.5),(-17.5,15.97),(-25.5,15.97))
tr("GPIO9", W_SIG,(7.62,-8),(7.62,-16),(-20,-16),(-20,13.43),(-25.5,13.43))
# TM1637 CLK/DIO: jumper wires pad-to-pad (see JUMPERS)
# Matrix bus: only J4 (chain head) gets copper CLK/DIN; CS via wire.
# J5 + J6 signals wired pin-to-pin from J4 spares at assembly
# (MAX7219 modules daisy-chain through their own pass-through headers).
tr("GPIO40",W_SIG,(24.13,-6.89),(19.9,-6.89),(19.9,-57),(-84,-57),
   (-84,-8.41),(-81.5,-8.41))
tr("GPIO39",W_SIG,(24.13,-4.35),(27,-4.35),(27,-59),(-87,-59),(-87,-12))
tr("GPIO38",W_SIG,(24.13,-1.81),(29,-1.81),(29,-61),(-92,-61),(-92,-15))
# I2S to amp (DIN=1, BCLK=43, LRC=44)
tr("GPIO1", W_SIG,(24.13,-17.05),(22,-17.05))
tr("GPIO43",W_SIG,(24.13,-22.13),(22,-22.13))
tr("GPIO44",W_SIG,(24.13,-19.59),(22,-19.59))
# Home LEDs (pad i4->H2 near via bottom run; pad i3->H1 far; pin swap in fw)
tr("GPIO5", W_SIG,(-24.13,-14.51),(-28,-14.51),(-28,-21))
tr("GPIO4", W_SIG,(-24.13,-17.05),(-28,-17.05),(-28,-19))
tr("H1A",   W_SIG,(-77,27),(-77,28.5),(-74.645,28.5))
# Away LEDs via jumper wires
tr("GPIO6W",W_SIG,(-24.13,-11.97),(-20.5,-11.97),(-20.5,-7))
tr("GPIO7W",W_SIG,(-24.13,-9.43),(-22,-9.43),(-22,-8))
tr("GPIO6W",W_SIG,(56,24),(60,24),(60,25))
tr("G1A",   W_SIG,(60,27),(60,28.5),(62.605,28.5))
tr("GPIO7W",W_SIG,(56,20),(70,20),(70,24))
tr("G2A",   W_SIG,(70,26),(70,28.5),(72.105,28.5))
# 3V3 pads tied (TFT VCC fed by wire from 3V3 pin)
tr("3V3",   W_SIG,(-24.13,-24.67),(-24.13,-22.13))

JUMPERS = [("5V",  (8,41),    (98,-16),   "5V right-side feed (J6, J7)"),
           ("5V",  (-46,-30), (4,-44),    "TM1637/J5 VCC feed"),
           ("GND", (-7.62,-6),(22.5,-42), "TFT GND wire"),
           ("GND", (22.5,-42),(5.715,-45.8), "TM1637 GND wire"),
           ("5V",  (-29.5,-2),(-74,-16),   "J4 VCC feed"),
           ("GPIO38",(-92,-15),(-81,-13.49), "J4 CS wire"),
           ("GPIO39",(-87,-12),(-81,-10.95), "J4 CLK wire"),
           ("GPIO16",(-5.715,-45.8),(-24.13,-4.35), "TM1637 CLK wire"),
           ("GPIO17",(-1.905,-45.8),(-24.13,-1.81), "TM1637 DIO wire"),
           ("GPIO4",(-28,-19),(-77,25), "home-penalty LED 1"),
           ("GPIO5",(-28,-21),(-67,25), "home-penalty LED 2"),
           ("GPIO6W",(-20.5,-7),(56,24),  "away-penalty LED 1"),
           ("GPIO7W",(-22,-8),  (56,20),  "away-penalty LED 2")]
# Wire-to-pin at assembly (no copper): J5 VCC, J7 Vin from 5V wires above.

def seg_int(a,b,c,d):
    def cr(o,p,q): return (q[0]-o[0])*(p[1]-o[1])-(q[1]-o[1])*(p[0]-o[0])
    d1,d2,d3,d4 = cr(c,d,a),cr(c,d,b),cr(a,b,c),cr(a,b,d)
    return ((d1>0)!=(d2>0)) and ((d3>0)!=(d4>0))
bad = []
for i in range(len(T)):
    for j in range(i+1, len(T)):
        n1,_,p1 = T[i]; n2,_,p2 = T[j]
        if n1 == n2: continue
        for k in range(len(p1)-1):
            for l in range(len(p2)-1):
                if seg_int(p1[k],p1[k+1],p2[l],p2[l+1]):
                    bad.append((n1,n2,p1[k],p1[k+1],p2[l],p2[l+1]))
print("cross-net trace crossings:", len(bad))
for b in bad[:15]: print("  X", b[0], b[1], b[2], "->", b[3], "vs", b[4], "->", b[5])

def fmt(v): return int(round(v * 1e6))
def gerber(path, ops):
    aps, aptab = {}, []
    def ap(dia):
        if dia not in aps:
            aps[dia] = 10 + len(aptab); aptab.append((aps[dia], dia))
        return aps[dia]
    with open(path, "w") as f:
        f.write("%FSLAX36Y36*%\n%MOMM*%\n")
        for code, dia in aptab:
            f.write("%ADD%02dC,%.4f*%\n" % (code, dia))
        prev = None
        for net, kind, x, y, dia in ops:
            d = ap(dia)
            f.write("D%02d*\nX%+09dY%+09dD%02d*\n" %
                    (d, fmt(x), fmt(y), 3 if kind == "flash" else 2))
        f.write("M02*\n")
ops = []
for net, w, pts in T:
    ops.append((net, "move", pts[0][0], pts[0][1], w))
    for p in pts[1:]: ops.append((net, "draw", p[0], p[1], w))
for net, x, y in P:
    ops.append((net, "flash", x, y, PAD_R*2))
gerber(os.path.join(OUT, "copper-bottom.gbr"), ops)
gerber(os.path.join(OUT, "board-outline.gbr"),
       [("outline","move",-110,-66,0.1),("outline","draw",110,-66,0.1),
        ("outline","draw",110,48,0.1),("outline","draw",-110,48,0.1),
        ("outline","draw",-110,-66,0.1)])
with open(os.path.join(OUT, "drill.drl"), "w") as f:
    f.write("M48\nMETRIC,TZ\nT1C%.2f\nT2C3.20\n%%\nT1\n" % PAD_DRILL)
    for net, x, y in P: f.write("X%.2fY%.2f\n" % (x, y))
    f.write("T2\nX-63.90Y30.00\nX-73.40Y30.00\nX63.90Y30.00\nX73.40Y30.00\n")
    f.write("T0\nM30\n")

COL = {"GND":"#4a5568","5V":"#c53030","3V3":"#b7791f","5VT":"#c53030"}
def col(n): return COL.get(n, "#2b6cb4" if n.startswith("GPIO") else "#276749")
s = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="-112 -68 224 118" '
     'width="672" height="354">',
     '<rect x="-110" y="-66" width="220" height="114" fill="#0a1a0a"/>',
     '<rect x="-110" y="-66" width="220" height="114" fill="none" '
     'stroke="#9ae6b4" stroke-width="0.6" stroke-dasharray="2 1.5"/>']
for net, w, pts in T:
    d = "M " + " L ".join("%g %g" % p for p in pts)
    s.append('<path d="%s" fill="none" stroke="%s" stroke-width="%g" '
             'stroke-linecap="round" stroke-linejoin="round"/>' % (d, col(net), w))
for net, x, y in P:
    s.append('<circle cx="%g" cy="%g" r="%g" fill="%s"/>'
             % (x, y, PAD_R, col(net)))
    s.append('<circle cx="%g" cy="%g" r="%g" fill="#0a1a0a"/>'
             % (x, y, PAD_DRILL/2))
for net, a, b, label in JUMPERS:
    s.append('<path d="M %g %g L %g %g" stroke="#f6e05e" stroke-width="0.7" '
             'stroke-dasharray="2 1.5" fill="none"/>' % (a[0],a[1],b[0],b[1]))
    s.append('<circle cx="%g" cy="%g" r="1.1" fill="#f6e05e"/>' % a)
    s.append('<circle cx="%g" cy="%g" r="1.1" fill="#f6e05e"/>' % b)
s.append("</svg>")
open(os.path.join(OUT, "preview.svg"), "w").write("\n".join(s))
print("wrote fab/: copper-bottom.gbr, board-outline.gbr, drill.drl, preview.svg")
print("pads:", len(P), " traces:", len(T), " jumpers:", len(JUMPERS))
