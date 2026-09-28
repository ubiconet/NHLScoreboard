#include "config.h"
#include "common/data/snapshot_channel.h"
#include "nhl_state.h"

namespace {
// One channel per feed; the data task publishes, the render loop samples
// with its own generation counters. See snapshot_channel.h for the memory
// ordering contract.
SnapshotChannel<GameSnapshot>     gGameChannel;
SnapshotChannel<ScheduleSnapshot> gScheduleChannel;

// Day-score JSON cache (the ONE JsonDocument allowed on the render core —
// see AGENTS.md). Feeds the waiting screen's upcoming-game cards.
JsonDocument gUpcomingScheduleDoc;
uint32_t     gUpcomingSchedulePublishedAt = 0;

// Cross-task counters + error tag for the day-score fetch. The data task
// increments these on every attempt; the waiting screen renders them as
// the "sched:..." diagnostics line so fetch failures can be told apart
// from publish ones.
volatile uint32_t gScheduleFetchAttempts  = 0;
volatile uint32_t gScheduleFetchSuccesses = 0;
char             gScheduleLastError[40]   = "none";
char             gScheduleLastUrl[80]     = "none";
volatile int     gScheduleLastHttpCode    = -1;
volatile uint32_t gScheduleLastFetchAt    = 0;
}  // namespace

bool takeGameSnapshot(GameSnapshot& out, uint32_t& lastGen) {
  return gGameChannel.take(out, lastGen);
}
bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen) {
  return gScheduleChannel.take(out, lastGen);
}

namespace nhl_data {
void publishGame(const GameSnapshot& s)     { gGameChannel.publish(s); }
void publishSchedule(const ScheduleSnapshot& s) { gScheduleChannel.publish(s); }
}

long gActiveGameId = 0;

long getActiveGameId() { return gActiveGameId; }
void setActiveGameId(long gameId) { gActiveGameId = gameId; }

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
  }
  gUpcomingSchedulePublishedAt = millis();
  DBG_PRINTF("[SCHED] republished: %u games\n",
             (unsigned)src["games"].as<JsonArrayConst>().size());
}
JsonObjectConst getUpcomingScheduleJson() {
  return gUpcomingScheduleDoc.as<JsonObjectConst>();
}
uint32_t getUpcomingSchedulePublishedAt() { return gUpcomingSchedulePublishedAt; }

uint32_t getScheduleFetchAttempts()  { return gScheduleFetchAttempts; }
uint32_t getScheduleFetchSuccesses() { return gScheduleFetchSuccesses; }
void bumpScheduleFetchAttempt() { ++gScheduleFetchAttempts; }
void bumpScheduleFetchSuccess() { ++gScheduleFetchSuccesses; }
const char* getScheduleLastError()   { return gScheduleLastError; }
void setScheduleLastError(const char* err) {
  if (err == nullptr) return;
  strlcpy(gScheduleLastError, err, sizeof(gScheduleLastError));
}
const char* getScheduleLastUrl()   { return gScheduleLastUrl; }
void setScheduleLastUrl(const char* url) {
  if (url == nullptr) return;
  strlcpy(gScheduleLastUrl, url, sizeof(gScheduleLastUrl));
}
int getScheduleLastHttpCode() { return (int)gScheduleLastHttpCode; }
void setScheduleLastHttpCode(int code) { gScheduleLastHttpCode = (volatile int)code; }
uint32_t getScheduleLastFetchAt() { return gScheduleLastFetchAt; }
void setScheduleLastFetchAt(uint32_t ms) { gScheduleLastFetchAt = ms; }
