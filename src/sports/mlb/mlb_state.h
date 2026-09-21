#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "mlb_snapshot.h"

// Cross-core state for the MLB app: the three feed snapshot channels, the
// active-game selector, the renderer's schedule-JSON cache, and the
// schedule-fetch diagnostics shown on the "no upcoming games" screen.
// Writers: core-0 data task (publish*/bump/set-diagnostics) and the main
// loop (setActiveGamePk). Readers: render core (take*/get*).

// Copies the current snapshot into `out` only if a new generation has been
// published since the caller's last sample. Returns true when a fresh (and
// valid) payload was delivered.
bool takeLinescoreSnapshot(LinescoreSnapshot& out, uint32_t& lastGen);
bool takePlaySnapshot(PlaySnapshot& out, uint32_t& lastGen);
bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen);

// Core-0 publishers: push fresh snapshots to the render core.
namespace mlb_data {
void publishLinescore(const LinescoreSnapshot& s);
void publishPlay(const PlaySnapshot& s);
void publishSchedule(const ScheduleSnapshot& s);
}

// Current live-game tracker (set by the main loop; read by the data task).
// 0 means "no game followed / waiting mode".
int  getActiveGamePk();
void setActiveGamePk(int gamePk);

// Schedule JSON cache used by the renderer's waiting carousel. The data task
// on core 0 republishes this whenever a fresh schedule fetch completes.
void publishUpcomingScheduleJson(JsonObjectConst src);
JsonObjectConst getUpcomingScheduleJson();
uint32_t getUpcomingSchedulePublishedAt();   // millis() of last publish, 0 = never
size_t    getUpcomingScheduleDocSize();      // bytes in the cached document

// Diagnostic helpers for the "no upcoming games" screen. They report what
// the data task has actually published so we can tell at a glance whether
// the schedule JSON is empty, the document is fresh, etc.
uint32_t getScheduleFetchAttempts();          // # times the schedule fetch ran
uint32_t getScheduleFetchSuccesses();         // # times it returned true
void     bumpScheduleFetchAttempt();          // data task calls on each fetch entry
void     bumpScheduleFetchSuccess();          // data task calls on fetch success
const char* getScheduleLastError();           // short tag for the most recent failure
void        setScheduleLastError(const char* err);
const char* getScheduleLastUrl();             // short tag of the last URL attempted
void        setScheduleLastUrl(const char* url);
int         getScheduleLastHttpCode();        // last HTTP response code (-1 = never)
void        setScheduleLastHttpCode(int code);
uint32_t    getScheduleLastFetchAt();         // millis() of last fetch attempt
void        setScheduleLastFetchAt(uint32_t ms);
