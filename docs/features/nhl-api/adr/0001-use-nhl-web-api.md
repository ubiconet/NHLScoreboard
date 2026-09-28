# ADR-0001 — Use the NHL public web API as the primary data source

- **Status**: Accepted (supersedes the MLB-template ADR of the same number)
- **Date**: 2026-09-24

## Context

NHLScoreboard needs live NHL scores, schedules, boxscores, and standings.
Candidate sources:

1. **NHL public web API** (`https://api-web.nhle.com/v1/`, free,
   unauthenticated, undocumented — the same endpoints nhl.com itself uses).
   Plus the stats REST surface (`https://api.nhle.com/stats/rest/en/…`) for
   reference data like the team directory.
2. **Commercial APIs** (Sportradar — the NHL's official data partner,
   SportsData.io): officially documented, SLA-backed, paid licensing.
3. **ESPN hidden API** (`site.api.espn.com`): free and convenient, but a
   third-party dependency for the league's own data.
4. **Community scrapers/libraries**: wrap the same endpoints; add a
   dependency without adding a data guarantee; no maintained ESP32/C++
   client exists anyway.

## Decision

Use the NHL public web API directly as the primary data source, behind our
own thin data-access layer (`nhl_client.*` on core 0). Use ESPN's
`hockey/nhl` news feed only for the news ticker, where the NHL itself has no
accessible endpoint (verified: `/v1/news` 404, `nhl.com/rss/news.xml` 403
bot-blocked).

## Consequences

**Positive**

- Free, no key management, no contract; covers every scoreboard need
  (day scores, week schedule, live game state, boxscore, standings).
- Same API that powers nhl.com and the official NHL app — well-exercised in
  production even without formal documentation.
- Payloads are compact where it matters for a microcontroller: the day
  scoreboard is 42 KB, the live-game `landing` summary 11 KB.

**Negative / risks & mitigations**

- Undocumented and can change without notice (the league already killed
  `statsapi.web.nhl.com` once). Mitigate with: lenient parsing, contract
  tests against checked-in live samples (`samples/`), configurable base URL,
  and monitoring the community `dword4/nhlapi` documentation for breakage.
- No SLA or rate-limit guarantee: keep request volume conservative and cache.
- **HTTPS-only** (unlike MLB's plain-HTTP feeds): every fetch costs a TLS
  session. The firmware must reuse connections and budget heap accordingly —
  see the constraints section of `docs/features/nhl-api/README.md`.
- No server-side field filtering (`?fields=`): payloads must be trimmed in
  the parser, on core 0.
- **Exit strategy**: swap the data-access layer to Sportradar's NHL API
  (official partner, closest feature match) if reliability becomes
  unacceptable; the rest of the app is insulated behind that layer.
