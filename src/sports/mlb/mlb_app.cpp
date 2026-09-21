#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "boot_logo.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/data/time_util.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "mlb_renderer.h"
#include "mlb_snapshot.h"
#include "mlb_state.h"
#include "mlb_teams.h"

// MLB app: the WAITING/LIVE state machine and sport:: contract
// implementation (see common/app/sport_api.h). The generic shell in
// src/main.cpp drives boot/network/update; everything here is what makes
// this build an MLB scoreboard.

namespace {

enum class ScoreboardState {
  WAITING,
  LIVE_GAME
};
ScoreboardState scoreboardState = ScoreboardState::WAITING;

// Last-rendered state, used to detect transitions between live/waiting and to
// drive the postgame grace window without re-reading snapshots.
uint32_t postgameGraceStartedAt = 0;
int  lastAtBatIndex       = -1;
bool atBatBaselineReady   = false;

// "Last seen" generation counters for the snapshots we consume.
uint32_t sLastLinescoreGen = 0;
uint32_t sLastPlayGen      = 0;
uint32_t sLastScheduleGen  = 0;
uint32_t sLastScheduleRenderAt = 0;

// The data task can't decide on its own which game the user wants to follow
// (that depends on the preferred-team list, which lives in NVS on core 1).
// We make that decision here, set activeGamePk via setActiveGamePk(), and the
// data task reads it on the next iteration. Until core-0 publishes its first
// schedule snapshot we stay in WAITING (the data task publishes a valid (but
// possibly empty) schedule within ~5 s).

// Pick the gamePk of the highest-priority preferred team currently live,
// straight from the other-games snapshot. This must not depend on any
// previously-fetched linescore: the data task only polls a linescore once a
// gamePk is selected here, so deriving the selection FROM a linescore can
// never bootstrap out of waiting mode (the bug this replaces).
int selectLiveGamePkForTeams(const ScheduleSnapshot& sch,
                             const int preferredTeams[3]) {
  for (int p = 0; p < 3; ++p) {
    int teamId = preferredTeams[p];
    if (teamId == 0) continue;
    for (size_t i = 0; i < sch.otherCount; ++i) {
      int a = atoi(sch.others[i].awayAbbrev);
      int h = atoi(sch.others[i].homeAbbrev);
      if (a == teamId || h == teamId) {
        return sch.others[i].gamePk;
      }
    }
  }
  return 0;
}

void logPendingLiveDisplayState() {
  DBG_PRINTF(
    "[DISPLAY] state=LIVE_GAME gamePk=%d | TFT/matrices/count LEDs=waiting "
    "for immediate linescore refresh\n", getActiveGamePk());
}

bool getScheduleRange(char startDate[11], char endDate[11]) {
  time_t now = time(nullptr);
  if (!timeIsSynced()) {  // NTP has not set the clock yet.
    return false;
  }

  tm startLocal = {};
  localtime_r(&now, &startLocal);
  time_t endTime = now + (13 * 24 * 60 * 60);
  tm endLocal = {};
  localtime_r(&endTime, &endLocal);

  return strftime(startDate, 11, "%Y-%m-%d", &startLocal) > 0 &&
         strftime(endDate, 11, "%Y-%m-%d", &endLocal) > 0;
}

}  // namespace

namespace sport {

const char* name() { return "MLB Scoreboard"; }

NetworkBranding branding() {
  return NetworkBranding{name(), NETWORK_AP_SSID, NETWORK_HOSTNAME};
}

const NetworkTeamOption* teamOptions(size_t& count) {
  return mlbTeamOptions(count);
}

const int* defaultPreferredTeams() { return MLB_DEFAULT_PREFERRED_TEAMS; }

void setup() {
  // Initialize hardware: discrete count LEDs + MAX7219 matrices, then the
  // TFT panel over software SPI (bit-bangs SCK/MOSI on exactly the pins
  // passed in — see common/hal/tft_panel.h for why the hardware-SPI variant
  // must not be used on this board).
  const int countLedPins[7] = {BALL_1_PIN, BALL_2_PIN, BALL_3_PIN,
                               STRIKE_1_PIN, STRIKE_2_PIN, OUT_1_PIN,
                               OUT_2_PIN};
  initCountLeds(countLedPins);
  initLedMatrix(MAX7219_DIN_PIN, MAX7219_CLK_PIN, MAX7219_CS_PIN);

  // TEST ONLY: shows H (home) / A (away) on the score matrices; leave enabled during hardware validation.
  //runMax7219BootTest();

  tftPanel.begin(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                 TFT_RESET_PIN, TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT, 1);

    // TEST ONLY: cycles ball/strike/out LEDs one at a time; leave enabled during hardware validation.
  runCountLedTestLoop();

  renderBootSplash(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, BOOT_LOGO_MLB);
  // No blocking hold here: network services start immediately and connect
  // behind the logo. The shell's loop() enforces the minimum splash time.
  Serial.printf("[MAIN] MLB Scoreboard ready (ESPN news TTL %lu min)\n",
                (unsigned long)(MLB_NEWS_CACHE_TTL_MS / 60000UL));
}

void startDataTask() {
  startMlbDataTask();  // core-0 linescore/play/schedule/news fetches
}

bool hasInitialData() {
  return getUpcomingSchedulePublishedAt() != 0;
}

void tick(const SportTickContext& ctx) {
  updateAtBatResultDisplay();
  rotateCarousel();

  if (consumeScoreboardRelease()) {
    int preferredTeams[3];
    getPreferredTeamIds(preferredTeams);
    scoreboardState = ScoreboardState::WAITING;
    setActiveGamePk(0);
    postgameGraceStartedAt = 0;
    JsonObjectConst sched = getUpcomingScheduleJson();
    renderWaiting(sched, sched, preferredTeams);
    logWaitingDisplayState(preferredTeams);
  }

  if (!ctx.isOnline) {
    return;
  }

  uint32_t now = ctx.nowMs;
  int preferredTeams[3];
  getPreferredTeamIds(preferredTeams);

  // Tick the scoreboard clock once per loop when idle (no live game).
  if (scoreboardState == ScoreboardState::WAITING) {
    updateMax7219Clock(isClockDisplayEnabled());
  }

  // ---- Consume new linescore snapshot from core 0 ----
  LinescoreSnapshot ls{};
  bool newLinescore = takeLinescoreSnapshot(ls, sLastLinescoreGen);

  // ---- Consume new play-by-play snapshot ----
  PlaySnapshot play{};
  bool newPlay = takePlaySnapshot(play, sLastPlayGen);

  // ---- Consume new schedule snapshot ----
  ScheduleSnapshot sch{};
  bool newSchedule = takeScheduleSnapshot(sch, sLastScheduleGen);

  // ---- Decide which game (if any) the renderer should follow ----
  //
  // The data task on core 0 polls linescore for whatever gamePk we tell it.
  // We pick the highest-priority preferred team that has a live game on
  // today's slate. The "other games" list inside ScheduleSnapshot carries
  // the away/home team ids; when one matches a preferred team we know there's
  // a live game for them. We then ask core 0 to switch to whichever gamePk
  // the data task is currently fetching (the linescore snapshot's gamePk) —
  // if that happens to be the same game.
  if (newSchedule && sch.valid) {
    // Select the followed game directly from the live-games snapshot.
    int newGamePk = selectLiveGamePkForTeams(sch, preferredTeams);

    updateOtherGames(sch, newGamePk);
    bool gameChanged = newGamePk != getActiveGamePk();
    if (newGamePk > 0) {
      setActiveGamePk(newGamePk);
      scoreboardState = ScoreboardState::LIVE_GAME;
      postgameGraceStartedAt = 0;
      if (gameChanged) {
        lastAtBatIndex = -1;
        atBatBaselineReady = false;
        resetAtBatResultDisplay();
      }
    }
  }

  // Render based on state.
  if (scoreboardState == ScoreboardState::WAITING) {
    // First entry to WAITING (or schedule refresh): draw the waiting page once.
    if (sLastScheduleRenderAt == 0 || newSchedule) {
      sLastScheduleRenderAt = now;
      JsonObjectConst sched = getUpcomingScheduleJson();
      renderWaiting(sched, sched, preferredTeams);
      logWaitingDisplayState(preferredTeams);
    }
  } else {
    // LIVE_GAME: each fresh linescore drives the live screen; each fresh
    // play snapshot may fire the at-bat result card.
    if (newLinescore && ls.valid && !isAtBatResultVisible()) {
      renderLinescore(ls);
      logLiveDisplayState(ls, ls.gamePk);
    }
    if (newPlay && play.valid) {
      if (atBatBaselineReady && play.atBatIndex > lastAtBatIndex &&
          play.description[0] != '\0') {
        showAtBatResult(play.batterName, play.event, play.description);
      }
      if (play.atBatIndex >= 0) {
        lastAtBatIndex = play.atBatIndex;
        atBatBaselineReady = true;
      }
    }
  }
}

}  // namespace sport
