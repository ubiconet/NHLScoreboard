# AGENTS.md — Operating instructions for AI coding assistants

This file describes how AI assistants should work on the **NHLScoreboard**
project (a port of the MLBScoreboard template — the framework is shared,
the sport layer under src/sports/ is being rewritten for the NHL). The goal: keep changes small, verifiable, and consistent with the
existing architecture; produce a build that an OTA-updated device can run
without surprises.

---

## 1. Project summary

- **Target hardware:** ESP32-S3 DevKit-1 (YD-ESP32-S3-family 44-pin
  DevKitC-1 clone), **N8R8** module — 8 MB flash, 8 MB octal PSRAM
  (permanently reserves GPIO 35–37; PSRAM not yet enabled in firmware).
  Build env stays `esp32-s3-devkitc-1`; OTA app slots are 3.375 MB each.
  The logical GPIO numbers are scrambled across the physical headers —
  pin assignments are chosen for physical sequence, see
  `docs/hardware.md`. GPIO 48 drives the onboard RGB LED; 19/20 are the
  native USB port.
- **Peripherals (NHL board topology — physical header map + rationale in
  `docs/hardware.md`; since v2.0 the firmware drives the target pin map
  for matrices/penalty LEDs/TM1637 — remaining: 3rd period matrix, I2S):**
  - 2.0" ST7789 TFT, 320×240, game details — **software-SPI** on pins
    `SCK=8, MOSI=3, CS=10, DC=9, RESET=46` (backplane-PCB rev map since
    v3.44; GPIO 3/46 are strapping pins but work as outputs after boot).
    Never switch to hardware SPI; see the gotcha note in
    `memories/repo/hardware_architecture.md` and in
    `src/common/hal/tft_panel.h`.
  - 3× MAX7219 8×8 matrices, one daisy chain (home score / guest score /
    period) — target `DIN=40, CLK=39, CS=38` (right header pos 8–10).
  - TM1637 4-digit 7-segment period clock — target `CLK=16, DIO=17`
    (left header pos 9–10).
  - MAX98357 I2S audio amp (goal horn) — target `BCLK=1, LRC=2, DIN=42`
    (right header pos 4–6).
  - 4 penalty LEDs (home P1/P2, guest P1/P2) — target GPIO `4, 5, 11, 12`
    (guest pair moved off 6/7 for the PCB rev, v3.44).
- **Firmware:** Arduino-ESP32 framework, PlatformIO build system.
- **Build envs:**
  - `esp32-s3-devkitc-1` → USB CDC upload.
  - `esp32-s3-devkitc-1-ota` → ArduinoOTA Wi-Fi upload.

### Template architecture (v2.59 reorg)

This repo is a **sport scoreboard template**: a generic framework plus one
sport implementation, so new sports (NHL, etc.) are started by copying the
repo and replacing the sport folder.

```
src/
├── main.cpp              # GENERIC app shell: boot sequence, OTA screen,
│                         #   boot status page, NTP sync → sport::tick()
├── config.h              # composition root: FIRMWARE_VERSION (deploy script
│                         #   reads it HERE), timezone, OTA manifest URLs
├── common/               # GENERIC framework — no sport knowledge
│   ├── config.h          # framework defaults (SB_DEBUG gate, boot/network/
│   │                     #   OTA pacing)
│   ├── app/sport_api.h   # THE contract a sport implements (namespace sport)
│   ├── hal/              # tft_panel (panel + canvas + pushes), led_matrix
│   │                     #   (MAX7219), count_leds (7 counter LEDs)
│   ├── comms/            # http_fetcher (keep-alive HTTP + buffered parse),
│   │                     #   network_service (Wi-Fi/portal/NVS, branding +
│   │                     #   team options injected), ota_update
│   ├── data/             # snapshot_channel.h (cross-core mailbox template),
│   │                     #   time_util (NTP/ISO-8601 helpers)
│   └── ui/               # gfx helpers, QR, boot splash/status, OTA screen
└── sports/
    └── nhl/              # THE SPORT — replaced wholesale per new sport
        ├── sport_config.h  # pins, panel geometry, branding, poll cadences,
        │                   #   UI theme (COLOR_*), sport timing constants
        ├── nhl_app.cpp     # WAITING/LIVE state machine + sport:: contract
        ├── nhl_renderer_linescore.cpp / _waiting.cpp / nhl_renderer_internal.h
        ├── nhl_logos.*     # logo RAM cache + draw helpers
        ├── nhl_state.*     # snapshot channels, activeGamePk, schedule cache,
        │                   #   fetch diagnostics
        ├── nhl_teams.*     # ONE team table {id, abbrev, label}
        ├── nhl_client.*    # feed endpoints + JSON filters (uses http_fetch)
        ├── nhl_data_task.cpp # core-0 fetch/publish loop
        ├── nhl_snapshot.h    # POD snapshot structs (the core0→core1 contract)
        └── team_logos.h / boot_logo.h   # generated assets
```

**Sport selection:** each PlatformIO env sets
`build_src_filter = +<main.cpp>, +<common/>, +<sports/nhl/>` and
`-Isrc/sports/nhl`, so `#include "sport_config.h"` resolves to whichever
sport the env selects. Common code NEVER names a sport (the only
common→sport reach is the include-path-resolved `sport_config.h` seam plus
the `sport::` function contract in `common/app/sport_api.h`).

**Starting a new sport:** see the checklist in README.md. Short version —
copy `src/sports/nhl` → `src/sports/<sport>`, rewrite the sport folder,
point the env's src filter + `-I` at it, update `OTA_*` URLs in
`src/config.h` and `RAW_BASE`/`LATEST_FILE` in `tools/release_deploy.py`.

### Runtime architecture (unchanged by the reorg)

- **Core 1 (Arduino `loop()`):** pure display. The generic shell handles
  boot/update/network screens, then calls `sport::tick()`; the sport renders
  from POD snapshots via `take*Snapshot()`. No HTTP on this core.
- **Core 0 (FreeRTOS tasks):** the network task (portal/DNS/ArduinoOTA,
  priority 2) and the NHL data task (`sports/nhl/nhl_data_task.cpp`,
  priority 1) do all API calls, JSON parsing, and snapshot publishing.
- **Carousel/news/news ticker:** cached for **30 minutes**
  (`NHL_NEWS_CACHE_TTL_MS` in `src/sports/nhl/sport_config.h`). News API is
  hit at most once per 30 min, not on every waiting-mode cycle.

---

## 2. Build & deploy workflow

```powershell
# Compile (no upload) — fast iteration on code
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --environment esp32-s3-devkitc-1

# USB upload (reliable; preferred when developing)
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --target upload --environment esp32-s3-devkitc-1

# OTA upload (Wi-Fi; flaky on weak signal — see notes below)
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --target upload --environment esp32-s3-devkitc-1-ota `
    --upload-port 192.168.1.205
```

**OTA caveats** (from past runs):
- Wi-Fi to 192.168.1.205 is variable (~80–600 ms RTT, occasional 50 %
  packet loss). OTA uploads often stall at 4–19 % and may take 3–4 min
  when they do succeed.
- `--timeout=120` is set in `platformio.ini` for the OTA env.
- If OTA stalls repeatedly, **power-cycle the device** (or flash over
  USB) before retrying — leftover OTA state can wedge the bootloader.

### Releases + firmware self-update

- **One-command release:** `pio run -e esp32-s3-devkitc-1 -t deploy`.
  This builds the firmware, writes `releases/`
  (`nhl_scoreboard_latest.bin`, permanent `nhl_scoreboard_<version>.bin`,
  `manifest.json` with `{"version","file","url"}`), then **commits
  `releases/` and pushes to GitHub** — the push is what publishes the
  update. The target refuses to deploy when `FIRMWARE_VERSION` still
  matches the published manifest version (devices only flash strictly
  newer versions), so **bump `FIRMWARE_VERSION` in `src/config.h` before
  every deploy**. GitHub's raw CDN caches the manifest ~5 min after a push.
- **Self-update flow** (`src/common/comms/ota_update.cpp`, verified
  working end-to-end at v3.19 — device pulled a 1.67 MB release from
  GitHub raw and rebooted into it in ~25 s): shortly after the network
  comes online (before any feed fetch — the TLS handshake needs the
  pristine boot heap; on this core 2.0.x the 2x16 KB mbedTLS buffers
  cannot be shrunk, `setBufferSizes` is a 3.x API), the core-0 data
  task fetches the manifest over TLS. If the manifest version is
  strictly newer than `FIRMWARE_VERSION`, manifest and binary download
  over ONE reused TLS session (this AP refuses a second fresh TLS
  handshake moments after the first) while the renderer shows the "do
  not turn off" progress screen (`common/ui/ota_screen.cpp`), then the
  device reboots into the new image. Failures leave the current
  firmware running.
- **Update checks are BOOT-ONLY** (product decision, v3.17): the
  automatic check runs exactly once per power cycle — two attempts in
  the boot window, then never again mid-session (the fragmented
  post-feed heap can't fund the handshake, and the AP refuses later
  fresh TLS connections). To update a running device: power-cycle it,
  or use the portal's "check now". The portal button remains available
  and works best right after a boot.
- **CRITICAL WIRING** — the sport's data task MUST call
  `serviceOtaUpdates(onlineFor)` every loop pass, hold feeds while
  `otaUpdateInProgress()`, and gate the first feed fetch behind
  `otaBootGateReached(onlineFor)` (see `nhlDataTaskLoop`). The NHL port
  rewrote the data task and silently dropped these calls — the updater
  was dead code until v3.19 restored them. When porting to a new sport,
  copy this block verbatim from the NHL data task.
- **Testing an OTA release** (the procedure that works): (1) deploy the
  release (vN) to GitHub; (2) build a DEBUG firmware labeled vN-1
  (`-DSB_DEBUG=1`, `-DARDUINO_USB_CDC_ON_BOOT=0` so logs reach the
  CH343 UART) and flash it over serial; (3) wait out the ~5 min raw-CDN
  manifest cache; (4) reset the board via the CH343 auto-reset lines
  (RTS high 120 ms → release) and capture serial: expect
  `[OTA] manifest -> HTTP 200`, `update available`, `flashed N bytes
  OK`, then a second ROM banner as the device reboots into the release.
  The device's portal `/ota/status` shows `checked/ok` and the splash
  shows the new version.

---

## 3. Build-versioning rules — **MANDATORY on every code change**

`src/config.h` (the composition root) carries a single human-readable
version string:

```cpp
static const char* FIRMWARE_VERSION = "v1.0";
```

This string is drawn on the boot splash (see `renderBootSplash()` in
`src/common/ui/boot_splash.cpp`) so the user can confirm at a glance which
firmware is on the device. `tools/release_deploy.py` reads it from
`src/config.h` — keep the definition in that file.

**Every change that ships to the device must bump `FIRMWARE_VERSION`.**

### Bump policy

- Increment the **minor** number with each build (i.e. the digit after
  the dot) — the "patch level" in semver terms. Example: `v2.58` →
  `v2.59` → `v2.60` → …
- Don't bump the major (`v2` → `v3`) unless the change is a breaking
  rewrite of the user-facing behavior (e.g. switching scoreboard modes,
  removing the OLED header, swapping in a totally different display
  controller, etc.). Ask the user before doing a major bump.
- Reset the minor back to `0` when the major goes up.
- The version is purely a string — keep it short, ASCII, and visible in
  the boot splash. Do not add build dates or commit SHAs to the string;
  the firmware compile date is already available via `__DATE__`.

### When to bump

| Change | Bump? |
|---|---|
| Bug fix that doesn't change user-visible behavior | **yes**, minor++ |
| New diagnostic info on screen | yes, minor++ |
| Tweaks to internal data flow / dirty-rect / caching | yes, minor++ |
| Comment-only / formatting / refactor with no behavior change | **yes**, minor++ (still ships, still counts as a build) |
| User-facing feature change (new carousel page, etc.) | yes, minor++ |
| Breaking change to display layout, modes, or APIs | ask first; major++ |

### Example edit

```diff
- static const char* FIRMWARE_VERSION = "v1.0";
+ static const char* FIRMWARE_VERSION = "v1.1";
```

The boot splash will pick this up automatically (it reads the constant on
every `renderBootSplash()` call).

---

## 4. Coding conventions

- **No new heap allocations in `loop()`/`sport::tick()`.** The render
  canvas is lazily `new`'d once inside `TftPanel` and cached. Don't add
  `String`/JSON docs/etc. on the render path.
- **Feed/API calls live in core 0** (`sports/nhl/nhl_data_task.cpp` +
  `nhl_client.cpp`). Do not fetch from core 1. If you need new NHL data,
  add a field to a struct in `sports/nhl/nhl_snapshot.h`, publish through
  the channels in `sports/nhl/nhl_state.cpp` (or
  `common/data/snapshot_channel.h` for a new channel), and read it via a
  `take*Snapshot()` accessor.
- **Common code must stay sport-agnostic.** `src/common/` and
  `src/main.cpp` never include a sport header other than the
  include-path-resolved `sport_config.h` (constants only) and never call
  anything outside the `sport::` contract in `common/app/sport_api.h`.
  Sport logic belongs in `src/sports/<sport>/`.
- **Serial.printf is expensive** at 115200 baud (~25 ms per call). Use
  `DBG_PRINTF(fmt, ...)` (defined in `src/common/config.h`) instead — it
  compiles to a no-op when `SB_DEBUG == 0`. Production builds default
  `SB_DEBUG` to 0; leave it at 1 temporarily only while diagnosing an
  issue.
- **No `Serial.println` in hot paths** (`loop()`/`sport::tick()`,
  `rotateCarousel()`, `updateAtBatResultDisplay()`). All status prints go
  through `DBG_PRINTF` and only fire on state transitions.
- **Renderer internals stay inside the sport's renderer files.** The data
  task publishes through the public APIs (`nhl_renderer.h`,
  `nhl_state.h`). `nhl_renderer_internal.h` is renderer-private — never
  include it from outside the renderer pair.
- **All drawing goes through the shared canvas in `TftPanel`** and one
  push call (`pushFull` / `pushBand` / `pushRows`). The only sanctioned
  direct-panel draw is the AP provisioning screen
  (`network_service.cpp`, via `tftPanel.raw()`).
- **Pin assignments live in the sport's `sport_config.h`** and are passed
  to the HAL at init (`initLedMatrix(din, clk, cs)`,
  `initCountLeds(pins)`, `tftPanel.begin(...)`). `src/common/` contains
  no pin constants. Don't change pins without verifying against the
  actual hardware — the SPI pin gotcha in
  `memories/repo/hardware_architecture.md` explains why we cannot use
  hardware SPI for the ST7789 even though it would be faster.

---

## 5. Testing before claiming a fix works

After any non-trivial change:

1. `pio run --environment esp32-s3-devkitc-1` — must compile with no
   errors. Warnings about unused variables/functions are okay but should
   be addressed before bumping the firmware version. Also sanity-check
   flash size against the 1.5 MB OTA partition (the build prints the
   percentage; the MLB template's v2.60 sat ~91.2%).
2. Upload to the device (OTA or USB).
3. Confirm via Serial Monitor (with `-DSB_DEBUG=1`) or the on-screen
   diagnostics that:
   - The boot splash shows the new version string.
   - The waiting screen, when no preferred team is live, shows the
     diagnostic line `sched:NB Ndates Ngames Ns ago` on the "no upcoming
     games" page — this confirms the data task is publishing schedule
     JSON.
   - The MAX7219 matrices still show the live time when the device is
     in waiting mode (hour on home, minute on away).
   - Count LEDs (balls/strikes/outs) update during a live game.

---

## 6. Things to avoid (lessons learned)

- **Don't switch to hardware SPI for the ST7789.** The 5-arg software
  SPI constructor is the proven-working configuration. See
  `src/common/hal/tft_panel.h` and
  `memories/repo/hardware_architecture.md` for the full story.
- **Don't block in `loop()`/`sport::tick()`.** Every API call belongs on
  core 0.
- **Don't keep large `JsonDocument`s on the render core.** The renderer
  only carries the small schedule cache in `nhl_state.cpp` (used by
  `findNextGame()`) — keep it that way.
- **Don't remove the dirty-rect tracking in `renderLinescore()`.**
  Without it the live screen flickers visibly on every 5 s linescore
  poll.
- **Don't change the news cache TTL below 5 minutes.** The ESPN endpoint
  is fine with 30 min; sub-5-min hammering is wasteful and may get
  rate-limited.
- **Don't open a second TLS connection while one is alive.** The OTA
  updater reuses one session for manifest + binary on purpose; the feed
  client closes its keep-alive session (`http_fetch::closeSession()`)
  before anything else connects.
- **Don't bump `FIRMWARE_VERSION` past `v9.x` without a discussion.**
  Two-digit minors look weird in the splash. Plan a major bump before
  then.
- **Don't add pin constants or sport names to `src/common/`.** That
  breaks the template separation the v2.59 reorg established.

---

## 7. Repo memory

Long-lived project notes live under `/memories/repo/` in the assistant's
memory. The current file `hardware_architecture.md` covers:

- Pin assignments
- NHL API integration details → the verified endpoint reference lives in
  `docs/features/nhl-api/README.md` (HTTPS-only transport, payload
  budgets, gameState semantics)
- The ST7789 SPI pin gotcha (DO NOT switch to hardware SPI)
- Performance notes (debug-flag, dirty-rect, sprite pre-rasterization,
  logo cache)
- The v2.59 template reorganization (folder layout, sport_api contract,
  include-path sport selection)

When you discover something durable about this project (a build quirk,
a library version pin, a hardware pitfall), **update
`hardware_architecture.md`** so the next assistant doesn't repeat the
investigation.
