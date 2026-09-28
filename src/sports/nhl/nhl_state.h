#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "nhl_snapshot.h"

// Cross-core state for the NHL app: the game/schedule snapshot channels,
// the followed-game id, the renderer's schedule-JSON cache, and fetch
// diagnostics for the waiting screen. Writers: core-0 data task
// (publish*/bump/set-diagnostics) and the main loop (setActiveGameId).
// Readers: render core (take*/get*).

// Copies the current snapshot into `out` only if a new generation has been
// published since the caller's last sample. Returns true when a fresh (and
// valid) payload was delivered.
bool takeGameSnapshot(GameSnapshot& out, uint32_t& lastGen);
bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen);

// Core-0 publishers: push fresh snapshots to the render core.
namespace nhl_data {
void publishGame(const GameSnapshot& s);
void publishSchedule(const ScheduleSnapshot& s);
}

// Followed-game tracker (set by the main loop; read by the data task).
// 0 means "no game followed / waiting mode".
long getActiveGameId();
void setActiveGameId(long gameId);

// Schedule JSON cache used by the renderer's waiting carousel (the raw
// day-score document, so upcoming cards can walk games/startTime fields).
void publishUpcomingScheduleJson(JsonObjectConst src);
JsonObjectConst getUpcomingScheduleJson();
uint32_t getUpcomingSchedulePublishedAt();   // millis() of last publish, 0 = never

// Diagnostic helpers for the "no upcoming games" screen.
uint32_t getScheduleFetchAttempts();
uint32_t getScheduleFetchSuccesses();
void     bumpScheduleFetchAttempt();
void     bumpScheduleFetchSuccess();
const char* getScheduleLastError();
void        setScheduleLastError(const char* err);
const char* getScheduleLastUrl();
void        setScheduleLastUrl(const char* url);
int         getScheduleLastHttpCode();
void        setScheduleLastHttpCode(int code);
uint32_t    getScheduleLastFetchAt();
void        setScheduleLastFetchAt(uint32_t ms);
