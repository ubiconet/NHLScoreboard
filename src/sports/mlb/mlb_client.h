#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Fetch today's schedule JSON from statsapi.mlb.com
bool fetchMlbSchedule(const char* dateStr, JsonDocument& doc);

// Fetch a date range of MLB games, optionally narrowed server-side to the
// given team ids (0 entries are skipped; pass nullptr for the full slate).
// Used to find the next matchup for each selected team while the device is
// waiting for a live game. Keeping the payload small matters: it must be
// bufferable while the TLS session is open.
bool fetchMlbScheduleRange(const char* startDate, const char* endDate,
                           JsonDocument& doc, const int teamIds[3] = nullptr);

// Fetch lightweight linescore JSON for a specific game
bool fetchMlbLinescore(int gamePk, JsonDocument& doc);

// Fetch the most recent completed plate appearance for the active game.
bool fetchMlbLatestPlay(int gamePk, JsonDocument& doc);

// Fetch AL+NL division standings (one record set per division). season=0 lets
// the API default to the current season.
bool fetchMlbStandings(JsonDocument& doc, int season = 0);

// Fetch the latest MLB news headlines from ESPN (used for the upcoming-games news slides).
bool fetchEspnMlbNews(JsonDocument& doc, int limit = 10);

// Select highest priority gamePk from daily schedule based on preferred team IDs
int selectGamePkForTeams(JsonObjectConst scheduleRoot, const int preferredTeamIds[3]);

// True when a specific scheduled game is final.
bool isMlbGameFinal(JsonObjectConst scheduleRoot, int gamePk);
