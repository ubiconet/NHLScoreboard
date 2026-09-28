# ADR-0002 — Polling cadence driven by game state; no push channel

- **Status**: Accepted (NHL adaptation of the MLB-template ADR)
- **Date**: 2026-09-24

## Context

The NHL web API is request/response HTTP with no published WebSocket/SSE
channel for live scores. The scoreboard must feel live (~30 s staleness
budget) without hammering an undocumented, limit-unknown service — and every
request is a TLS round trip (see ADR-0001), so request count matters more
than in the MLB template. Hockey has natural pauses (intermissions, TV
timeouts, stoppages) but also continuous play, so cadence should track game
state.

A second, NHL-specific constraint: Cloudflare serves the live endpoints with
`Cache-Control: max-age=14`, so polling faster than ~15 s returns cached
bytes anyway.

## Decision

Poll `GET /v1/score/{date}` (one request covers every game that day) and
`GET /v1/gamecenter/{id}/landing` (11 KB, per followed live game) on a
cadence chosen by aggregate game state:

| Aggregate state of the day's games | Cadence |
|---|---|
| No games, or all `FUT` more than 1 h away | 10 min |
| Games within 1 h (`FUT`, `PREVIEW`) | 2 min |
| Any game `LIVE` (period in play or intermission) | 20 s |
| Any game `CRIT` (final minutes / OT / shootout) | 15 s (CDN floor) |
| All games `FINAL`/`OFF` | stop polling (refetch on state change) |

Implementation rules:

- A single scheduler on core 0 owns all polling; the day fetch covers every
  game at once, only followed live games add per-game `landing` requests.
- `/now` endpoints are never used — compute dates locally from the
  NTP-synced clock (they 307-redirect, doubling TLS round trips).
- Responses are cached and deduplicated; a byte-identical response does not
  re-publish a snapshot or re-render.
- `landing` (11 KB) — not `boxscore` (13.5 KB) or `play-by-play` (146 KB) —
  is the in-progress poll; it carries clock, period, score, shots, and the
  per-period scoring summary.
- The week `/v1/schedule/{date}` fetch (85 KB) backs the schedule cache for
  upcoming-game countdowns and refreshes at a slow cadence (e.g. hourly /
  on day rollover), not per poll tick.
- On repeated failures, back off exponentially (cap 5 min) and surface a
  staleness indicator rather than clearing displayed data.
- One TLS connection at a time, reused within a poll cycle; the feed session
  closes before the OTA updater runs (template rule, now applies to every
  fetch).

## Consequences

- Worst case (one followed game in `CRIT`): ~4 day-fetches + 4 landing
  fetches/min — modest for a CDN-fronted service and under the 30 s budget.
- No request lands faster than the CDN can refresh (14 s), so we never pay
  TLS overhead for stale bytes.
