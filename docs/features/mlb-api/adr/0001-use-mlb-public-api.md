# ADR-0001 — Use the public MLB Stats API as the primary data source

- **Status**: Accepted
- **Date**: 2026-09-07

## Context

MLBScoreboard needs live MLB scores, schedules, boxscores, and standings.
Candidate sources:

1. **MLB Stats API public endpoints** (`statsapi.mlb.com/api/v1`, free,
   unauthenticated, undocumented).
2. **Commercial APIs** (Sportradar, SportsData.io): officially documented,
   SLA-backed, paid, require licensing agreements.
3. **Community scrapers/libraries** (e.g., MLB-StatsAPI Python package): wrap
   the same public endpoints; add a dependency without adding a data
   guarantee, and this project targets an embedded C++ (ESP32) client anyway.

## Decision

Use the MLB Stats API public endpoints directly as the primary data source,
behind a thin, well-tested data-access layer of our own.

## Consequences

**Positive**

- Free, no key management, no contract; covers every scoreboard need
  (schedule, live score, line score, boxscore, play-by-play, standings).
- Same API that powers MLB.com and the official MLB app, so it is
  well-exercised in production even without formal documentation.

**Negative / risks & mitigations**

- Undocumented and can change without notice. Mitigate with: lenient
  parsing, contract tests against live samples, configurable base URL,
  and monitoring the community MLB-StatsAPI wiki for breakage reports.
- No SLA or rate-limit guarantee: keep request volume conservative and cache.
- Numeric stats are frequently serialized as strings, including sentinel
  placeholders (`"-.--"`, `".---"`) for undefined values (e.g. ERA with no
  innings pitched); the data-access layer must parse these defensively
  rather than assuming numeric JSON types.
- **Exit strategy**: if reliability becomes unacceptable, swap the
  data-access layer to a commercial provider (Sportradar MLB API is the
  closest feature match); the rest of the app is insulated by that layer.

## Notes

We deliberately avoid depending on third-party wrapper libraries (e.g. the
Python MLB-StatsAPI package) so that schema drift is observed and handled in
code we control, and because the target runtime (ESP32 / C++) has no
equivalent maintained client anyway.
