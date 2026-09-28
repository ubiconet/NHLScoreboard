#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// NHL feed endpoints + JSON filters (core 0 only). Transport lives in
// common/comms/http_fetcher; the NHL web API is HTTPS-only, so these calls
// ride the shared TLS session (endpoints/payload sizes documented in
// docs/features/nhl-api/README.md).

// Day slate for a local date ("YYYY-MM-DD") from /v1/score/{date} (~42 KB,
// filtered). Keeps per-game: id, gameType, gameState, startTimeUTC,
// home/away {id, abbrev, score}.
bool fetchNhlDayScore(JsonDocument& doc, const char* dateStr);

// Live-game summary from /v1/gamecenter/{id}/landing (~11 KB, filtered):
// gameState, periodDescriptor, clock, team score/sog, penalty summary.
bool fetchNhlGameLanding(JsonDocument& doc, long gameId);

// News headlines from ESPN (plain HTTP — see the News section of the API
// doc for why the RSS host lost to the api host).
bool fetchEspnNhlNews(JsonDocument& doc, int limit = 10);
