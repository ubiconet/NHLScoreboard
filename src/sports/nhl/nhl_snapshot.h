#pragma once

#include <Arduino.h>

// Shared "snapshot" structs written by the core-0 NHL data task and read by
// the core-1 render loop (POD only; generation counters in nhl_state signal
// fresh publishes). Field truth comes from the NHL web API — endpoint
// semantics documented in docs/features/nhl-api/README.md.

// One currently- or recently-active penalty (derived from the landing
// summary by the data task's game-clock math).
struct NhlPenalty {
  char teamAbbrev[4];  // penalized team
  char lastName[14];   // committedByPlayer's last name ("" if unknown)
  int  number;         // sweater number (0 = unknown)
  char desc[14];       // "slashing", "tripping"...
  int  remainSec;      // seconds left on the clock (0 = just expired)
  int  durMin;         // original minutes (2/4/5)
};

// Live-game state from /v1/gamecenter/{id}/landing (5 s poll).
struct GameSnapshot {
  bool  valid;
  long  gameId;
  char  gameState[8];        // FUT/PREVIEW/LIVE/CRIT/FINAL/OFF
  int   homeTeamId, awayTeamId;
  char  homeAbbrev[4], awayAbbrev[4];
  int   homeScore, awayScore;
  int   homeSog, awaySog;    // shots on goal
  int   period;              // 1..3, 4+ = OT
  char  periodType[4];       // REG/OT/SO
  int   clockSec;            // seconds remaining in the period
  bool  clockRunning;
  bool  inIntermission;
  int   homePenaltyCount, awayPenaltyCount;  // active (0..2 LEDs each)
  NhlPenalty penalties[4];   // active ones, newest first (TFT detail strip)
  int   penaltyCount;
};

// One game from the day slate (/v1/score/{date}).
struct NhlDayGame {
  long  gameId;
  char  homeAbbrev[4], awayAbbrev[4];
  int   homeTeamId, awayTeamId;
  int   homeScore, awayScore;
  char  gameState[8];
  char  startUtc[26];        // ISO-8601 Z
};

// Day-slate snapshot: every game today (live selection + ticker) plus, in
// the hours after local midnight, the previous API date's late games (west
// coast slates run past midnight ET; without the merge the board would
// drop a still-live game at the date rollover).
struct ScheduleSnapshot {
  NhlDayGame games[26];
  size_t count;
  bool  valid;
};

// Division standings snapshot (from /v1/standings, refreshed a few times
// a day — rows only change after games). Four divisions in fixed order,
// teams pre-sorted by the feed's sequence (points/rank) within division.
struct StandingsRow {
  char abbrev[4];
  int16_t wins, losses, otLosses, points;
};
struct StandingsSnapshot {
  char divisionName[4][12];   // "Atlantic", "Metropolitan", ...
  StandingsRow rows[4][8];
  uint8_t count[4];
  bool valid;
};

// Core-0 NHL data task lifecycle.
void startNhlDataTask();
