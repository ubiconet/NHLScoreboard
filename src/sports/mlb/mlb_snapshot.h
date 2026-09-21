#pragma once

#include <Arduino.h>

// Shared "snapshot" structs written by the core-0 MLB data task and read by
// the core-1 render loop. All fields are plain POD so the writer never has to
// hold a lock while reading; the render loop reads them atomically (single
// aligned struct copy). A small generation counter signals when a new payload
// has been published since the last read.
//
// This replaces the old pattern where every MLB API call (linescore,
// playByPlay, schedule, news) blocked inside loop() with 3-5s HTTPS timeouts.
// Offloading them to core 0 means core 1's render loop only ever touches the
// TFT/LEDs/MAX7219 hardware, so frame timing is no longer hostage to network
// latency.

struct LinescoreSnapshot {
  int gamePk;
  int awayScore;
  int homeScore;
  int balls;
  int strikes;
  int outs;
  int currentInning;
  uint8_t inningState; // 0=Top, 1=Bottom, 2=Middle, 3=End, 4=other
  bool isTopInning;
  bool offenseFirst;
  bool offenseSecond;
  bool offenseThird;
  int offenseTeamId;
  int defenseTeamId;
  int awayTeamId;   // resolved home/away ids (may be copied from offense/defense)
  int homeTeamId;
  char batter[32];
  char pitcher[32];
  char onDeck[32];
  char inHole[32];
  char catcher[32];
  bool valid;
};

struct PlaySnapshot {
  int atBatIndex;
  int batterId;
  char batterName[32];
  char event[24];       // short outcome: "Single", "Strikeout", "Walk"...
  char description[160]; // full play-by-play sentence
  bool valid;
};

struct OtherGameLite {
  int gamePk;
  char awayAbbrev[8];
  char homeAbbrev[8];
  int awayScore;
  int homeScore;
  char inningState[8];
  char inningOrdinal[8];
};

struct ScheduleSnapshot {
  OtherGameLite others[9];
  size_t otherCount;
  bool valid;
};

// All snapshot accessors live in scoreboard.cpp / main.cpp to keep this header
// allocation-free (no JsonDocument on the render path).

// Core-0 MLB data task lifecycle.
void startMlbDataTask();

// Publishers used by the core-0 data task to push fresh snapshots to the
// render core. Defined in scoreboard.cpp at file scope.
namespace mlb_data {
void publishLinescore(const LinescoreSnapshot& s);
void publishPlay(const PlaySnapshot& s);
void publishSchedule(const ScheduleSnapshot& s);
}