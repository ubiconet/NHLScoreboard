# NHLScoreboard — backplane PCB design (CNC starting point)

A single-sided carrier board that replaces the point-to-point wiring: the
DevKit-1 plugs on, every module connects by header, the penalty LEDs and
resistors are surface-mounted, and one 5 V feed powers everything. Sized
to sit behind the 3D-printed front panel (model: `NHL Scoreboard.stl`,
220 × 114 mm). Reference drawing: `pcb-layout.svg` (1:1 front view,
regenerate with `tools/gen_pcb.py`).

## Layout read from the 3D model (front view, mm, origin at board center)

| Zone | Position (x, y) | Size | Notes |
|---|---|---|---|
| Board | (0, 0) | 220 × 114 | matches panel |
| Matrix cutout LEFT | (−68, −12) | 32 × 32 | full-module cutout |
| Matrix cutout RIGHT | (68, −12) | 32 × 32 | full-module cutout |
| Matrix PERIOD window | (23, −20.5) | 20 × 20 | below/right of the clock — LED face window, 32×32 module sits behind |
| TM1637 | (0, −45.8) | 31 × 15 | top center |
| DevKit-1 | (0, 6) | 70 × 53 | centered |
| TFT 2.0" | (0, 15) | 48 × 38 | center, below DevKit top zone |
| Mounting holes 4 × Ø5 | (±63.9, 28.5), (±73.4, 28.5) | — | panel standoffs |
| Label text band | y ≈ −34..−55 | — | raised text across top |
| Label under each matrix | y ≈ 16..22 | 31 × 6 | e.g. HOME / GUEST |

The MAX98357 amp needs no panel cutout (audio only) — it lives on the
backplane in the free lower-right area, near the speaker terminal.

## Connectors and parts (all on the back of the PCB, copper milled on the
same single side; modules face the panel through their cutouts)

| Ref | Part | Position | Pins / nets |
|---|---|---|---|
| J1 | 2 × 20-pin female header (DevKit socket) | (0, 6) zone | see pin map |
| J2 | 1 × 7 female header (TFT) | (0, −2) | VCC GND SCL SDA RES DC CS |
| J3 | 1 × 4 (TM1637) | (0, −45.8) | VCC GND CLK DIO |
| J4 | 1 × 5 (matrix 1 = GUEST) | (−68, 8) | VCC GND DIN CLK CS |
| J5 | 1 × 5 (matrix 2 = PERIOD) | (23, −20.5) | behind its 20×20 window |
| J6 | 1 × 5 (matrix 3 = HOME) | (68, 8) | VCC GND DIN CLK CS |
| J7 | 1 × 5 (MAX98357) | (66, 40) | VIN GND BCLK LRC DIN |
| J8 | 1 × 2 (speaker) | (84, 40) | — |
| J9 | 1 × 2 screw terminal 5.08 (5 V in) | (0, 48) | 5V GND |
| D1–D4 | SMD LED 1206 + label side | (−90, −44) (−45, −44) (45, −44) (90, −44) | H1 H2 G1 G2 |
| R1–R4 | **220 kΩ** 0805 | beside each LED | series with LED — see note |
| C1–C6 | 100 nF + 10 µF 0805/1206 per matrix header | at J4–J6 | local decoupling |

> **LED resistor note**: at 3.3 V a 220 kΩ series resistor limits the LED
> to ~15 µA — effectively dark on standard LEDs. If the intent was the
> classic 220 **Ω** (≈13 mA, bright), change the value in
> `tools/gen_pcb.py` and this table. High-efficiency LEDs that genuinely
> run at 220 kΩ exist; verify with a bench test before milling.

## Rough trace plan (drawn in `pcb-layout.svg`)

- **Shared GND rail** (black, 2 mm): a perimeter ring inset 3 mm from the
  board edge, with taps dropping/rising to every connector's GND, the
  LED cathodes, and the DevKit. The ring keeps every return short and
  gives the matrix current a wide path home.
- **Shared 5V rail** (red, 2 mm): from J9 the rail splits left and right
  along the bottom edge, rising to J4 (left matrix) and J6 (right
  matrix), with branches to J7 (amp) and a routed run up the right of
  the DevKit zone to J5 (period matrix) and the DevKit's 5V pin.
- **Signals** (colored, 0.9 mm): the TFT bundle drops straight from J2
  into the DevKit's top row; TM1637 wraps the DevKit's left; the matrix
  CLK/CS bus rides a corridor at y ≈ −32..−35 above the DevKit to reach
  all three matrix headers; GPIO 40's DIN and the two dashed chain
  jumpers (J4→J5→J6) route below/around the DevKit; I2S runs down the
  right side to the amp; the four LED drives fan up to the top band.
- Dashed teal = the DOUT→DIN jumpers, which can be traces OR short
  wires if the milling gets tight.

Positions are a starting point — shift the small parts freely; keep J1's
zone exactly on the DevKit footprint.

## DevKit socket footprint (J1)

The controller measures **56 × 28 mm** (compact S3 dev board — not the
larger DevKitC). Socket: two rows of female header along the 56 mm edges,
pads at 2.54 mm pitch spanning ~48.3 mm, row spacing ~24.1 mm — **measure
your board's row spacing and pad count before drilling** (compact boards
run 19–20 pads per row). Only the used pins below need copper; the rest
of the pads may go unrouted.

### Pin map — only the used pins need copper

| Signal | DevKit pin | Goes to |
|---|---|---|
| 3V3 (two pins) | rail | J2 VCC, J3 VCC |
| 5V | rail | J9-1, J4/J5/J6 VCC, J7 VIN |
| GND (4 pins) | rail | everything (star at J9-2) |
| GPIO 8/3/46/9/10 | TFT | J2 SCL/SDA/RES/DC/CS |
| GPIO 16/17 | TM1637 | J3 CLK/DIO |
| GPIO 40/39/38 | matrices | J4 DIN, CLK bus (39), CS bus (38) |
| GPIO 1/2/42 | I2S | J7 BCLK/LRC/DIN |
| GPIO 4/5/11/12 | LEDs | R1/R2/R3/R4 (H1 H2 G1 G2) |

Matrix daisy-chain ON the PCB: J4 DOUT pad → J5 DIN, J5 DOUT → J6 DIN
(short jumpers on the copper side; label them on the silk).

## Netlist summary

- **5V**: J9-1 → DevKit 5V pin → J4/J5/J6 VCC → J7 VIN.
- **3V3**: DevKit 3V3 pins → J2 VCC, J3 VCC. (No regulator on board —
  the DevKit's supplies both rails.)
- **GND**: J9-2 → DevKit GND → all headers, LED cathodes. Keep one star
  point near J9; the matrix returns are the biggest current path.
- **Signals**: straight point-to-point per the pin map above; only the
  three matrix buses (DIN chain, CLK, CS) route to more than one place.

## CNC fabrication rules (single-sided, isolation routing)

- 1 oz copper, FR-1/FR-4; copper on the BACK (components mounted from the
  back so they face the panel through its cutouts).
- V-bit 30–60°, 0.1–0.2 mm tip; isolation ≥ 0.4 mm (16 mil).
- Signal traces ≥ 0.8 mm; 5 V / GND trunk ≥ 2 mm (three matrices can
  pull ~1 A at full brightness).
- Pads ≥ 1.6 mm annular ring; drill 0.9 mm signal, 1.2 mm power/terminal,
  3.2 mm at the four model hole positions (M3 screws through Ø5 panel
  holes).
- No plane fill needed; a fat GND trace ringing the board keeps returns
  short. If your mill is reliable, a hatched ground pour is nicer but
  optional.
- Export path when ready for KiCad: recreate this layout on F.Cu →
  generate Gerber + drill → FlatCAM (or pcbgcode) for isolation toolpaths.

## Build order

1. Mill + drill, deburr, tin or leave bare copper.
2. SMD first: R1–R4, D1–D4 (check polarity against the front labels),
   C1–C6.
3. Headers: J1 female (DevKit plugs in last), J2–J9.
4. Bench test BEFORE plugging the DevKit: continuity-beep every net in the
   pin map, then power J9 from a current-limited 5 V supply and verify
   5 V on J4/J5/J6/J7 VCC and 0 Ω nowhere it shouldn't be.
5. Plug the DevKit, flash the bench test firmware
   (`HARDWARE_TEST_MODE = true`), verify all displays through the
   connectors, then flash production firmware.

---

## CNC fabrication package (fab/) — generated by `tools/gen_pcb_fab.py`

Single source of truth = PADS/TRACES/JUMPERS tables in the script. It emits
into `fab/`: `copper-bottom.gbr` (all copper), `board-outline.gbr`,
`drill.drl` (Excellon: T1 0.9mm signal, T2 3.2mm LED corner holes), and
`preview.svg`. A built-in checker reports cross-net trace crossings —
**current status: 0**. Load the two Gerbers + drill into FlatCAM/bcnc to
generate isolation-milling G-code (0.8mm signal / 1.5mm power / 2mm GND ring,
>=0.3mm isolation; cut on the BOTTOM side as drawn — mirror in your CAM if
you prefer viewing from the top).

Frame = panel SVG coords exactly: x ±110, y −66 (top) .. +48 (bottom).
Component pin orders verified from photos in `docs/components/`
(tft.jpg, scorematrix.jpg, periodmatrix.jpg, audioamp.jpg):
TFT GND-VCC-SCL-SDA-RST-DC-CS · matrices VCC-GND-CS-CLK-DIN(-DOUT) ·
amp LRC-BCLK-DIN-GAIN-SD-GND-Vin · TM1637 CLK-DIO-VCC-GND.

**Penalty LEDs are 5mm THT centered in the four panel round holes**
(±63.9/±73.4, y 28.5) with 220k SMD-1206 series resistors inline (R1–R4).
J6 (home matrix) is plugged rotated 180° so VCC is the bottom pin.

### 13 assembly wires (dashed yellow in preview.svg)
The MAX7219 chain also needs module-to-module wiring — modules daisy-chain
through their own pass-through headers, so wire J4 DOUT→J5 DIN and
J5 DOUT→J6 DIN with dupont jumpers; CLK/CS/5V/GND tap from J4's spare pins.

1. 5V right feed: (8,41) → (98,−16) [feeds J6 VCC + J7 Vin]
2. 5V J4 feed: (−29.5,−2) → (−74,−16)
3. TFT VCC: J2 VCC pin → DevKit 3V3 pin
4. TFT GND: J2 GND pin → (22.5,−42)
5. TM1637 GND: (22.5,−42) → J3 GND pin
6. TM1637 CLK/DIO: J3 pins → DevKit 16/17 pins (2 wires)
7. Away LEDs ×2, Home LEDs ×2: resistor pads (56/70, ±24) → (−28,−19/−21)
8. J4 CS wire: (−92,−15) → J4 CS pin (continue to J5/J6 CS)
9. J4 CLK wire: (−87,−12) → J4 CLK pin

### Firmware pin changes — final routing (applied in v3.44)
- TFT: SCK 13→8, MOSI 12→3, RESET 11→46, CS 9→10, DC 10→9
- Penalty LEDs: GUEST1 6→11, GUEST2 7→12 (home stay 4/5)
- PENALTY_FILL_A 8→13 (GPIO 8 became the TFT SCL — `initCountLeds`
  configures the fill pins, so it can no longer sit there)
- The first routing's proposals (I2S to BCLK=43/LRC=44/DIN=1, home LED
  4↔5 swap) were superseded by the final routing — I2S stays
  BCLK=1, LRC=2, DIN=42; home LEDs stay 4/5.

**Verify against hardware before cutting** (per your note): header
positions/orientations, J6 rotation, LED hole alignment, DevKit placement
(rows x ±24.13, pads y −24.67..28.67).
