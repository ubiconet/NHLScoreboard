# NHL API — primary data source (verified reference)

The NHL publishes **no official API documentation**. What follows was verified
live against production endpoints on **2026-09-24** (preseason week of the
2026-27 season). The NHL runs two public, unauthenticated surfaces:

| Surface | Base URL | Role for this project |
|---|---|---|
| **Web API** (powers nhl.com) | `https://api-web.nhle.com/v1/` | **Primary**: schedule, live scores, game detail, standings |
| **Stats REST** | `https://api.nhle.com/stats/rest/en/…` | Secondary: team directory (source for the team table) |

Community references (kept up to date by reverse engineering):
<https://github.com/dword4/nhlapi> and the unofficial NHL API reference
mirrors linked from it. The old `statsapi.web.nhl.com/api/v1` API is **dead**
(dark since ~2023); any guide referencing it is out of date.

## Transport properties (all verified)

- **HTTPS only.** `http://` answers `301` (Cloudflare) to the `https://` URL.
  This is a major departure from the MLB template, which ran feeds over plain
  HTTP and reserved TLS for OTA only — see *ESP32 constraints* below.
- **No authentication**, no API key, no required headers (works with a bare
  `curl` User-Agent).
- **No response filtering.** Unlike MLB's `?fields=` parameter there is no way
  to slim a payload server-side; sizes below are full bodies.
- HTTP/1.1 **keep-alive** works; served via Cloudflare with
  `Cache-Control: max-age=14` and a weak ETag — polling a live endpoint faster
  than ~15 s mostly hits CDN cache.
- Localized string fields are objects: `{"default": "Penguins", "fr": …}` —
  always read the `default` sub-key.
- `/v1/*/now` endpoints answer **307 redirects** to the current date. On the
  device, compute the date locally from the NTP-synced clock instead of
  following redirects (a redirect doubles the TLS round trips).

## Game identity & state

- **Game id** (e.g. `2026010016`): `YYYY` + 2-digit `gameType` + 4-digit
  sequence. `gameType`: `1` preseason, `2` regular season, `3` playoffs.
  The `season` field is the two years joined: `20262027`.
- **`gameState`** (observed: `FUT`, `FINAL`, `OFF`; community also documents
  `PREVIEW`, `LIVE`, `CRIT`):
  - `FUT` — scheduled, future
  - `PREVIEW` — game day, before start
  - `LIVE` — in progress
  - `CRIT` — critical/late (final minutes, OT)
  - `FINAL` / `OFF` — completed (`OFF` = official/finalized)
- **`periodDescriptor`**: `{"number": 3, "periodType": "REG", "maxRegulationPeriods": 3}`
  — `periodType` is `REG`, `OT`, or `SO` (shootout).

## Endpoints

### `GET /v1/score/{yyyy-mm-dd}` — day scoreboard *(waiting-mode poll)*

~42 KB. Top-level: `prevDate`, `currentDate`, `nextDate`, `gameWeek[]`
(per-day `date`/`dayAbbrev`/`numberOfGames` metadata only), `games[]`.

Each game carries the live essentials in one flat array (this is the smallest
"everything happening today" payload):

```json
{"id": 2026010016, "season": 20262027, "gameType": 1,
 "gameDate": "2026-09-21", "startTimeUTC": "2026-09-21T23:00:00Z",
 "gameState": "FINAL", "gameScheduleState": "OK",
 "clock": {"timeRemaining": "00:00", "secondsRemaining": 0,
           "running": false, "inIntermission": false},
 "period": 3, "periodDescriptor": {"number": 3, "periodType": "REG",
                                   "maxRegulationPeriods": 3},
 "gameOutcome": {"lastPeriodType": "REG"},
 "awayTeam": {"id": 7, "name": {"default": "Sabres"}, "abbrev": "BUF",
              "score": 1, "sog": 24,
              "logo": "https://assets.nhle.com/logos/nhl/svg/BUF_light.svg"},
 "homeTeam": {"id": 5, "name": {"default": "Penguins"}, "abbrev": "PIT",
              "score": 4, "sog": 28, "logo": "..."},
 "goals": [ … ], "tvBroadcasts": [ … ], "venue": {"default": "PPG Paints Arena"},
 "gameCenterLink": "/gamecenter/pit-vs-buf/2026/09/21/2026010016"}
```

One request covers every game of the day — the analog of the MLB template's
single schedule fetch.

### `GET /v1/schedule/{yyyy-mm-dd}` — week schedule *(schedule cache / countdown)*

~85 KB. Returns the **Mon–Sun week containing the date**, plus:
`nextStartDate` / `previousStartDate` (for paging), `numberOfGames`, and the
season boundaries `preSeasonStartDate`, `regularSeasonStartDate`,
`regularSeasonEndDate`, `playoffEndDate` (useful for "is today in season").

Game objects here add vs. `/v1/score`: `commonName`/`placeName` (split team
names), `darkLogo`, `winningGoalie`, `winningGoalScorer`, `threeMinRecap`,
`condensedGame`, `oddsPartners`.

### `GET /v1/gamecenter/{id}/landing` — live game summary *(live-screen poll)*

~11 KB. **The NHL analog of MLB's `/linescore`** — poll this while a followed
game is live:

```json
{"id": 2025020001, "gameState": "OFF",
 "periodDescriptor": {"number": 3, "periodType": "REG"},
 "clock": {"timeRemaining": "00:00", "secondsRemaining": 0,
           "running": false, "inIntermission": false},
 "shootoutInUse": false, "otInUse": true, "tiesInUse": false,
 "maxPeriods": 5, "regPeriods": 3,
 "homeTeam": {"abbrev": "FLA", "score": 3, "sog": 37, …},
 "awayTeam": {"abbrev": "CHI", "score": 2, "sog": 19, …},
 "summary": {
   "scoring": [
     {"periodDescriptor": {"number": 1}, "goals": [
        {"strength": "ev", "playerId": 8483493, "name": {"default": "F. Nazar"},
         "teamAbbrev": {"default": "CHI"}, "situationCode": "1551",
         "highlightClipSharingUrl": "https://nhl.com/vid…"}]},
     … one entry per period with goals …],
   "threeStars": [ {"star": 1, "name": {"default": "…"}, "teamAbbrev": "FLA",
                    "goals": 1, "assists": 0, "points": 1} ],
   "penalties": [ {"periodDescriptor": {"number": 1}, "penalties": [
        {"timeInPeriod": "06:58", "duration": 2, "descKey": "slashing",
         "teamAbbrev": {"default": "CHI"}}]}]}}
```

A period-by-period **linescore is derived** from `summary.scoring[]` (count
`goals[]` per `teamAbbrev` per period). Shots on goal per period are **not**
exposed here — only game totals (`sog`); per-period shots must be aggregated
from play-by-play if a screen ever needs them.

`situationCode` encodes strength as four digits —
`[awayGoalies][awaySkaters][homeSkaters][homeGoalies]`, e.g. `"1551"` = 5v5
with both goalies, `"0651"` = away team on an empty-net extra attacker. This
is the natural source for a **power-play indicator** (replacing MLB's
balls/strikes/outs LEDs — actual LED meaning is a board-topology decision,
out of scope here).

### `GET /v1/gamecenter/{id}/boxscore` — box score

~13.5 KB. Same envelope (`gameState`, `clock`, `periodDescriptor`, teams with
`score` + `sog`) plus `playerByGameStats: {homeTeam/awayTeam:
{forwards, defense, goalies}}` (per-player G/A/TOI/SOG…) and `gameOutcome`.
Use for a stats screen; **not** needed for the live poll.

### `GET /v1/gamecenter/{id}/play-by-play` — full event stream

~146 KB for a completed regular-season game. `plays[]` (361 events in the
sample): `eventId`, `periodDescriptor`, `timeInPeriod`, `timeRemaining`,
`situationCode`, `typeCode`/`typeDescKey` (`period-start`, `faceoff`,
`shot-on-goal`, `goal`, `stoppage`, `game-end`, …), event `details` (players,
zone, coordinates), `sortOrder`; plus `rosterSpots` and a `summary`.
The last `plays[]` entry is the "current play" analog of MLB's live feed.

Too large for the render path and marginal for the ESP32 parse budget —
reserve for an on-demand drill-down screen, parsed on core 0 with a streaming
filter (see constraints).

### `GET /v1/standings/{yyyy-mm-dd}` — standings

~60 KB. `/v1/standings/now` 307-redirects to the latest date with standings —
during the 2026 preseason that was **2026-04-17** (end of the previous
season), which is the right behavior for an offseason carousel. Entries
include `divisionAbbrev`/`conferenceAbbrev` + rank sequences,
`wildCardIndicator`, `clinchIndicator`, `gamesPlayed`, `points`, `pointPctg`,
`goalDifferential`, L10/home/road splits.

### `GET https://api.nhle.com/stats/rest/en/team` — team directory

~6.7 KB: `{"data": [{"id": 32, "franchiseId": 27, "fullName": "Quebec
Nordiques", "leagueId": 133, "rawTricode": "QUE", "triCode": "QUE"}, …]}`.
**Includes defunct franchises** — filter to the 32 active clubs when
generating the hardcoded `nhl_teams.*` table (`{id, abbrev, label}`). Supports
`cayenne_exp` filter expressions (community-documented); the scoreboard
itself never needs them.

### Team logos & player assets

Embedded in schedule/score payloads:
- `https://assets.nhle.com/logos/nhl/svg/{ABBREV}_light.svg`
- `https://assets.nhle.com/logos/nhl/svg/{ABBREV}_dark.svg`
  (some carry `?season=YYYYYYYY`)
- Player headshots: `https://assets.nhle.com/mugs/nhl/{season}/{ABBREV}/{playerId}.png`

### News

**No accessible official NHL endpoint**: `/v1/news` → 404, and
`https://www.nhl.com/rss/news.xml` → 403 (Akamai bot filter — would likely
block the ESP32 too). Chosen source (live since v1.8): **ESPN's NHL feed on
`site.api.espn.com`** over plain HTTP —
`http://site.api.espn.com/apis/site/v2/sports/hockey/nhl/news?limit=10`
(~27 KB with limit=5; `articles[].headline`/`description` are all the
ticker parses, via a streaming filter). ESPN's `www.espn.com/espn/rss/nhl/news`
RSS carries the same stories at 9 KB but **301-forces HTTPS**, which the
TLS-free feed architecture (see `common/comms/http_fetcher.h`) avoids; the
api host serves plain HTTP with no redirect and tolerated an ESP32
User-Agent in testing. Keep the ≥30-min cache rule and the gentle retry
pacing (`NHL_NEWS_RETRY_MS`) — ESPN's edge has a temper.

## MLB template → NHL mapping (porting cheat sheet)

| MLB template | NHL equivalent |
|---|---|
| `gamePk` | `id` (`2026010016`) |
| `/api/v1/schedule?sportId=1&date=` | `/v1/score/{date}` (day, 42 KB) or `/v1/schedule/{date}` (week, 85 KB) |
| `/game/{pk}/linescore` | `/v1/gamecenter/{id}/landing` (11 KB) |
| `/game/{pk}/boxscore` | `/v1/gamecenter/{id}/boxscore` (13.5 KB) |
| `/v1.1/game/{pk}/feed/live` | `/v1/gamecenter/{id}/play-by-play` (146 KB) |
| `/api/v1/standings` | `/v1/standings/{date}` |
| `detailedState` (PreGame/InPlay/Over) | `gameState` (PREVIEW/LIVE·CRIT/FINAL·OFF) |
| inning / half inning | `periodDescriptor.number` + `periodType` (REG/OT/SO) |
| inning-by-inning runs | derive from `landing.summary.scoring[]` |
| at-bat | last `plays[]` entry / `situationCode` strength |
| balls/strikes/outs | power-play/strength indicator (board topology TBD) |
| `team.id` / `abbrev` / name | `id` / `abbrev` / `placeName` + `commonName` |
| W-L record in standings | `points`, `pointPctg`, wildcard flags |
| ESPN `mlb` news | ESPN `hockey/nhl` news |

## ESP32 constraints (architecture notes for the rewrite)

- **Everything is TLS now.** The MLB template ran feeds on plain HTTP with a
  single reused TLS session reserved for OTA. With NHL feeds, every fetch is
  TLS: plan on one reused `WiFiClientSecure` connection per poll cycle on
  core 0, never concurrent sessions (the OTA rule "no second TLS connection
  while one is alive" still applies and now interacts with feed polling —
  the feed session must close before the OTA updater runs, as the template
  already does via `http_fetch::closeSession()`).
- **Payload budget.** `score` (42 KB), `landing` (11 KB) and `boxscore`
  (13.5 KB) fit the template's buffered-parse discipline. `schedule` week
  (85 KB), `standings` (60 KB) and especially `play-by-play` (146 KB) must
  be fetched sparsely (schedule cache / carousel TTL) or parsed with a
  streaming filter on core 0 — never parsed on the render core.
- **No `fields=` filtering** — filter in the parser.
- **CDN freshness is 14 s** — a 15–20 s live cadence is the fastest useful
  poll; faster requests just hit Cloudflare cache.
- **Avoid `/now`** — compute dates locally (NTP) rather than following 307s.
- Localized fields are objects — always read `.default`.

## Samples (captured 2026-09-24)

| File | Endpoint | Size |
|---|---|---|
| `samples/score-2026-09-21.json` | `/v1/score/2026-09-21` | 42 KB |
| `samples/schedule-2026-09-21.json` | `/v1/schedule/2026-09-21` | 85 KB |
| `samples/landing-2025020001.json` | `/v1/gamecenter/2025020001/landing` | 11 KB |
| `samples/boxscore-2025020001.json` | `/v1/gamecenter/2025020001/boxscore` | 13.5 KB |
| `samples/play-by-play-2025020001.json` | `/v1/gamecenter/2025020001/play-by-play` | 146 KB |
