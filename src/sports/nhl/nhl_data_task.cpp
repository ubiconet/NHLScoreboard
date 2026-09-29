#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "common/comms/network_service.h"
#include "common/data/time_util.h"
#include "nhl_client.h"
#include "nhl_renderer.h"
#include "nhl_state.h"

// Core-0 NHL data task: day-score poll (60 s), followed-game landing poll
// (5 s while live), ESPN news poll (30 min cache). Everything parses into
// POD snapshots and publishes through nhl_state; nothing here touches the
// display. Cadence rationale: docs/features/nhl-api/adr/0002.

namespace {

// ---- News cache: refresh at most every NHL_NEWS_CACHE_TTL_MS ----
uint32_t gLastNewsFetchAt   = 0;
uint32_t gLastNewsAttemptAt = 0;
uint32_t gNewsRetryInterval = NHL_NEWS_RETRY_MS;
NewsStory gCachedNews[MAX_NEWS_STORIES];

// ---- Schedule / landing pacing ----
uint32_t gLastScheduleAt = 0;
bool     gScheduleValid  = false;
uint32_t gLastLandingAt  = 0;

// ---- Week look-ahead cache (/v1/schedule, Mon-Sun) ----
JsonDocument gWeekDoc;       // filtered slate: gameWeek[] {date, games[]}
char     gWeekDate[11] = ""; // Monday the cache covers; "" = no cache
uint32_t gWeekFetchAt = 0;   // last refresh ATTEMPT (success or failure)

int periodLength(int period) { return period <= 3 ? 1200 : 300; }

int parseMmSs(const char* s) {
  int mm = 0, ss = 0;
  if (s == nullptr) return 0;
  sscanf(s, "%d:%d", &mm, &ss);
  return mm * 60 + ss;
}

// Game clock elapsed (seconds since puck drop) from the countdown clock:
// "period + seconds remaining in that period".
int gameElapsedFromClock(int period, int secRemaining) {
  int e = 0;
  for (int p = 1; p < period; ++p) e += periodLength(p);
  return e + (periodLength(period) - secRemaining);
}

// Game clock elapsed at an event's "timeInPeriod" stamp. Verified against
// play-by-play ground truth (timeInPeriod 00:45 <-> timeRemaining 19:15):
// the summary stamps ELAPSED time into the period, not the countdown.
int gameElapsedAtStamp(int period, const char* timeInPeriod) {
  int e = 0;
  for (int p = 1; p < period; ++p) e += periodLength(p);
  return e + parseMmSs(timeInPeriod);
}

GameSnapshot landingToSnapshot(JsonObjectConst d, long gameId) {
  GameSnapshot s{};
  s.valid = true;
  s.gameId = gameId;
  strlcpy(s.gameState, d["gameState"] | "?", sizeof(s.gameState));
  s.period = d["periodDescriptor"]["number"] | 0;
  strlcpy(s.periodType, d["periodDescriptor"]["periodType"] | "REG",
          sizeof(s.periodType));
  s.clockSec = d["clock"]["secondsRemaining"] | 0;
  s.clockRunning = d["clock"]["running"] | false;
  s.inIntermission = d["clock"]["inIntermission"] | false;
  s.awayTeamId = d["awayTeam"]["id"] | 0;
  s.homeTeamId = d["homeTeam"]["id"] | 0;
  strlcpy(s.awayAbbrev, d["awayTeam"]["abbrev"] | "??", sizeof(s.awayAbbrev));
  strlcpy(s.homeAbbrev, d["homeTeam"]["abbrev"] | "??", sizeof(s.homeAbbrev));
  s.awayScore = d["awayTeam"]["score"] | 0;
  s.homeScore = d["homeTeam"]["score"] | 0;
  s.awaySog = d["awayTeam"]["sog"] | 0;
  s.homeSog = d["homeTeam"]["sog"] | 0;

  // Active penalties: the summary lists every penalty with its start
  // (period + elapsed stamp) and duration; a penalty is active while
  // start + duration > now on the game clock.
  int now = gameElapsedFromClock(s.period > 0 ? s.period : 1, s.clockSec);
  int cnt = 0;
  for (JsonObjectConst grp : d["summary"]["penalties"].as<JsonArrayConst>()) {
    int pPeriod = grp["periodDescriptor"]["number"] | 0;
    for (JsonObjectConst pen : grp["penalties"].as<JsonArrayConst>()) {
      int start = gameElapsedAtStamp(pPeriod > 0 ? pPeriod : 1,
                                     pen["timeInPeriod"] | "0:00");
      int dur = pen["duration"] | 2;
      int remain = start + dur * 60 - now;
      if (remain <= 0) continue;
      const char* team = pen["teamAbbrev"]["default"] | "";
      if (strcmp(team, s.homeAbbrev) == 0) ++s.homePenaltyCount;
      else ++s.awayPenaltyCount;
      if (cnt < 4) {
        NhlPenalty& p = s.penalties[cnt++];
        strlcpy(p.teamAbbrev, team, sizeof(p.teamAbbrev));
        strlcpy(p.lastName,
                pen["committedByPlayer"]["lastName"]["default"] | "",
                sizeof(p.lastName));
        p.number = pen["committedByPlayer"]["sweaterNumber"] | 0;
        strlcpy(p.desc, pen["descKey"] | "penalty", sizeof(p.desc));
        p.remainSec = remain;
        p.durMin = dur;
      }
    }
  }
  s.penaltyCount = cnt;
  return s;
}

void fetchNewsIfDue(bool online) {
  uint32_t now = millis();
  bool fresh = gLastNewsFetchAt != 0 &&
               (now - gLastNewsFetchAt) < NHL_NEWS_CACHE_TTL_MS;
  bool throttled = gLastNewsFetchAt == 0 && gLastNewsAttemptAt != 0 &&
                   (now - gLastNewsAttemptAt) < gNewsRetryInterval;
  if (!online || fresh || throttled || portalEngaged()) return;
  gLastNewsAttemptAt = now;

  JsonDocument doc;
  if (!fetchEspnNhlNews(doc)) {
    gNewsRetryInterval = min<uint32_t>(gNewsRetryInterval * 2, 300000UL);
    return;
  }
  gNewsRetryInterval = NHL_NEWS_RETRY_MS;
  gLastNewsFetchAt = now;

  size_t n = 0;
  for (JsonObjectConst a : doc["articles"].as<JsonArrayConst>()) {
    if (n >= MAX_NEWS_STORIES) break;
    const char* h = a["headline"] | "";
    const char* d2 = a["description"] | "";
    if (h[0] == '\0') continue;
    strlcpy(gCachedNews[n].headline, h, sizeof(gCachedNews[n].headline));
    strlcpy(gCachedNews[n].description, d2, sizeof(gCachedNews[n].description));
    NewsSlotUpdate slot{};
    slot.index = n;
    strlcpy(slot.headline, h, sizeof(slot.headline));
    strlcpy(slot.description, d2, sizeof(slot.description));
    publishNewsStory(slot);
    ++n;
  }
  setNewsStoryCount(n);
  DBG_PRINTF("[ESPN] applied %u news stories\n", (unsigned)n);
}

void fetchScheduleIfDue(bool online) {
  if (!online || portalEngaged()) return;
  if (!timeIsSynced()) return;  // no trustworthy local date yet
  uint32_t now = millis();
  uint32_t interval = gScheduleValid ? NHL_SCHEDULE_POLL_INTERVAL_MS
                                     : NHL_SCHEDULE_RETRY_MS;
  if (gLastScheduleAt != 0 && (now - gLastScheduleAt) < interval) return;
  gLastScheduleAt = now;

  char date[11];
  time_t t = time(nullptr);
  tm lt = {};
  localtime_r(&t, &lt);
  if (strftime(date, sizeof(date), "%Y-%m-%d", &lt) == 0) return;

  JsonDocument doc;
  if (!fetchNhlDayScore(doc, date)) {
    gScheduleValid = false;
    return;
  }
  gScheduleValid = true;

  ScheduleSnapshot snap{};
  size_t n = 0;
  auto appendGames = [&](JsonObjectConst src) {
    for (JsonObjectConst g : src["games"].as<JsonArrayConst>()) {
      if (n >= 26) break;
      long id = g["id"] | 0;
      bool dup = false;
      for (size_t k = 0; k < n; ++k)
        if (snap.games[k].gameId == id) { dup = true; break; }
      if (dup) continue;
      NhlDayGame& o = snap.games[n++];
      o.gameId = id;
      strlcpy(o.awayAbbrev, g["awayTeam"]["abbrev"] | "??", sizeof(o.awayAbbrev));
      strlcpy(o.homeAbbrev, g["homeTeam"]["abbrev"] | "??", sizeof(o.homeAbbrev));
      o.awayTeamId = g["awayTeam"]["id"] | 0;
      o.homeTeamId = g["homeTeam"]["id"] | 0;
      o.awayScore = g["awayTeam"]["score"] | 0;
      o.homeScore = g["homeTeam"]["score"] | 0;
      strlcpy(o.gameState, g["gameState"] | "?", sizeof(o.gameState));
      strlcpy(o.startUtc, g["startTimeUTC"] | "", sizeof(o.startUtc));
    }
  };
  appendGames(doc.as<JsonObjectConst>());

  // Upcoming-games look-ahead: one /v1/schedule call covers the Mon-Sun
  // week containing today. The cache only refreshes on TTL (6 h) or when
  // the week rolls over — future-day states never change, and today's
  // half of the upcoming doc below always comes from the fresh day-score
  // fetch above. Failed refreshes retry on their own spacing.
  {
    time_t monday = t - ((lt.tm_wday + 6) % 7) * 24 * 60 * 60;
    tm md = {};
    localtime_r(&monday, &md);
    char mondayStr[11];
    if (strftime(mondayStr, sizeof(mondayStr), "%Y-%m-%d", &md) > 0) {
      uint32_t sinceFetch = millis() - gWeekFetchAt;
      bool need = strcmp(gWeekDate, mondayStr) != 0 ||
                  sinceFetch > NHL_WEEK_SCHEDULE_TTL_MS;
      bool mayTry = gWeekFetchAt == 0 ||
                    sinceFetch >= NHL_WEEK_SCHEDULE_RETRY_MS;
      if (need && mayTry) {
        gWeekFetchAt = millis();
        if (fetchNhlWeekSchedule(gWeekDoc, date)) {
          strlcpy(gWeekDate,
                  gWeekDoc["gameWeek"][0]["date"] | mondayStr,
                  sizeof(gWeekDate));
        } else {
          gWeekDate[0] = '\0';
          gWeekDoc.clear();
        }
      }
    }
  }
  bool weekValid = gWeekDate[0] != '\0';

  // The upcoming-games cache the waiting screen walks: today's fresh
  // games, then every later day of the cached week — so the carousel's
  // per-team cards and the upcoming list span a full seven days.
  JsonDocument upcomingDoc;
  JsonArray merged = upcomingDoc["games"].to<JsonArray>();
  for (JsonObjectConst g : doc["games"].as<JsonArrayConst>()) merged.add(g);
  if (weekValid) {
    for (JsonObjectConst day : gWeekDoc["gameWeek"].as<JsonArrayConst>()) {
      if (strcmp(day["date"] | "", date) <= 0) continue;  // today merged fresh
      appendGames(day);  // snapshot/ticker look-ahead (dedup + 26 cap)
      for (JsonObjectConst g : day["games"].as<JsonArrayConst>()) {
        merged.add(g);
      }
    }
  } else {
    // Week fetch unavailable (fresh boot in a bad TLS window): fall back
    // to the old one-day look-ahead so tomorrow's card still shows.
    time_t tmrw = t + 24 * 60 * 60;
    tm td = {};
    localtime_r(&tmrw, &td);
    char tdate[11];
    if (strftime(tdate, sizeof(tdate), "%Y-%m-%d", &td) > 0) {
      JsonDocument tdoc;
      if (fetchNhlDayScore(tdoc, tdate)) {
        appendGames(tdoc.as<JsonObjectConst>());
        for (JsonObjectConst g : tdoc["games"].as<JsonArrayConst>()) {
          merged.add(g);
        }
      }
    }
    if (n == 0) {
      const char* nextDate = doc["nextDate"] | "";
      if (nextDate[0] != '\0') {
        JsonDocument ndoc;
        if (fetchNhlDayScore(ndoc, nextDate)) {
          appendGames(ndoc.as<JsonObjectConst>());
          for (JsonObjectConst g : ndoc["games"].as<JsonArrayConst>()) {
            merged.add(g);
          }
        }
      }
    }
  }

  // After local midnight the API's "today" no longer contains the previous
  // date's late games — some still in progress. Merge yesterday's slate
  // during the overlap window so live selection never drops a game (and a
  // followed game stays visible through its postgame grace).
  if (lt.tm_hour < 8) {
    time_t yst = t - 24 * 60 * 60;
    tm yd = {};
    localtime_r(&yst, &yd);
    char ydate[11];
    if (strftime(ydate, sizeof(ydate), "%Y-%m-%d", &yd) > 0) {
      JsonDocument ydoc;
      if (fetchNhlDayScore(ydoc, ydate)) appendGames(ydoc.as<JsonObjectConst>());
    }
  }

  snap.count = n;
  snap.valid = true;
  nhl_data::publishSchedule(snap);
  publishUpcomingScheduleJson(upcomingDoc.as<JsonObjectConst>());
}

void fetchLandingIfDue(bool online) {
  long id = getActiveGameId();
  if (id <= 0 || !online || portalEngaged()) return;
  uint32_t now = millis();
  if (gLastLandingAt != 0 && (now - gLastLandingAt) < NHL_LIVE_POLL_INTERVAL_MS) {
    return;
  }
  gLastLandingAt = now;

  JsonDocument doc;
  if (!fetchNhlGameLanding(doc, id)) return;
  nhl_data::publishGame(landingToSnapshot(doc.as<JsonObjectConst>(), id));
}

void nhlDataTaskLoop(void*) {
  while (true) {
    bool online = isOnline();
    fetchScheduleIfDue(online);
    fetchLandingIfDue(online);
    fetchNewsIfDue(online);
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

void startNhlDataTask() {
  xTaskCreatePinnedToCore(nhlDataTaskLoop, "nhl_data", 12288, nullptr, 1,
                          nullptr, 0);
}
