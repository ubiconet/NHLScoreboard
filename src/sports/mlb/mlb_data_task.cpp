#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "config.h"
#include "common/comms/http_fetcher.h"
#include "common/data/time_util.h"
#include "common/comms/network_service.h"
#include "common/comms/ota_update.h"
#include "mlb_client.h"
#include "mlb_snapshot.h"
#include "mlb_state.h"
#include "mlb_renderer.h"

namespace {

// The publisher functions live at global scope in scoreboard.cpp (namespace
// mlb_data::). They're declared in mlb_state.h
// via the take* helpers, and we just call them by qualified name here.

// JSON workspace on core 0. Each API routine clears it before beginning a
// subsequent HTTPS request, so a previous response does not crowd TLS buffers.
JsonDocument mlbDoc;

// News cache: only refresh every MLB_NEWS_CACHE_TTL_MS so the scoreboard
// doesn't hammer ESPN's news endpoint on every waiting-mode cycle. Stories are
// published into the renderer-owned newsStories[] slots declared in mlb_renderer.h.
uint32_t gLastNewsFetchAt    = 0;
uint32_t gLastNewsAttemptAt  = 0;  // most recent attempt (success or fail) for failed-retry throttle
uint32_t gLastNewsAppliedAt  = 0;
uint32_t gNewsRetryInterval  = MLB_NEWS_RETRY_MS;  // doubles per consecutive failure
NewsStory gCachedNews[MAX_NEWS_STORIES];

const char* inningStateCode(const char* state) {
  if (strcmp(state, "Top") == 0)    return "Top";
  if (strcmp(state, "Bottom") == 0) return "Bot";
  if (strcmp(state, "Middle") == 0) return "Mid";
  if (strcmp(state, "End") == 0)    return "End";
  return "";
}

uint8_t classifyInningState(const char* state) {
  if (strcmp(state, "Top") == 0)    return 0;
  if (strcmp(state, "Bottom") == 0) return 1;
  if (strcmp(state, "Middle") == 0) return 2;
  if (strcmp(state, "End") == 0)    return 3;
  return 4;
}

void copyLinescoreSnapshot(int gamePk, JsonObjectConst src, LinescoreSnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  dst.gamePk         = gamePk;
  dst.awayScore      = src["teams"]["away"]["runs"]      | 0;
  dst.homeScore      = src["teams"]["home"]["runs"]      | 0;
  dst.balls          = src["balls"]                      | 0;
  dst.strikes        = src["strikes"]                    | 0;
  dst.outs           = src["outs"]                       | 0;
  dst.currentInning  = src["currentInning"]              | 0;
  const char* innState = src["inningState"] | "";
  dst.inningState    = classifyInningState(innState);
  dst.isTopInning    = (src["isTopInning"] | (strcmp(innState, "Top") == 0)) ? true : false;
  dst.offenseFirst   = !src["offense"]["first"].isNull();
  dst.offenseSecond  = !src["offense"]["second"].isNull();
  dst.offenseThird   = !src["offense"]["third"].isNull();
  dst.offenseTeamId  = src["offense"]["team"]["id"] | 0;
  dst.defenseTeamId  = src["defense"]["team"]["id"] | 0;
  // teams.away.team.id / teams.home.team.id are the most reliable ids
  // available from the linescore endpoint; offense/defense.team.id are
  // fallbacks if the teams block didn't include them.
  dst.awayTeamId     = src["teams"]["away"]["team"]["id"] | 0;
  dst.homeTeamId     = src["teams"]["home"]["team"]["id"] | 0;
  if (dst.awayTeamId == 0) {
    dst.awayTeamId = dst.isTopInning ? dst.offenseTeamId : dst.defenseTeamId;
  }
  if (dst.homeTeamId == 0) {
    dst.homeTeamId = dst.isTopInning ? dst.defenseTeamId : dst.offenseTeamId;
  }

  strlcpy(dst.batter,
          src["offense"]["batter"]["fullName"] | "",
          sizeof(dst.batter));
  strlcpy(dst.pitcher,
          src["defense"]["pitcher"]["fullName"] | "",
          sizeof(dst.pitcher));
  strlcpy(dst.onDeck,
          src["offense"]["onDeck"]["fullName"] | "",
          sizeof(dst.onDeck));
  strlcpy(dst.inHole,
          src["offense"]["inHole"]["fullName"] | "",
          sizeof(dst.inHole));
  strlcpy(dst.catcher,
          src["defense"]["catcher"]["fullName"] | "",
          sizeof(dst.catcher));
  dst.valid = true;
}

void copyPlaySnapshot(JsonObjectConst src, PlaySnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  JsonArrayConst plays = src["allPlays"].as<JsonArrayConst>();
  if (plays.isNull() || plays.size() == 0) {
    dst.valid = false;
    return;
  }
  // allPlays ends with the IN-PROGRESS at-bat (empty result fields, new
  // batter). Snapshot the latest COMPLETED play instead: its atBatIndex
  // only advances when a result actually exists, which is what the
  // renderer's card trigger keys on — the in-progress entry used to
  // consume the index early and suppress (or blank) the result card.
  JsonObjectConst latest;
  for (size_t i = plays.size(); i-- > 0;) {
    JsonObjectConst play = plays[i];
    if (play["about"]["isComplete"] | false) {
      latest = play;
      break;
    }
  }
  if (latest.isNull()) {
    dst.valid = false;  // game just started, nothing completed yet
    return;
  }
  dst.atBatIndex  = latest["about"]["atBatIndex"] | -1;
  dst.batterId    = latest["matchup"]["batter"]["id"] | 0;
  strlcpy(dst.batterName,
          latest["matchup"]["batter"]["fullName"] | "",
          sizeof(dst.batterName));
  strlcpy(dst.event,
          latest["result"]["event"] | "",
          sizeof(dst.event));
  strlcpy(dst.description,
          latest["result"]["description"] | "",
          sizeof(dst.description));
  dst.valid = true;
}

void copyOtherGames(JsonObjectConst scheduleRoot, ScheduleSnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  JsonArrayConst dates = scheduleRoot["dates"].as<JsonArrayConst>();
  if (dates.isNull() || dates.size() == 0) return;
  JsonArrayConst games = dates[0]["games"].as<JsonArrayConst>();
  if (games.isNull()) return;

  for (JsonObjectConst game : games) {
    if (dst.otherCount >= 9) break;
    const char* state = game["status"]["abstractGameState"] | "";
    if (strcmp(state, "Live") != 0) continue;
    OtherGameLite& slot = dst.others[dst.otherCount++];
    slot.gamePk      = game["gamePk"] | 0;
    int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
    int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
    // Store team-id digits in the abbrev fields as a placeholder; the
    // renderer's updateOtherGames() runs getTeamAbbrev() to resolve the
    // canonical abbreviation at read time.
    snprintf(slot.awayAbbrev, sizeof(slot.awayAbbrev), "%d", awayTeamId);
    snprintf(slot.homeAbbrev, sizeof(slot.homeAbbrev), "%d", homeTeamId);
    slot.awayScore = game["teams"]["away"]["score"] | 0;
    slot.homeScore = game["teams"]["home"]["score"] | 0;
    strlcpy(slot.inningState,
            inningStateCode(game["linescore"]["inningState"] | ""),
            sizeof(slot.inningState));
    strlcpy(slot.inningOrdinal,
            game["linescore"]["currentInningOrdinal"] | "",
            sizeof(slot.inningOrdinal));
  }
  dst.valid = true;
}

bool fetchLinescore(int gamePk, LinescoreSnapshot& out) {
  if (!fetchMlbLinescore(gamePk, mlbDoc)) return false;
  copyLinescoreSnapshot(gamePk, mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();
  return out.valid;
}

bool fetchLatestPlay(int gamePk, PlaySnapshot& out) {
  if (!fetchMlbLatestPlay(gamePk, mlbDoc)) return false;
  copyPlaySnapshot(mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();
  return out.valid;
}

bool fetchScheduleSnapshot(ScheduleSnapshot& out) {
  // Bump the attempt counter immediately so the renderer can see the data
  // task is alive even when every fetch fails.
  bumpScheduleFetchAttempt();
  // Build a 3-day forward window so findNextGame() can locate the *next*
  // game for each preferred team, even when none of them play today. When
  // NTP hasn't synced yet (epoch < 2024-01-01), fall back to the firmware's
  // __DATE__ as the start of the window so the carousel still has games on a
  // fresh boot.
  char startDate[11] = {};
  char endDate[11]   = {};
  time_t nowEpoch = time(nullptr);
  bool ntpReady = timeIsSynced();

  const int SCHEDULE_LOOKAHEAD_DAYS = 3;

  tm startTm = {};
  if (ntpReady) {
    localtime_r(&nowEpoch, &startTm);
  } else if (!parseCompileDate(__DATE__, startTm)) {
    setScheduleLastError("date_parse_failed");
    return false;
  }
  tm endTm = startTm;
  endTm.tm_mday += SCHEDULE_LOOKAHEAD_DAYS - 1;
  mktime(&endTm);
  if (strftime(startDate, sizeof(startDate), "%Y-%m-%d", &startTm) == 0 ||
      strftime(endDate,   sizeof(endDate),   "%Y-%m-%d", &endTm)   == 0) {
    setScheduleLastError("strftime_failed");
    return false;
  }

  int preferredTeams[3];
  getPreferredTeamIds(preferredTeams);

  // Fetch 1: today's full slate (a few KB with hydrate=linescore) for the
  // other-live-games ticker and live-game detection.
  if (!fetchMlbSchedule(nullptr, mlbDoc)) {
    setScheduleLastError("today_fetch_failed");
    return false;
  }
  copyOtherGames(mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();

  // Fetch 2: 3-day window for just the preferred teams (~2 KB with the
  // server-side teamId filter) for the renderer's upcoming-games carousel.
  // A 3-day full-slate body (~13 KB) cannot be buffered while the TLS
  // session is open on this heap, which is why the two are separate calls.
  bool rangeOk =
      fetchMlbScheduleRange(startDate, endDate, mlbDoc, preferredTeams);
  if (rangeOk) {
    publishUpcomingScheduleJson(mlbDoc.as<JsonObjectConst>());
    mlbDoc.clear();
  } else {
    setScheduleLastError("range_fetch_failed");
  }

  bumpScheduleFetchSuccess();
  if (rangeOk) {
    setScheduleLastError("ok");
  }
  DBG_PRINTF("[SCHED] fetched OK on attempt %u (range %s)\n",
             (unsigned)getScheduleFetchSuccesses(),
             rangeOk ? "ok" : "failed");
  return out.valid;
}

// News fetch is throttled: only hit ESPN every MLB_NEWS_CACHE_TTL_MS once we
// have a successful fetch in cache, but a cold cache (first boot) fetches
// immediately so the carousel isn't empty. We throttle *failed* retries to
// MLB_NEWS_RETRY_MS (default 15 s) so a fresh boot that's still associating
// with WiFi keeps retrying every 15 s instead of locking itself out for the
// full 30-min cache TTL.
// (An earlier revision had a "force refresh" flag here that the data task set
// itself after a successful fetch and consumed on the next 50 ms tick,
// re-fetching ESPN in a tight loop forever. The renderer is updated directly
// through publishNewsStory()/setNewsStoryCount(), so no flag is needed.)
void fetchNewsSnapshotIfDue() {
  uint32_t now = millis();
  bool fresh       = (gLastNewsFetchAt != 0 &&
                      (now - gLastNewsFetchAt) < MLB_NEWS_CACHE_TTL_MS);
  bool retryThrottled = (gLastNewsFetchAt == 0 &&
                         gLastNewsAttemptAt != 0 &&
                         (now - gLastNewsAttemptAt) < gNewsRetryInterval);
  if (fresh || retryThrottled) {
    return;
  }
  gLastNewsAttemptAt = now;
  if (!fetchEspnMlbNews(mlbDoc)) {
    // Exponential backoff on consecutive failures: retrying every few
    // seconds against a network that is refusing TLS connections makes
    // flood protection worse, never better.
    gNewsRetryInterval = (gNewsRetryInterval * 2 > MLB_RETRY_BACKOFF_MAX_MS)
                             ? MLB_RETRY_BACKOFF_MAX_MS
                             : gNewsRetryInterval * 2;
    DBG_PRINTF("[NEWS] fetch failed; serving cached (%u stories); retry in %lu s\n",
               (unsigned)getNewsStoryCount(),
               (unsigned long)(gNewsRetryInterval / 1000UL));
    return;
  }
  gNewsRetryInterval = MLB_NEWS_RETRY_MS;
  gLastNewsFetchAt = now;
  size_t count = 0;
  for (JsonObjectConst article : mlbDoc["articles"].as<JsonArrayConst>()) {
    if (count >= MAX_NEWS_STORIES) break;
    const char* h = article["headline"] | "";
    if (h[0] == '\0') continue;
    strlcpy(gCachedNews[count].headline, h, sizeof(gCachedNews[count].headline));
    strlcpy(gCachedNews[count].description,
            article["description"] | "",
            sizeof(gCachedNews[count].description));
    count++;
  }
  if (count == 0) {
    mlbDoc.clear();
    return;
  }
  // Publish into the renderer-owned slots via the setter API.
  for (size_t i = 0; i < count; ++i) {
    NewsSlotUpdate slot;
    slot.index = i;
    strlcpy(slot.headline,    gCachedNews[i].headline,    sizeof(slot.headline));
    strlcpy(slot.description, gCachedNews[i].description, sizeof(slot.description));
    publishNewsStory(slot);
  }
  setNewsStoryCount(count);  // also resets newsStoryIndex to 0
  gLastNewsAppliedAt = now;
  mlbDoc.clear();
  DBG_PRINTF("[NEWS] cache refreshed: %u stories; next refresh in %lu min\n",
             (unsigned)count,
             (unsigned long)(MLB_NEWS_CACHE_TTL_MS / 60000UL));
}

void mlbDataTaskLoop(void*) {
  // Schedule goes first in each tick: it is the primary display feed, and at
  // boot we want it to win the race for the first (often only) healthy TLS
  // connection window.
  static uint32_t lastScheduleAt = 0;
  static bool lastScheduleOk = true;
  static uint32_t scheduleRetryInterval = MLB_SCHEDULE_RETRY_MS;

  for (;;) {
    // Yield to the network task. We only do work when online.
    static uint32_t sOnlineSince = 0;
    if (isOnline()) {
      if (sOnlineSince == 0) sOnlineSince = millis();
    } else {
      sOnlineSince = 0;
    }
    if (sOnlineSince != 0) {
      uint32_t onlineFor = millis() - sOnlineSince;
      // Firmware self-update check (manifest + OTA download). While a
      // download is running, hold off the feed fetches entirely: the TLS
      // download needs the heap and airtime to itself.
      if (otaCheckRequested()) {
        // Free the JSON pool and response buffer so a portal-triggered
        // mid-session TLS check gets the largest contiguous heap we can
        // offer (see the note at the top of ota_update.cpp).
        mlbDoc.clear();
        mlbDoc.shrinkToFit();
        http_fetch::releaseBodyBuffer();
      }
      serviceOtaUpdates(onlineFor);
      if (otaUpdateInProgress()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      // Portal priority mode: someone is actively using the setup pages —
      // pause all feed activity (and drop the statsapi session) so the web
      // server gets the core and the radio airtime to itself. Cached data
      // keeps the display going; feeds resume when the portal goes idle.
      if (portalEngaged()) {
        http_fetch::closeSession();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      // The first OTA check runs before any feed fetch: its TLS handshake
      // needs the pristine boot heap (see otaBootGateReached).
      if (!otaBootGateReached(onlineFor)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      uint32_t now = millis();

      // News: cache TTL-gated, with exponential failure backoff. Runs FIRST
      // so the boot-time attempt lands while the heap is pristine (ESPN's
      // cert chain needs larger allocations than statsapi's) and before the
      // statsapi session is opened; fetchEspnMlbNews closes that session
      // itself so the two TLS contexts never coexist on this heap.
      fetchNewsSnapshotIfDue();
      // Schedule: refresh ~once a minute after a successful fetch. While the
      // last fetch failed, retry after scheduleRetryInterval, which doubles
      // per consecutive failure (capped) — hammering a network that is
      // refusing TLS connections only prolongs the lockout.
      bool firstEver = (lastScheduleAt == 0);
      bool due       = (now - lastScheduleAt >= MLB_SCHEDULE_POLL_INTERVAL_MS);
      bool retryDue  = (!lastScheduleOk &&
                        now - lastScheduleAt >= scheduleRetryInterval);
      if (firstEver || due || retryDue) {
        lastScheduleAt = now;
        ScheduleSnapshot sch;
        if (fetchScheduleSnapshot(sch)) {
          mlb_data::publishSchedule(sch);
        }
        // Judge success at the fetch level (lastError == "ok"), not by
        // out.valid — a day with no games parses fine but leaves no live
        // games to publish, and that shouldn't trigger fast retries.
        lastScheduleOk = (strcmp(getScheduleLastError(), "ok") == 0);
        if (lastScheduleOk) {
          scheduleRetryInterval = MLB_SCHEDULE_RETRY_MS;
        } else {
          scheduleRetryInterval =
              (scheduleRetryInterval * 2 > MLB_RETRY_BACKOFF_MAX_MS)
                  ? MLB_RETRY_BACKOFF_MAX_MS
                  : scheduleRetryInterval * 2;
        }
      }

      int activeGamePk = getActiveGamePk();
      static uint32_t lastLivePollAt = 0;
      if (activeGamePk > 0 && now - lastLivePollAt >= MLB_LIVE_POLL_INTERVAL_MS) {
        lastLivePollAt = now;
        LinescoreSnapshot ls;
        if (fetchLinescore(activeGamePk, ls)) {
          mlb_data::publishLinescore(ls);
        }
        PlaySnapshot play;
        if (fetchLatestPlay(activeGamePk, play)) {
          mlb_data::publishPlay(play);
        }
      }

      // NOTE: the statsapi keep-alive session is deliberately left OPEN
      // between ticks. The install's network path refuses new TCP flows
      // from this device after a burst of connections, so reusing one
      // session across refreshes (and across the 5 s live polls during a
      // game) is what keeps the feeds alive. fetchEspnMlbNews closes it
      // before its own connection; the next statsapi fetch reopens it.
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

} // namespace

void startMlbDataTask() {
  // Don't pre-prime news here — at this point WiFi is still associating
  // and the synchronous fetch returns HTTP -1 immediately, then the 30-min
  // cache TTL locks out retries for the rest of the session. The spawned
  // loop below will naturally retry news every ~30 s until the first
  // success, so the carousel gets populated as soon as WiFi is up.
  xTaskCreatePinnedToCore(mlbDataTaskLoop, "MlbData", 12288,
                          nullptr, 1, nullptr, 0);
}