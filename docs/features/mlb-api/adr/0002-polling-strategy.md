# ADR-0002 — Polling cadence driven by game state; no push channel

- **Status**: Accepted
- **Date**: 2026-09-07

## Context

MLB's `statsapi.mlb.com` endpoints are request/response HTTP with no
published WebSocket or SSE channel for live scores. The scoreboard must feel
live (~30 s staleness budget from NFR-1) without hammering an undocumented,
limit-unknown service. Baseball has long idle periods (between pitches,
between innings) punctuated by bursts of action, so cadence should track
game state rather than poll at a flat rate.

## Decision

Poll `GET /v1/schedule?sportId=1&date={date}` (for the day's overview) and
`GET /v1/game/{gamePk}/linescore` (only for a game with an open detail view)
on a cadence chosen by aggregate game state:

| Aggregate state of the day's games | Cadence |
|---|---|
| No games, or all `Preview` more than 1 h away | 10 min |
| Games within 1 h (`Preview`, `Pre-Game`) | 2 min |
| Any game `Live` / `In Progress` | 20 s |
| Any game in a late/high-leverage state (9th inning or later) | 10 s |
| All games `Final`/`Game Over` | stop polling (refetch on focus) |

Implementation rules:

- A single shared scheduler owns all polling; components subscribe, so N
  visible games still cost 1 request per tick (the schedule endpoint covers
  every game for the date at once).
- Responses are cached and deduplicated; a byte-identical response does not
  re-render the UI.
- The lightweight `/linescore` endpoint (not the full `/boxscore` or
  `/v1.1/game/{gamePk}/feed/live`) is used for in-progress polling of a
  single open game, since it is materially smaller and covers inning,
  count, outs, and baserunners.
- Refetch on window/tab focus and on network reconnect in addition to the
  timer.
- On repeated failures, back off exponentially (cap 5 min) and surface a
  staleness indicator rather than clearing displayed data.

## Alternatives considered

- **Constant fast polling (5 s)**: mimics the MLB app/MLB.com Gameday, but
  wasteful and increases blocking risk for a service with no published
  limits.
- **Full live feed (`/v1.1/game/{gamePk}/feed/live`) on every poll**: richest
  payload (play-by-play, full state) but far larger than `/linescore`;
  reserved for on-demand play-by-play drill-down, not the polling loop.
- **Push via a third-party relay** (e.g., a commercial feed): deferred per
  ADR-0001 exit strategy; only warranted if polling proves insufficient.

## Consequences

- Worst case during late/high-leverage innings: 6 requests/min from one
  client for the open-game linescore — modest and comparable to a browser
  tab refreshing MLB Gameday.
- Score/count updates land within the 30 s NFR-1 budget at the `Live`/late-
  inning cadences.
