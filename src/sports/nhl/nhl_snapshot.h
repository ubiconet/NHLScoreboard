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

struct NhlGoalPlayer {
  int  playerId;
  int  number;
  char name[32];
};

struct NhlGoal {
  bool valid;
  long eventId;
  int  homeScore, awayScore;
  char teamAbbrev[4];
  NhlGoalPlayer scorer;
  NhlGoalPlayer assists[2];
  uint8_t assistCount;
};

// Three stars of the game (landing summary; usually populated only at or
// near game end).
struct NhlThreeStar {
  char name[24];
  char teamAbbrev[4];
  int  number;       // sweater number (0 = unknown)
  int  goals, assists;
};

// One goal of the followed game (landing summary.scoring), in game order —
// the intermission/postgame scoring pages list these.
struct NhlGoalLine {
  uint8_t period;      // periodDescriptor.number
  char periodType[4];  // REG/OT/SO
  char time[6];        // "MM:SS" elapsed in the period
  char teamAbbrev[4];
  char name[20];       // "F. Nazar"
  char tag[3];         // "PP"/"SH"/"EN"/"PS", "" at even strength
};
static const size_t NHL_MAX_GOAL_LINES = 18;

// Team comparison from /v1/gamecenter/{id}/right-rail teamGameStats —
// fetched once per intermission and once at the final, not on the 5 s poll.
struct NhlTeamStats {
  bool valid;
  char homePowerPlay[6], awayPowerPlay[6];  // "1/2" goals/opportunities
  int16_t homeFaceoffPct, awayFaceoffPct;   // whole percent
  int16_t homeHits, awayHits;
  int16_t homeBlocks, awayBlocks;
};

// Live-game state from /v1/gamecenter/{id}/landing (5 s poll).
struct GameSnapshot {
  bool  valid;
  long  gameId;
  char  gameState[8];        // FUT/PRE/LIVE/CRIT/FINAL/OFF
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
  NhlGoal latestGoal;
  // Per-period scoring derived from summary.scoring: index 0..2 = P1..P3,
  // 3 = OT (multi-OT collapses into one column), 4 = SO. otColumn/soColumn
  // say which extra columns the linescore strip should show.
  uint8_t homePeriodGoals[5], awayPeriodGoals[5];
  bool  otColumn, soColumn;
  NhlThreeStar threeStars[3];
  uint8_t threeStarCount;
  NhlGoalLine goals[NHL_MAX_GOAL_LINES];  // game order, oldest first
  uint8_t goalCount;
  NhlTeamStats teamStats;
};

// Appends a goal line; a full list drops its oldest entry so the most
// recent goals always survive.
static inline void appendGoalLine(GameSnapshot& s, const NhlGoalLine& line) {
  if (s.goalCount >= NHL_MAX_GOAL_LINES) {
    memmove(&s.goals[0], &s.goals[1],
            (NHL_MAX_GOAL_LINES - 1) * sizeof(NhlGoalLine));
    s.goalCount = NHL_MAX_GOAL_LINES - 1;
  }
  s.goals[s.goalCount++] = line;
}

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
  // "Metropolitan" (12 chars) must fit whole: the data task matches
  // later rows against this stored name, and a truncated copy never
  // matches the full API string (which scattered one division's rows
  // across slots and dropped the rest).
  char divisionName[4][16];   // "Atlantic", "Metropolitan", ...
  StandingsRow rows[4][8];
  uint8_t count[4];
  bool valid;
};

// Core-0 NHL data task lifecycle.
void startNhlDataTask();
