#include "config.h"
#include "common/data/snapshot_channel.h"
#include "mlb_state.h"

namespace {
// One channel per feed; the data task publishes, the render loop samples
// with its own generation counters. See snapshot_channel.h for the memory
// ordering contract.
SnapshotChannel<LinescoreSnapshot> gLinescoreChannel;
SnapshotChannel<PlaySnapshot>      gPlayChannel;
SnapshotChannel<ScheduleSnapshot>  gScheduleChannel;

// Upcoming-schedule JSON cache (the ONE JsonDocument allowed on the render
// core — see AGENTS.md).
JsonDocument gUpcomingScheduleDoc;
uint32_t     gUpcomingSchedulePublishedAt = 0;

// Cross-task counters + error tag for the schedule fetch. The data task on
// core 0 increments these every time it tries (and fails) to fetch the
// schedule; the renderer reads them for the "no upcoming games" diagnostic
// screen so fetch failures can be told apart from publish ones.
volatile uint32_t gScheduleFetchAttempts  = 0;
volatile uint32_t gScheduleFetchSuccesses = 0;
char             gScheduleLastError[40]   = "none";
char             gScheduleLastUrl[80]     = "none";
volatile int     gScheduleLastHttpCode    = -1;
volatile uint32_t gScheduleLastFetchAt    = 0;
}  // namespace

bool takeLinescoreSnapshot(LinescoreSnapshot& out, uint32_t& lastGen) {
  return gLinescoreChannel.take(out, lastGen);
}
bool takePlaySnapshot(PlaySnapshot& out, uint32_t& lastGen) {
  return gPlayChannel.take(out, lastGen);
}
bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen) {
  return gScheduleChannel.take(out, lastGen);
}

namespace mlb_data {
void publishLinescore(const LinescoreSnapshot& s) { gLinescoreChannel.publish(s); }
void publishPlay(const PlaySnapshot& s)          { gPlayChannel.publish(s); }
void publishSchedule(const ScheduleSnapshot& s)  { gScheduleChannel.publish(s); }
}

int gActiveGamePk = 0;

int getActiveGamePk() { return gActiveGamePk; }
void setActiveGamePk(int gamePk) { gActiveGamePk = gamePk; }

void publishUpcomingScheduleJson(JsonObjectConst src) {
  if (src.isNull()) {
    gUpcomingScheduleDoc.clear();
    gUpcomingSchedulePublishedAt = millis();
    return;
  }
  gUpcomingScheduleDoc.clear();
  if (!gUpcomingScheduleDoc.set(src)) {
    DBG_PRINTF("[SCHED] republish copy failed\n");
    gUpcomingScheduleDoc.clear();
    gUpcomingSchedulePublishedAt = millis();
    return;
  }
  gUpcomingSchedulePublishedAt = millis();
  size_t dateCount = 0, gameCount = 0;
  for (JsonObjectConst date : src["dates"].as<JsonArrayConst>()) {
    ++dateCount;
    JsonArrayConst games = date["games"].as<JsonArrayConst>();
    if (!games.isNull()) gameCount += games.size();
  }
  DBG_PRINTF("[SCHED] republished: %u dates, %u games\n",
             (unsigned)dateCount, (unsigned)gameCount);
}
JsonObjectConst getUpcomingScheduleJson() {
  return gUpcomingScheduleDoc.as<JsonObjectConst>();
}
uint32_t getUpcomingSchedulePublishedAt() { return gUpcomingSchedulePublishedAt; }
size_t   getUpcomingScheduleDocSize()     { return gUpcomingScheduleDoc.size(); }

uint32_t getScheduleFetchAttempts()  { return gScheduleFetchAttempts; }
uint32_t getScheduleFetchSuccesses() { return gScheduleFetchSuccesses; }
void bumpScheduleFetchAttempt() { ++gScheduleFetchAttempts; }
void bumpScheduleFetchSuccess() { ++gScheduleFetchSuccesses; }
const char* getScheduleLastError()   { return gScheduleLastError; }
void setScheduleLastError(const char* err) {
  if (err == nullptr) return;
  strlcpy(gScheduleLastError, err, sizeof(gScheduleLastError));
}
const char* getScheduleLastUrl() { return gScheduleLastUrl; }
void setScheduleLastUrl(const char* url) {
  if (url == nullptr) return;
  strlcpy(gScheduleLastUrl, url, sizeof(gScheduleLastUrl));
}
int getScheduleLastHttpCode() { return (int)gScheduleLastHttpCode; }
void setScheduleLastHttpCode(int code) { gScheduleLastHttpCode = (volatile int)code; }
uint32_t getScheduleLastFetchAt() { return gScheduleLastFetchAt; }
void setScheduleLastFetchAt(uint32_t ms) { gScheduleLastFetchAt = ms; }
