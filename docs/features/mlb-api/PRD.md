# PRD — ESP32 Live MLB Scoreboard

- **Status**: Draft
- **Last updated**: 2026-09-07
- **Owner**: MLBScoreboard team

## 1. Problem statement

Baseball fans want a physical mini scoreboard for their desk or wall that
renders live MLB game information in real time without needing a phone or PC.
This device uses an ESP32-S3 microcontroller to poll the public MLB Stats API,
driving a 2.0-inch ST7789 TFT display for logos and game details, two 8x8
MAX7219 LED matrices for home/away scores, and 7 discrete LEDs for the count
(balls/strikes/outs).

## 2. Goals

1. **Hardware Scoreboard**: Drive ST7789 TFT (320x240), 2x MAX7219 8x8 LED
   matrices (scores), and 7 discrete LEDs (3 Balls, 2 Strikes, 2 Outs).
2. **Web Configuration Portal**: ESP32 web interface allowing users to connect
   to WiFi and select their favorite teams in priority order.
3. **Multi-Team Priority Tracking**: Track live games for user-selected teams.
   Primary team gets priority; if secondary teams are playing, cycle
   periodic updates.
4. **Live Scoreboard Updates**: Refresh score, inning, outs, and count every
   ~5 seconds during live games using the lightweight `/linescore` endpoint.
5. **Dynamic TFT Display**: Keep logos and inning constant while carousel
   rotating game stats (current batter, pitcher, last play, LOB/RISP).

## 3. Hardware Architecture & Hardware Interfaces

- **Microcontroller**: ESP32-S3 (SPI, WiFi, NVS storage, Dual Core).
- **Primary TFT Display**: ST7789 320x240 TFT over SPI.
  - Constant Region: Inning & state (e.g. "Top 3rd"), Team Logos (e.g. TOR @ ATL).
  - Carousel Region: Dynamic stats (Current Pitcher, Batter, Last Play).
- **Score Displays**: 2x MAX7219 8x8 LED matrix modules (1 Home score, 1 Away score).
- **Count & Out LEDs**: 7 discrete GPIO-driven LEDs:
  - 3 Green/Yellow LEDs for **Balls** (B1, B2, B3)
  - 2 Red/Yellow LEDs for **Strikes** (S1, S2)
  - 2 Red LEDs for **Outs** (O1, O2)

## 4. Users and Use Cases

| User | Use case |
|---|---|
| Desk Fan | Glance at physical LED scores, count, and TFT details during a live game |
| Multi-Team Fan | Configure top 3 favorite teams; board tracks priority game and periodically updates on others |
| First-Time Setup | Connect to ESP32 AP portal via phone/PC browser, select WiFi and ranked team preferences |

## 5. Functional Requirements

### 5.1 Web Configuration & Preferences

- FR-1: Host an HTTP configuration portal (Captive Portal in AP mode or local web server in STA mode).
- FR-2: Save network credentials (SSID/Password) and ranked list of preferred team IDs in NVS (`Preferences`).

### 5.2 Game Selection & Priority Tracking

- FR-3: Poll `GET /v1/schedule?sportId=1&date={today}` to identify active/upcoming games for selected teams.
- FR-4: Select highest-priority team currently in a `Live` game as the primary game.
- FR-5: If secondary selected teams are also in `Live` games, periodically cycle/flash their score and status on the TFT carousel.

### 5.3 Live Score & Hardware Output

- FR-6: Poll `GET /v1/game/{gamePk}/linescore` every ~5 seconds for the active game.
- FR-7: Update Home and Away score digits on the MAX7219 LED matrices.
- FR-8: Light discrete LEDs according to `balls` (0-3), `strikes` (0-2), and `outs` (0-2).
- FR-9: Render team logos, current inning, inning half (`Top`/`Bottom`/`Middle`/`End`), and dynamic carousel stat lines on the ST7789 TFT display.

## 6. Non-Functional Requirements

- NFR-1 **Refresh Latency**: Polling loop updates hardware state within 5 s of API response during live games.
- NFR-2 **Resilience**: Handle WiFi dropouts without crashing; display staleness indicator or last-known scores.
- NFR-3 **Bandwidth Efficiency**: Use `/linescore` (~2 KB) for fast live ticks instead of full `/boxscore` (~80 KB).
- NFR-4 **Defensive Parsing**: Handle string-formatted stats and sentinels (`"-.--"`, `".---"`) safely.


## 7. Data sources

Primary: `https://statsapi.mlb.com/api/v1` (see
[`openapi/mlb-stats-api.yaml`](openapi/mlb-stats-api.yaml)).
Fallback/exit strategy: commercial feeds (Sportradar, SportsData.io) —
see ADR-0001.

## 8. Release criteria

- All FRs above demonstrable against a real game day.
- Stale-data and error states exercised (e.g., request during MLB
  off-season or an outage).
- OpenAPI schema validates against live sample responses for the covered
  endpoints.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Undocumented API changes without notice | Lenient parsing, contract tests against live responses, community reference monitoring |
| No SLA | ADR-0001 exit strategy to commercial provider |
| Rate limiting / IP blocking | Conservative cadence, caching, single shared fetcher |
| Numeric stats returned as strings/placeholder sentinels (`"-.--"`, `".---"`) | Defensive parsing with fallback defaults |
| Long off-season (no games) | Schedule endpoint returns empty `dates`; handle gracefully with an "off-season" UI state |
