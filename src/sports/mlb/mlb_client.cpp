#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "common/comms/http_fetcher.h"
#include "mlb_client.h"
#include "mlb_state.h"

// Feed endpoints + JSON filters for the MLB build. Transport (shared
// keep-alive session, buffered body reader, filtered parse) lives in
// common/comms/http_fetcher; the plain-HTTP rationale is documented there.
namespace {

// ArduinoJson filter mask for the schedule endpoint's hydrate=linescore
// payload: keeps the per-game team ids/names/scores plus the inning state
// the other-live-games ticker needs, dropping everything else.
void buildScheduleFilter(JsonDocument& filter) {
  filter["dates"][0]["date"] = true;
  filter["dates"][0]["games"][0]["gamePk"] = true;
  filter["dates"][0]["games"][0]["gameDate"] = true;
  filter["dates"][0]["games"][0]["status"]["abstractGameState"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["team"]["id"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["team"]["name"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["score"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["team"]["id"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["team"]["name"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["score"] = true;
  filter["dates"][0]["games"][0]["linescore"]["inningState"] = true;
  filter["dates"][0]["games"][0]["linescore"]["currentInningOrdinal"] = true;
}
} // namespace

bool fetchMlbSchedule(const char* dateStr, JsonDocument& doc) {
  // A null/empty date asks the server for today's slate (US game-day), so the
  // device never needs its own clock. hydrate+fields pulls per-game inning
  // state for the other-live-games ticker while keeping the payload ~3 KB.
  String url = "http://statsapi.mlb.com/api/v1/schedule?sportId=1"
               "&hydrate=linescore"
               "&fields=dates,date,games,gamePk,status,abstractGameState,detailedState,"
               "gameDate,"
               "linescore,currentInningOrdinal,inningState,isTopInning,"
               "teams,away,team,id,name,score,home,team,id,name,score";
  if (dateStr != nullptr && dateStr[0] != '\0') {
    url += "&date=";
    url += dateStr;
  }

  setScheduleLastUrl(url.c_str());
  setScheduleLastFetchAt(millis());

  doc.clear();
  int httpCode = http_fetch::get(url, 10000);
  http_fetch::logCall("schedule", httpCode);
  setScheduleLastHttpCode(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    buildScheduleFilter(filter);
    DeserializationError error = http_fetch::parseBody(doc, &filter);
    if (!error) {
      return true;
    } else {
      DBG_PRINTF("[MLB] Schedule JSON parse error: %s\n", error.c_str());
      http_fetch::closeSession();
      return false;
    }
  }
  DBG_PRINTF("[MLB] Schedule HTTP error: %d\n", httpCode);
  http_fetch::closeSession();
  return false;
}

bool fetchMlbScheduleRange(const char* startDate, const char* endDate,
                           JsonDocument& doc, const int teamIds[3]) {
  HTTPClient http;
  String url = "http://statsapi.mlb.com/api/v1/schedule?sportId=1"
               "&fields=dates,date,games,gamePk,gameDate,status,abstractGameState,detailedState,"
               "teams,away,team,id,name,score,home,team,id,name,score"
               "&startDate=";
  url += startDate;
  url += "&endDate=";
  url += endDate;
  // Restrict to the preferred teams server-side: a 3-day full-slate body is
  // ~13 KB, which cannot be buffered alongside the ~45 KB an open TLS session
  // pins on this heap; three teams over 3 days is ~2 KB and fits easily.
  // findNextGame() in the renderer only ever looks these teams up anyway.
  String teamIdParam;
  for (int i = 0; i < 3; i++) {
    if (teamIds == nullptr || teamIds[i] == 0) continue;
    if (teamIdParam.length() > 0) teamIdParam += ',';
    teamIdParam += String(teamIds[i]);
  }
  if (teamIdParam.length() > 0) {
    url += "&teamId=";
    url += teamIdParam;
  }

  setScheduleLastUrl(url.c_str());
  setScheduleLastFetchAt(millis());

  doc.clear();
  int httpCode = http_fetch::get(url, 10000);
  http_fetch::logCall("schedule_range", httpCode);
  setScheduleLastHttpCode(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    buildScheduleFilter(filter);
    DeserializationError error = http_fetch::parseBody(doc, &filter);
    if (!error) {
      return true;
    }
    DBG_PRINTF("[MLB] Schedule range JSON parse error: %s\n", error.c_str());
    http_fetch::closeSession();
    return false;
  }
  DBG_PRINTF("[MLB] Schedule range HTTP error: %d\n", httpCode);
  http_fetch::closeSession();
  return false;
}

bool fetchMlbLinescore(int gamePk, JsonDocument& doc) {
  String url = "http://statsapi.mlb.com/api/v1/game/";
  url += String(gamePk);
  url += "/linescore";

  // http_fetch::get() already retries once on a fresh connection; two rounds
  // total is enough and keeps the per-poll latency bounded.
  for (int attempt = 1; attempt <= 2; attempt++) {
    int httpCode = http_fetch::get(url, 4000);
    http_fetch::logCall("linescore", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      doc.clear();
      // Linescore bodies are tiny (~1.5 KB): read them via the exact-length
      // body reader so the keep-alive connection stays clean, then parse
      // the whole thing (no filter).
      DeserializationError error = http_fetch::parseBody(doc, nullptr);
      if (!error) {
        return true;
      }
      DBG_PRINTF("[MLB] Linescore JSON parse error (attempt %d/2): %s\n",
                    attempt, error.c_str());
      http_fetch::closeSession();
    } else {
      DBG_PRINTF("[MLB] Linescore HTTP error (attempt %d/2): %d\n",
                 attempt, httpCode);
      http_fetch::closeSession();
    }
  }

  DBG_PRINTF("[MLB] Linescore failed after 2 attempts\n");
  return false;
}

bool fetchMlbLatestPlay(int gamePk, JsonDocument& doc) {
  String url = "http://statsapi.mlb.com/api/v1/game/";
  url += String(gamePk);
  url += "/playByPlay?fields=allPlays,about,atBatIndex,isComplete,"
         "result,event,description,matchup,batter,fullName";

  int httpCode = http_fetch::get(url, 5000);
  http_fetch::logCall("play_by_play", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    doc.clear();
    JsonDocument filter;
    filter["allPlays"][0]["about"]["atBatIndex"] = true;
    filter["allPlays"][0]["about"]["isComplete"] = true;
    filter["allPlays"][0]["result"]["event"] = true;
    filter["allPlays"][0]["result"]["description"] = true;
    filter["allPlays"][0]["matchup"]["batter"]["fullName"] = true;

    DeserializationError error =
        http_fetch::parseBody(doc, &filter);
    if (!error) {
      return true;
    }
    DBG_PRINTF("[MLB] Play-by-play JSON parse error: %s\n", error.c_str());
    http_fetch::closeSession();
    return false;
  }
  DBG_PRINTF("[MLB] Play-by-play HTTP error: %d\n", httpCode);
  http_fetch::closeSession();
  return false;
}

bool fetchMlbStandings(JsonDocument& doc, int season) {
  HTTPClient http;
  String url = "http://statsapi.mlb.com/api/v1/standings?leagueId=103,104"
               "&standingsTypes=regularSeason"
               "&fields=records,division,id,teamRecords,team,id,name,"
               "divisionRank,gamesBack,wins,losses,clinched,wildCardGamesBack";
  if (season > 0) {
    url += "&season=";
    url += String(season);
  }

  WiFiClient client;  // plain HTTP — see the note at the top of this file
  http.begin(client, url);
  http.setTimeout(12000);
  int httpCode = http.GET();
  http_fetch::logCall("standings", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    // The API's "fields" filter can't tell our outer "records" array apart from the
    // (much larger) per-team "records" stats blob that shares the same key name, so
    // every team's split/division/league/expected records come through regardless.
    // Filter client-side so only what we render is kept in memory.
    JsonDocument filter;
    filter["records"][0]["division"]["id"] = true;
    filter["records"][0]["teamRecords"][0]["team"]["id"] = true;
    filter["records"][0]["teamRecords"][0]["team"]["name"] = true;
    filter["records"][0]["teamRecords"][0]["wins"] = true;
    filter["records"][0]["teamRecords"][0]["losses"] = true;
    filter["records"][0]["teamRecords"][0]["divisionRank"] = true;
    filter["records"][0]["teamRecords"][0]["gamesBack"] = true;
    filter["records"][0]["teamRecords"][0]["clinched"] = true;
    filter["records"][0]["teamRecords"][0]["wildCardGamesBack"] = true;

    DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (!error) {
      size_t divisionCount = doc["records"].as<JsonArrayConst>().size();
      DBG_PRINTF("[MLB] Standings parsed OK: %u divisions\n",
                    static_cast<unsigned int>(divisionCount));
      return true;
    }
    DBG_PRINTF("[MLB] Standings JSON parse error: %s\n", error.c_str());
    return false;
  }
  DBG_PRINTF("[MLB] Standings HTTP error: %d\n", httpCode);
  http.end();
  return false;
}

bool fetchEspnMlbNews(JsonDocument& doc, int limit) {
  String url = "http://site.api.espn.com/apis/site/v2/sports/baseball/mlb/news?limit=";
  url += String(limit);

  doc.clear();
  doc.shrinkToFit();
  http_fetch::releaseBodyBuffer();
  // Drop the statsapi keep-alive session so the two connections never
  // overlap; the next statsapi fetch reopens its session transparently.
  http_fetch::closeSession();

  int httpCode = -1;
  for (int attempt = 0; attempt < 2 && httpCode < 0; attempt++) {
    HTTPClient http;
    WiFiClient client;  // plain HTTP — see the note at the top of this file
    http.begin(client, url);
    http.setTimeout(8000);
    httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      // ESPN's article objects are heavy (categories/athletes/links/images),
      // so filter client-side to just headline + description to keep this
      // parseable on-device. This stays a streaming parse on purpose: the
      // unfiltered body is far larger than we could ever buffer, and the
      // filtered doc (~4 KB) is trivial against a TLS-free heap.
      JsonDocument filter;
      filter["articles"][0]["headline"] = true;
      filter["articles"][0]["description"] = true;

      DeserializationError error =
          deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      http.end();
      http_fetch::logCall("espn_mlb_news", httpCode);
      if (!error) {
        size_t articleCount = doc["articles"].as<JsonArrayConst>().size();
        DBG_PRINTF("[ESPN] News parsed OK: %u articles\n",
                   static_cast<unsigned int>(articleCount));
        return true;
      }
      DBG_PRINTF("[ESPN] News JSON parse error: %s\n", error.c_str());
      return false;
    }
    // Non-OK code: log and (if negative = transport failure) retry once on
    // a completely fresh client — fragmentation and router moods are both
    // transient.
    http_fetch::logCall("espn_mlb_news", httpCode);
    DBG_PRINTF("[ESPN] News HTTP error (attempt %d/2): %d\n", attempt + 1, httpCode);
    http.end();
  }
  return false;
}

int selectGamePkForTeams(JsonObjectConst scheduleRoot, const int preferredTeamIds[3]) {
  JsonArrayConst dates = scheduleRoot["dates"].as<JsonArrayConst>();
  if (dates.isNull() || dates.size() == 0) return 0;

  // Search for games involving preferred teams in order of priority (p1 -> p2 -> p3)
  for (int p = 0; p < 3; p++) {
    int targetTeam = preferredTeamIds[p];
    if (targetTeam == 0) continue;

    for (JsonObjectConst date : dates) {
      for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
        int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
        int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;

        if (awayTeamId == targetTeam || homeTeamId == targetTeam) {
          const char* state = game["status"]["abstractGameState"] | "";
          if (strcmp(state, "Live") == 0) {
            return game["gamePk"] | 0;
          }
        }
      }
    }
  }

  return 0;
}

bool isMlbGameFinal(JsonObjectConst scheduleRoot, int gamePk) {
  if (gamePk <= 0) return false;

  for (JsonObjectConst date : scheduleRoot["dates"].as<JsonArrayConst>()) {
    for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
      if ((game["gamePk"] | 0) != gamePk) continue;
      const char* state = game["status"]["abstractGameState"] | "";
      return strcmp(state, "Final") == 0;
    }
  }
  return false;
}
