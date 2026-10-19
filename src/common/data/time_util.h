#pragma once

#include <Arduino.h>
#include <time.h>

// Time helpers shared across sports: NTP readiness, compile-date fallback,
// and ISO-8601 UTC date parsing (statsapi-style feeds emit gameDate as
// "YYYY-MM-DDTHH:MM:SSZ").

// NTP epoch convention used across the codebase: any clock below
// 2024-01-01 UTC has not been synced yet.
bool timeIsSynced();

// Parse the firmware's compile date string ("Mmm DD YYYY" e.g. "Sep 17
// 2026") into a struct tm. Used as a schedule-window fallback when NTP
// hasn't synced on a fresh boot.
bool parseCompileDate(const char* compileDate, tm& out);

// Parse "YYYY-MM-DDTHH:MM" (UTC) into a Unix epoch, correct across DST
// transitions of the local timezone.
bool isoDateToEpoch(const char* isoDate, time_t& outEpoch);

// Coarse clock bootstrap from an HTTP `Date` response header ("Mon, 19 Oct
// 2026 15:04:05 GMT"). Sets the system clock ONLY while it is still
// unsynced (see timeIsSynced), so an install whose NTP is blocked or slow
// still converges to a date-correct clock from any successful feed fetch;
// a later NTP sync overwrites it with the precise time. No-op once the
// clock is real.
void applyCoarseHttpDate(const char* httpDate);
