# MLB Data Integration — Feature Documentation

This folder documents the MLB baseball data API that backs the MLBScoreboard
application, for both human readers (PRD, ADRs) and machine readers (OpenAPI
schema).

## Important context

MLB Advanced Media does **not** publish official developer documentation for
the "MLB Stats API" (`statsapi.mlb.com`), but it is free, unauthenticated, and
widely used by the open-source community (e.g. the MLB-StatsAPI Python
wrapper, and reverse-engineered from MLB.com/the MLB app themselves). Treat
the schema as community-derived and subject to unannounced change. All
responses carry a `copyright` string crediting MLB Advanced Media, L.P.; see
http://gdx.mlb.com/components/copyright.txt for terms of use.

- **Stats API**: `https://statsapi.mlb.com/api/v1/...` (a few endpoints use
  `v1.1`, notably the live game feed).
- Commercial, officially documented alternatives exist (Sportradar,
  SportsData.io) if contractual guarantees are required.

## Contents

| File | Audience | Purpose |
|---|---|---|
| [`PRD.md`](PRD.md) | Humans | Product requirements for MLB data integration |
| [`adr/0001-use-mlb-public-api.md`](adr/0001-use-mlb-public-api.md) | Humans | Decision: use the public MLB Stats API as primary data source |
| [`adr/0002-polling-strategy.md`](adr/0002-polling-strategy.md) | Humans | Decision: polling cadence per game state instead of push/WebSocket |
| [`openapi/mlb-stats-api.yaml`](openapi/mlb-stats-api.yaml) | Machines | OpenAPI 3.1 schema for the scoreboard-relevant endpoints |

## Endpoint quick reference (verified 2026-09-07)

| Purpose | Endpoint |
|---|---|
| Day schedule with linescores | `GET /v1/schedule?sportId=1&date={date}` |
| Live game feed (score, linescore, plays) | `GET /v1.1/game/{gamePk}/feed/live` |
| Linescore only (lightweight, for polling) | `GET /v1/game/{gamePk}/linescore` |
| Boxscore (batting/pitching stat lines) | `GET /v1/game/{gamePk}/boxscore` |
| Play-by-play | `GET /v1/game/{gamePk}/playByPlay` |
| Standings | `GET /v1/standings?leagueId=103,104&date={date}` |
| Team info | `GET /v1/teams/{teamId}` |
| Person/player info | `GET /v1/people/{personId}` |

Common conventions:

- All dates are `YYYY-MM-DD` strings; timestamps are ISO-8601 UTC
  (e.g. `gameDate: "2026-09-07T17:05:00Z"`).
- `sportId=1` is Major League Baseball (the same family of endpoints serves
  MiLB and other levels via other sport ids).
- `gamePk` is MLB's numeric game identifier (e.g. `823415`); it is opaque
  (not date/season-derived like some other leagues' game ids).
- `leagueId=103` is American League, `leagueId=104` is National League.
- Game status lives in `status.abstractGameState` (`Preview`, `Live`,
  `Final`), `status.detailedState` (e.g. `Pre-Game`, `In Progress`,
  `Final`, `Game Over`, `Postponed`, `Delayed`), and `status.codedGameState`
  (single-letter code, e.g. `S`, `P`, `I`, `F`).
- Linescore carries `currentInning`, `inningState` (`Top`/`Bottom`/`Middle`/
  `End`), `balls`/`strikes`/`outs`, and base-runner/offense-defense context.
- Boxscore is organized as `teams.away` / `teams.home`, each with
  `teamStats` (aggregate batting/pitching/fielding) and a `players` map
  keyed by `"ID<personId>"`, plus `batters`/`pitchers`/`bench`/`bullpen`/
  `battingOrder` id arrays for convenient roster slicing.
- No known rate limit is published; the API is not intended for
  high-frequency scraping, so poll conservatively (see ADR-0002).

## References

- [MLB-StatsAPI (toddrob99) endpoint reference](https://github.com/toddrob99/MLB-StatsAPI/wiki/Endpoints) — the most complete community reference
- [MLB-StatsAPI Python wrapper](https://github.com/toddrob99/MLB-StatsAPI) — maintained Python client
- [Sportradar MLB API](https://developer.sportradar.com/baseball/reference/mlb-overview) — commercial alternative
- [SportsData.io MLB API](https://sportsdata.io/developers/api-documentation/mlb) — commercial alternative
