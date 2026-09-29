#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "common/comms/http_fetcher.h"
#include "config.h"
#include "nhl_client.h"
#include "nhl_state.h"

// NHL feed endpoints + JSON filters. The NHL web API (api-web.nhle.com) is
// HTTPS-only, so the two feed fetchers ride http_fetch's shared TLS
// session; ESPN news stays on its own plain-HTTP client (the api host
// serves without the 301-to-HTTPS that the www.espn.com RSS host forces).
// Endpoint semantics, payload sizes, and polling rationale:
// docs/features/nhl-api/README.md.

namespace {

const char* NHL_API_BASE = "https://api-web.nhle.com/v1";

void buildDayScoreFilter(JsonDocument& filter) {
  JsonArray g = filter["games"].to<JsonArray>();
  JsonObject o = g.add<JsonObject>();
  o["id"] = true;
  o["gameType"] = true;
  o["gameState"] = true;
  o["startTimeUTC"] = true;
  o["awayTeam"]["id"] = true;
  o["awayTeam"]["abbrev"] = true;
  o["awayTeam"]["score"] = true;
  o["homeTeam"]["id"] = true;
  o["homeTeam"]["abbrev"] = true;
  o["homeTeam"]["score"] = true;
}

void buildWeekScheduleFilter(JsonDocument& filter) {
  JsonArray week = filter["gameWeek"].to<JsonArray>();
  JsonObject day = week.add<JsonObject>();
  day["date"] = true;
  JsonArray games = day["games"].to<JsonArray>();
  JsonObject o = games.add<JsonObject>();
  o["id"] = true;
  o["gameState"] = true;
  o["startTimeUTC"] = true;
  o["awayTeam"]["id"] = true;
  o["awayTeam"]["abbrev"] = true;
  o["awayTeam"]["score"] = true;
  o["homeTeam"]["id"] = true;
  o["homeTeam"]["abbrev"] = true;
  o["homeTeam"]["score"] = true;
}

void buildLandingFilter(JsonDocument& filter) {
  filter["gameState"] = true;
  filter["periodDescriptor"]["number"] = true;
  filter["periodDescriptor"]["periodType"] = true;
  filter["clock"]["secondsRemaining"] = true;
  filter["clock"]["running"] = true;
  filter["clock"]["inIntermission"] = true;
  filter["awayTeam"]["id"] = true;
  filter["awayTeam"]["abbrev"] = true;
  filter["awayTeam"]["score"] = true;
  filter["awayTeam"]["sog"] = true;
  filter["homeTeam"]["id"] = true;
  filter["homeTeam"]["abbrev"] = true;
  filter["homeTeam"]["score"] = true;
  filter["homeTeam"]["sog"] = true;
  JsonArray groups = filter["summary"]["penalties"].to<JsonArray>();
  JsonObject grp = groups.add<JsonObject>();
  grp["periodDescriptor"]["number"] = true;
  JsonArray pens = grp["penalties"].to<JsonArray>();
  JsonObject pen = pens.add<JsonObject>();
  pen["timeInPeriod"] = true;
  pen["duration"] = true;
  pen["descKey"] = true;
  pen["committedByPlayer"]["lastName"]["default"] = true;
  pen["committedByPlayer"]["sweaterNumber"] = true;
  pen["teamAbbrev"]["default"] = true;
}

}  // namespace

bool fetchNhlDayScore(JsonDocument& doc, const char* dateStr) {
  String url = String(NHL_API_BASE) + "/score/" + dateStr;
  doc.clear();
  http_fetch::releaseBodyBuffer();

  setScheduleLastUrl("nhl_day_score");
  bumpScheduleFetchAttempt();
  int code = http_fetch::getSecure(url, 10000);
  setScheduleLastHttpCode(code);
  http_fetch::logCall("nhl_day_score", code);
  if (code != HTTP_CODE_OK) {
    setScheduleLastError(code < 0 ? "transport" : "http");
    return false;
  }
  JsonDocument filter;
  buildDayScoreFilter(filter);
  DeserializationError err = http_fetch::parseBody(doc, &filter);
  size_t gameCount = doc["games"].as<JsonArrayConst>().size();
  if (err || gameCount == 0) {
    // Bring-up diagnostics: what we asked for and what actually came back
    // (printed before the body buffer is released).
    const char* b = http_fetch::debugBody();
    size_t len = http_fetch::debugBodyLen();
    DBG_PRINTF("[NHL] date=%s parseErr=%s games=%u bodyLen=%u\n", dateStr,
               err.c_str(), (unsigned)gameCount, (unsigned)len);
    DBG_PRINTF("[NHL] head: %.130s\n", b);
    if (len > 170) DBG_PRINTF("[NHL] tail: %.160s\n", b + len - 160);
  }
  http_fetch::releaseBodyBuffer();
  if (err && !(err == DeserializationError::IncompleteInput && gameCount > 0)) {
    setScheduleLastError("parse");
    DBG_PRINTF("[NHL] day-score parse error: %s\n", err.c_str());
    return false;
  }
  if (err) {
    // Tail truncated past the filtered keys (games array's kept fields all
    // precede the skipped bulk) — the snapshot is usable as-is.
    DBG_PRINTF("[NHL] day-score tail truncated; using %u games\n",
               (unsigned)gameCount);
  }
  bumpScheduleFetchSuccess();
  setScheduleLastError("ok");
  DBG_PRINTF("[NHL] day-score parsed: %u games\n", (unsigned)gameCount);
  return true;
}

bool fetchNhlWeekSchedule(JsonDocument& doc, const char* dateStr) {
  String url = String(NHL_API_BASE) + "/schedule/" + dateStr;
  doc.clear();
  http_fetch::releaseBodyBuffer();

  setScheduleLastUrl("nhl_week_schedule");
  bumpScheduleFetchAttempt();
  int code = http_fetch::getSecure(url, 12000);
  setScheduleLastHttpCode(code);
  http_fetch::logCall("nhl_week_schedule", code);
  if (code != HTTP_CODE_OK) {
    setScheduleLastError(code < 0 ? "transport" : "http");
    return false;
  }
  JsonDocument filter;
  buildWeekScheduleFilter(filter);
  DeserializationError err = http_fetch::parseBody(doc, &filter);
  http_fetch::releaseBodyBuffer();
  size_t days = doc["gameWeek"].as<JsonArrayConst>().size();
  // Tail tolerance (same discipline as the day-score parse): the raw week
  // body is the largest payload we fetch (~85-115 KB with per-game media
  // URLs); a truncated tail past the last kept field still yields all
  // seven day objects. Anything short of a full week, or a structural
  // error, rejects the fetch so the data task falls back.
  bool tailOnly = err == DeserializationError::IncompleteInput ||
                  err == DeserializationError::InvalidInput;
  if ((!tailOnly && err) || days < 7) {
    setScheduleLastError("parse");
    DBG_PRINTF("[NHL] week-schedule parse error: %s days=%u\n", err.c_str(),
               (unsigned)days);
    return false;
  }
  if (err) {
    DBG_PRINTF("[NHL] week-schedule tail truncated; using %u days\n",
               (unsigned)days);
  }
  bumpScheduleFetchSuccess();
  setScheduleLastError("ok");
  DBG_PRINTF("[NHL] week-schedule parsed: %u days\n", (unsigned)days);
  return true;
}

bool fetchNhlStandings(JsonDocument& doc, const char* dateStr) {
  String url = String(NHL_API_BASE) + "/standings/" + dateStr;
  doc.clear();
  http_fetch::releaseBodyBuffer();

  setScheduleLastUrl("nhl_standings");
  bumpScheduleFetchAttempt();
  int code = http_fetch::getSecure(url, 10000);
  setScheduleLastHttpCode(code);
  http_fetch::logCall("nhl_standings", code);
  if (code != HTTP_CODE_OK) {
    setScheduleLastError(code < 0 ? "transport" : "http");
    return false;
  }
  JsonDocument filter;
  JsonArray rows = filter["standings"].to<JsonArray>();
  JsonObject o = rows.add<JsonObject>();
  o["teamAbbrev"]["default"] = true;
  o["divisionName"] = true;
  o["wins"] = true;
  o["losses"] = true;
  o["otLosses"] = true;
  o["points"] = true;
  o["sequence"] = true;
  DeserializationError err = http_fetch::parseBody(doc, &filter);
  http_fetch::releaseBodyBuffer();
  size_t n = doc["standings"].as<JsonArrayConst>().size();
  if (err || n == 0) {
    setScheduleLastError("parse");
    DBG_PRINTF("[NHL] standings parse error: %s rows=%u\n", err.c_str(),
               (unsigned)n);
    return false;
  }
  bumpScheduleFetchSuccess();
  setScheduleLastError("ok");
  DBG_PRINTF("[NHL] standings parsed: %u rows\n", (unsigned)n);
  return true;
}

bool fetchNhlGameLanding(JsonDocument& doc, long gameId) {
  String url = String(NHL_API_BASE) + "/gamecenter/" + String(gameId) +
               "/landing";
  doc.clear();
  http_fetch::releaseBodyBuffer();

  int code = http_fetch::getSecure(url, 8000);
  http_fetch::logCall("nhl_landing", code);
  if (code != HTTP_CODE_OK) return false;
  JsonDocument filter;
  buildLandingFilter(filter);
  DeserializationError err = http_fetch::parseBody(doc, &filter);
  http_fetch::releaseBodyBuffer();
  if (err) {
    DBG_PRINTF("[NHL] landing parse error: %s\n", err.c_str());
    return false;
  }
  return true;
}

bool fetchEspnNhlNews(JsonDocument& doc, int limit) {
  // ESPN's NHL news over plain HTTP (see the News section of the API doc).
  // Rides the shared buffered reader so chunked/truncated responses are
  // handled identically to the NHL feeds; a zero-article parse dumps the
  // body prefix so edge weirdness is visible on the diagnostic build.
  String url = "http://site.api.espn.com/apis/site/v2/sports/hockey/nhl/news?limit=";
  url += String(limit);

  doc.clear();
  http_fetch::releaseBodyBuffer();
  http_fetch::closeSession();  // fresh connection for the scheme switch

  int code = http_fetch::get(url, 8000);
  http_fetch::logCall("espn_nhl_news", code);
  if (code != HTTP_CODE_OK) return false;

  JsonDocument filter;
  filter["articles"][0]["headline"] = true;
  filter["articles"][0]["description"] = true;

  DeserializationError err = http_fetch::parseBody(doc, &filter);
  size_t articleCount = doc["articles"].as<JsonArrayConst>().size();
  if (err || articleCount == 0) {
    DBG_PRINTF("[ESPN] news oddity: parseErr=%s articles=%u bodyLen=%u "
               "head: %.130s\n", err.c_str(), (unsigned)articleCount,
               (unsigned)http_fetch::debugBodyLen(), http_fetch::debugBody());
  }
  http_fetch::releaseBodyBuffer();
  return !err && articleCount > 0;
}
