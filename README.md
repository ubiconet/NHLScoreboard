# NHLScoreboard

ESP32-S3 firmware for a mini NHL scoreboard — a port of the
[MLBScoreboard](https://github.com/ubiconet/mlb_scoreboard) template (same
hardware and framework; the sport layer now lives at `src/sports/nhl/` under
NHL naming). The data layer still speaks the template's MLB feeds; the NHL
rewrite against the documented NHL web API (`docs/features/nhl-api/`) is
the next step.

Hardware (full topology + pin map: [`docs/hardware.md`](docs/hardware.md);
assembly guide + wiring diagram + tables:
[`docs/assembly.md`](docs/assembly.md)):
2.0" ST7789 TFT (320×240) for game details, 3× MAX7219 8×8 matrices (home
score, guest score, period), a TM1637 4-digit period clock, a MAX98357 I2S
audio amp for the goal horn, and 4 penalty LEDs (2 home, 2 guest). Live
linescore for your preferred teams, upcoming-game countdowns, a league news
ticker, an over-the-air captive-portal setup page, and GitHub-hosted
firmware self-update.

The repo is also a **sport scoreboard template**: the generic framework
(`src/common/` + `src/main.cpp`) is sport-agnostic, and everything NHL
lives in `src/sports/nhl/`. See *Starting a new sport* below.

## Architecture

```
┌─────────────────────────── core 1 (Arduino loop) ──────────────────────┐
│ main.cpp (generic shell)      sports/nhl/nhl_app.cpp + renderers       │
│  boot / OTA screen /          WAITING↔LIVE state machine,             │
│  boot status / NTP sync  ──▶  linescore, carousel, news ticker        │
│                               │ takes POD snapshots                   │
└───────────────────────────────┼────────────────────────────────────────┘
                                │ SnapshotChannel<T> (lock-free, gen ctr)
┌────────────────────────── core 0 (FreeRTOS) ───────────────────────────┐
│ network_service (Wi-Fi AP/portal, P2)   nhl_data_task (feeds, P1)     │
│ ota_update (self-update via TLS)         nhl_client → http_fetch      │
└────────────────────────────────────────────────────────────────────────┘
```

Source layout and conventions: see `AGENTS.md` §1. Build/deploy: `AGENTS.md`
§2. Released binaries + `manifest.json` live in [`releases/`](releases/) —
devices self-update from them after boot.

## Building

```powershell
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run --environment esp32-s3-devkitc-1
```

Hardware notes (pins, the software-SPI gotcha, heap/TLS constraints) are in
`docs/` and `AGENTS.md`. The verified NHL API reference (endpoints, payload
sizes, TLS constraints) lives in
[`docs/features/nhl-api/README.md`](docs/features/nhl-api/README.md).

## Starting a new sport from this template

The framework (`src/main.cpp` + `src/common/`) never names a sport. A sport
is a folder under `src/sports/` that implements the `sport::` contract
(`src/common/app/sport_api.h`) and provides `sport_config.h`. Checklist:

1. **Copy the repo** (new GitHub repo per sport keeps OTA streams separate).
2. **Copy `src/sports/nhl` → `src/sports/<sport>`** and rewrite the sport:
   - `sport_config.h` — pins/panel geometry, branding (AP SSID, hostname,
     portal title), UI theme colors, feed poll cadences.
   - `nhl_teams.*` — the league's team table `{id, abbrev, label}`.
   - `nhl_snapshot.h` — your live-game POD structs (the MLB template's
     carries balls/strikes/outs and base runners; hockey carries period,
     shots, power play…).
   - `nhl_client.*` — feed endpoints + JSON filters (build on
     `common/comms/http_fetcher`; keep payloads small and check the
     league's transport — the NHL API is HTTPS-only, see
     `docs/features/nhl-api/README.md`).
   - `nhl_data_task.cpp` — poll cadence + parse-to-snapshot publishing.
   - `nhl_app.cpp` / renderer files — your screens and state machine.
   - `team_logos.h` / `boot_logo.h` — regenerate assets
     (`tools/gen_boot_logo.py` pattern: RGB565, 0x1909 transparent).
3. **Point the env at it** in `platformio.ini`: change `+<sports/nhl/>`
   to `+<sports/<sport>/>` in `build_src_filter` and `-Isrc/sports/nhl`
   to `-Isrc/sports/<sport>` in `build_flags`, then delete the old folder.
4. **Repo identity**: set `OTA_MANIFEST_URL` / `OTA_LATEST_BIN_URL` in
   `src/config.h`, and `RAW_BASE` + `LATEST_FILE` in
   `tools/release_deploy.py`, to the new repo; bump `FIRMWARE_VERSION` to
   `v1.0`.
5. Build, USB-flash, walk through the setup portal, and run a live-game
   session against your feed.

Notes for the port: NVS keys (`ssid`, `team1..3`, `show_clock`) are shared
by design so a re-purposed board keeps its Wi-Fi; team ids are league-local,
so clear/re-pick teams in the portal after re-flashing a board.
