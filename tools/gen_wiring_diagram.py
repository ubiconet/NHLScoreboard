# Renders docs/wiring-diagram.png — the NHLScoreboard assembly wiring
# diagram: the DevKit-1 board centered with its true physical header map,
# peripherals boxed left/right, color-coded connections. Geometry mirrors
# docs/hardware.md / docs/assembly.md; regenerate after any pin change.

import os

from PIL import Image, ImageDraw, ImageFont

W, H = 1760, 1360
BG = "white"

C_TFT = "#1976D2"
C_TM = "#E65100"
C_LED = "#9E9D24"
C_MTX = "#388E3C"
C_AUD = "#8E24AA"
C_5V = "#D32F2F"
C_3V3 = "#FFC107"
C_GND = "#424242"

# Confirmed physical header map (docs/hardware.md).
LEFT = ["3V3", "3V3", "RST", "4", "5", "6", "7", "15", "16", "17", "18",
        "8", "3", "46", "9", "10", "11", "12", "13", "14", "5V", "GND"]
RIGHT = ["GND", "43", "44", "1", "2", "42", "41", "40", "39", "38", "37",
         "36", "35", "0", "45", "48", "47", "21", "20", "19", "GND", "GND"]

BOARD = (700, 180, 1080, 1140)     # x0,y0,x1,y1
PIN_Y0, PIN_PITCH = 215, 42


def font(size, bold=False):
    path = "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def pin_y(pos):  # pos: 1-based physical position
    return PIN_Y0 + (pos - 1) * PIN_PITCH


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title = font(30, True)
    f_sub = font(19)
    f_pin = font(15, True)
    f_lbl = font(16, True)
    f_txt = font(15)
    f_sml = font(13)

    d.text((30, 18), "NHLScoreboard — Wiring Diagram (v2.0 pin map)",
           fill="black", font=f_title)
    d.text((30, 58), "ESP32-S3 DevKit-1 (YD family, N8R8)  ·  full steps + "
           "wiring tables: docs/assembly.md  ·  regenerate: tools/gen_wiring_diagram.py",
           fill="#555555", font=f_sub)

    # ---- board ----
    d.rounded_rectangle(BOARD, 12, fill="#1B3A5C", outline="black", width=3)
    d.text(((BOARD[0]+BOARD[2])//2 - 150, BOARD[1] + 14),
           "ESP32-S3 DevKit-1", fill="white", font=f_lbl)
    d.text(((BOARD[0]+BOARD[2])//2 - 210, BOARD[3] - 34),
           "USB-C ports at bottom (use NATIVE USB)", fill="#B9CCE0",
           font=f_sml)
    for i, name in enumerate(LEFT):
        y = pin_y(i + 1)
        d.rectangle((BOARD[0] - 7, y - 6, BOARD[0] + 7, y + 6),
                    fill="#C9A227", outline="black")
        d.text((BOARD[0] + 14, y - 8), name, fill="white", font=f_pin)
    for i, name in enumerate(RIGHT):
        y = pin_y(i + 1)
        d.rectangle((BOARD[2] - 7, y - 6, BOARD[2] + 7, y + 6),
                    fill="#C9A227", outline="black")
        tw = d.textlength(name, font=f_pin)
        d.text((BOARD[2] - 14 - tw, y - 8), name, fill="white", font=f_pin)

    def lpin(pos):  # board left pin point (wire anchor)
        return (BOARD[0] - 8, pin_y(pos))

    def rpin(pos):
        return (BOARD[2] + 8, pin_y(pos))

    def wire(pts, color, width=3):
        d.line(pts, fill=color, width=width, joint="curve")
        x, y = pts[-1]
        d.ellipse((x - 4, y - 4, x + 4, y + 4), fill=color)

    def elbow(a, b, color, width=3):
        (x0, y0), (x1, y1) = a, b
        xm = (x0 + x1) // 2
        wire([(x0, y0), (xm, y0), (xm, y1), (x1, y1)], color, width)

    # ---- LEFT column peripherals ----
    def box(x0, y0, x1, y1, title, fill="#F4F6F8"):
        d.rounded_rectangle((x0, y0, x1, y1), 8, fill=fill, outline="black",
                            width=2)
        d.text((x0 + 12, y0 + 8), title, fill="black", font=f_lbl)

    def right_edge_pins(x1, y0, names, pitch=36):
        pts = {}
        for i, n in enumerate(names):
            y = y0 + 46 + i * pitch
            d.ellipse((x1 - 9, y - 6, x1 + 3, y + 6), fill="#C9A227",
                      outline="black")
            tw = d.textlength(n, font=f_pin)
            d.text((x1 - 16 - tw, y - 8), n, fill="black", font=f_pin)
            pts[n] = (x1 + 3, y)
        return pts

    def left_edge_pins(x0, y0, names, pitch=36):
        pts = {}
        for i, n in enumerate(names):
            y = y0 + 46 + i * pitch
            d.ellipse((x0 - 3, y - 6, x0 + 9, y + 6), fill="#C9A227",
                      outline="black")
            d.text((x0 + 16, y - 8), n, fill="black", font=f_pin)
            pts[n] = (x0 - 3, y)
        return pts

    # TFT (left header pos 15-19 = GPIO 9,10,11,12,13)
    box(120, 210, 560, 420, '2.0" ST7789 TFT 320x240 — game details')
    tft = right_edge_pins(560, 210, ["VCC", "GND", "SCL/SCK", "SDA/MOSI",
                                     "RES", "DC", "CS", "BLK"])
    elbow(lpin(2), tft["VCC"], C_3V3, 4)            # 3V3 (pos 2)
    elbow(lpin(22), tft["GND"], C_GND, 4)           # GND (pos 22)
    elbow(lpin(19), tft["SCL/SCK"], C_TFT)          # GPIO13
    elbow(lpin(18), tft["SDA/MOSI"], C_TFT)         # GPIO12
    elbow(lpin(17), tft["RES"], C_TFT)              # GPIO11
    elbow(lpin(16), tft["DC"], C_TFT)               # GPIO10
    elbow(lpin(15), tft["CS"], C_TFT)               # GPIO9
    d.text((120, 386), "BLK (backlight): leave unconnected = always on",
           fill="#555555", font=f_sml)

    # TM1637 (pos 9-10 = GPIO 16/17)
    box(120, 470, 560, 620, "TM1637 4-digit — period clock / wall clock")
    tm = right_edge_pins(560, 470, ["VCC", "GND", "CLK", "DIO"])
    elbow(lpin(1), tm["VCC"], C_3V3, 4)
    elbow(lpin(22), tm["GND"], C_GND, 4)
    elbow(lpin(9), tm["CLK"], C_TM)                 # GPIO16
    elbow(lpin(10), tm["DIO"], C_TM)                # GPIO17
    d.text((120, 584), "Power from 3V3 (5V makes 3.3V logic marginal)",
           fill="#555555", font=f_sml)

    # Penalty LEDs (pos 4-7 = GPIO 4,5,6,7)
    box(120, 660, 560, 940, "Penalty LEDs — 330-470 \u03A9 each")
    led_gpio = [("H1", 4), ("H2", 5), ("A1", 6), ("A2", 7)]
    for i, (nm, gpio) in enumerate(led_gpio):
        y = 716 + i * 52
        d.text((140, y - 10), f"{nm}  GPIO{gpio}", fill="black", font=f_txt)
        d.line([(285, y), (330, y)], fill=C_LED, width=3)
        d.ellipse((330, y - 9, 348, y + 9), outline=C_LED, width=3)
        d.line([(348, y), (392, y)], fill=C_LED, width=3)
        d.rectangle((392, y - 8, 452, y + 8), outline=C_LED, width=2)
        d.text((400, y - 8), "330R", fill=C_LED, font=f_sml)
        d.line([(452, y), (520, y)], fill=C_GND, width=4)
        elbow(lpin(3 + i + 1), (285, y), C_LED)
    d.line([(520, 716), (520, 872)], fill=C_GND, width=4)
    elbow(lpin(22), (520, 872), C_GND, 4)
    d.text((120, 902), "H = home penalties, A = guest (away); anode toward GPIO",
           fill="#555555", font=f_sml)

    # ---- RIGHT column peripherals ----
    # MAX7219 chain x3 (right header pos 8-10 = GPIO 40,39,38)
    mtx_pins = ["VCC", "GND", "CLK", "CS", "DIN", "DOUT"]
    mtx = []
    for i, nm in enumerate(["1: GUEST score", "2: PERIOD", "3: HOME score"]):
        y0 = 210 + i * 130
        box(1200, y0, 1640, y0 + 112, f"MAX7219 8x8  —  {nm}", fill="#EFF7EF")
        p = left_edge_pins(1200, y0, mtx_pins)
        mtx.append((y0, p))
        elbow(rpin(9), p["CLK"], C_MTX)             # GPIO39 (all modules)
        elbow(rpin(10), p["CS"], C_MTX)             # GPIO38 (all modules)
        elbow(rpin(21), p["VCC"], C_5V, 4)          # 5V
        elbow(rpin(1), p["GND"], C_GND, 4)          # GND (right pos 1)
    elbow(rpin(8), mtx[0][1]["DIN"], C_MTX)         # GPIO40 -> module 1
    # daisy chain: DOUT n -> DIN n+1 (right side of the module stack)
    for i in range(2):
        y0a = mtx[i][0]
        y0b = mtx[i + 1][0]
        ya = y0a + 46 + 5 * 36  # DOUT row of module i
        yb = y0b + 46 + 4 * 36  # DIN row of module i+1
        wire([(1636, ya), (1660, ya), (1660, yb), (1636, yb)], C_MTX, 5)
        d.polygon([(1636, yb), (1648, yb - 6), (1648, yb + 6)], fill=C_MTX)
        d.text((1670, (ya + yb) // 2 - 8), "DOUT>DIN", fill=C_MTX, font=f_sml)
    d.text((1200, 600), "DIN/GPIO40 -> mod1 DIN (GUEST);  mod1 DOUT -> mod2 "
           "DIN (PERIOD);  mod2 DOUT -> mod3 DIN (HOME).\nCLK + CS shared by "
           "all three; 100nF + 10uF per module; keep intensity low.",
           fill="#555555", font=f_sml)

    # MAX98357 (right pos 4-6 = GPIO 1,2,42)
    box(1200, 660, 1640, 830, "MAX98357 I2S amp — goal horn")
    au = left_edge_pins(1200, 660, ["VIN", "GND", "BCLK", "LRC", "DIN"])
    elbow(rpin(21), au["VIN"], C_5V, 4)
    elbow(rpin(1), au["GND"], C_GND, 4)
    elbow(rpin(4), au["BCLK"], C_AUD)               # GPIO1
    elbow(rpin(5), au["LRC"], C_AUD)                # GPIO2
    elbow(rpin(6), au["DIN"], C_AUD)                # GPIO42
    d.text((1200, 800), "SD + GAIN: leave unconnected (active, 9dB). "
           "Speaker 4R/3W or 8R on the screw terminals.",
           fill="#555555", font=f_sml)

    # power note
    box(1200, 880, 1640, 990, "Power", fill="#FDF3F3")
    d.text((1216, 926), "5V supply >= 2A into the board's 5V pin (or USB + "
           "IN-OUT jumper).\n3 x MAX7219 at full brightness can draw ~1A; "
           "firmware keeps intensity low.", fill="black", font=f_txt)

    # legend (top-right, clear of all wiring)
    lx, ly = 1210, 66
    d.rounded_rectangle((lx, ly, 1710, ly + 118), 8, fill="#F4F6F8",
                        outline="black", width=2)
    d.text((lx + 14, ly + 10), "Legend", fill="black", font=f_lbl)
    items = [(C_TFT, "TFT (SPI)"), (C_TM, "TM1637"), (C_LED, "Penalty LEDs"),
             (C_MTX, "MAX7219 chain"), (C_AUD, "I2S audio"),
             (C_5V, "5V"), (C_3V3, "3V3"), (C_GND, "GND")]
    for i, (c, t) in enumerate(items):
        cx = lx + 16 + (i % 2) * 260
        cy = ly + 44 + (i // 2) * 26
        d.line([(cx, cy), (cx + 30, cy)], fill=c, width=4)
        d.text((cx + 38, cy - 9), t, fill="black", font=f_txt)

    out = os.path.join(os.path.dirname(__file__), "..", "docs",
                       "wiring-diagram.png")
    img.save(out)
    print("wrote", out, img.size)


if __name__ == "__main__":
    main()
