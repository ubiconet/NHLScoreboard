#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "boot_logo.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/data/time_util.h"
#include "common/hal/audio.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"
#include "nhl_renderer.h"
#include "nhl_snapshot.h"
#include "nhl_state.h"
#include "nhl_teams.h"

#include "audio_clips.h"

// NHL app: the WAITING/LIVE state machine and sport:: contract
// implementation (see common/app/sport_api.h). The generic shell in
// src/main.cpp drives boot/network/update; everything here is what makes
// this build an NHL scoreboard.

namespace {

enum class ScoreboardState {
  WAITING,
  LIVE_GAME
};
ScoreboardState scoreboardState = ScoreboardState::WAITING;

// Postgame grace: keep the final score on screen for a few minutes before
// falling back to waiting mode.
uint32_t postgameGraceStartedAt = 0;

uint32_t sLastGameGen     = 0;
uint32_t sLastScheduleGen = 0;
uint32_t sLastScheduleRenderAt = 0;

// Most recent valid live snapshot — the display test restores from it.
GameSnapshot sLastGame{};

// Pick the highest-priority preferred team with a game currently in
// progress (LIVE/CRIT). Selection deliberately reads only the day-slate
// snapshot: the data task polls a landing feed only after we set the
// followed-game id here, so selection must never depend on that feed.
long selectLiveGameId(const ScheduleSnapshot& sch, const int preferred[3]) {
  for (int p = 0; p < 3; ++p) {
    int teamId = preferred[p];
    if (teamId == 0) continue;
    for (size_t i = 0; i < sch.count; ++i) {
      const NhlDayGame& g = sch.games[i];
      bool liveish = strcmp(g.gameState, "LIVE") == 0 ||
                     strcmp(g.gameState, "CRIT") == 0;
      if (liveish && (g.homeTeamId == teamId || g.awayTeamId == teamId)) {
        return g.gameId;
      }
    }
  }
  return 0;
}

// Portal-triggered display test: one cycle through every display, a 2 s
// hold with everything on, then back to whatever was showing. Runs on the
// render core (blocking ~12 s — nothing renders during the cycle by
// definition; feeds keep publishing on core 0 and land right after).
void runDisplayTestCycle() {
  Serial.println("[TEST] display test cycle (portal)");
  int preferred[3];
  getPreferredTeamIds(preferred);

  // Screen banner
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(4);
  drawCenteredText(c, "DISPLAY", 160, 78);
  drawCenteredText(c, "TEST", 160, 118);
  tftPanel.pushFull();

  // Clock: every segment lit ("88:88")
  tm1637ShowPair(88, 88, true);

  // Matrices: all-on, one chain position at a time
  for (int pos = 1; pos <= 3; ++pos) {
    setMax7219PositionTest(pos, true);
    delay(1200);
    setMax7219PositionTest(pos, false);
    delay(150);
  }
  // Then the digit pattern
  setMax7219Display(1, 3, 2);
  delay(1200);

  // Penalty LEDs: chase then all on
  static const int ledOrder[4] = {PENALTY_AWAY2_PIN, PENALTY_AWAY1_PIN,
                                  PENALTY_HOME2_PIN, PENALTY_HOME1_PIN};
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 4; ++k) digitalWrite(ledOrder[k], k == i ? HIGH : LOW);
    delay(250);
  }
  for (int k = 0; k < 4; ++k) digitalWrite(ledOrder[k], HIGH);

  // HOLD: everything on for 2 s
  setMax7219DisplayTestAll(true);
  delay(2000);

  // Restore whatever was showing before the cycle
  setMax7219DisplayTestAll(false);
  if (scoreboardState == ScoreboardState::LIVE_GAME && sLastGame.valid) {
    forceLiveRepaint(sLastGame);
  } else {
    JsonObjectConst sched = getUpcomingScheduleJson();
    renderWaiting(sched, preferred);
    invalidateMax7219Clock();
    invalidateTm1637WallClock();
  }
  Serial.println("[TEST] display test done — restored");
}

}  // namespace

// ==== HARDWARE TEST MODE =====================================================
// Bench wiring verification (TFT "TEST MODE", TM1637 12:00, matrices
// 1/3/2, rotating penalty LEDs, plus the narrated MAX7219 chain
// diagnostic at boot). Set to true to bench-test display wiring again;
// false runs the normal scoreboard state machine.
static const bool HARDWARE_TEST_MODE = false;

void runHardwareTestMode(uint32_t nowMs) {
  static bool staticDrawn = false;
  if (!staticDrawn) {
    staticDrawn = true;
    GFXcanvas16& c = tftPanel.canvas();
    c.fillScreen(COLOR_BG);
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(5);  // 30x35 px glyphs
    drawCenteredText(c, "TEST MODE", 160, 100);
    tftPanel.pushFull();
    tm1637ShowPair(12, 0, true);
    setMax7219Display(1, 3, 2);  // home, guest, period
    Serial.println("[TEST] hardware test mode active");
  }
  static const int ledOrder[4] = {PENALTY_AWAY2_PIN, PENALTY_AWAY1_PIN,
                                  PENALTY_HOME2_PIN, PENALTY_HOME1_PIN};
  int step = (nowMs / 500) % 4;
  for (int i = 0; i < 4; ++i) {
    digitalWrite(ledOrder[i], i == step ? HIGH : LOW);
  }
}

namespace sport {

const char* name() { return "NHL Scoreboard"; }

NetworkBranding branding() {
  return NetworkBranding{name(), NETWORK_AP_SSID, NETWORK_HOSTNAME};
}

const NetworkTeamOption* teamOptions(size_t& count) {
  return nhlTeamOptions(count);
}

const int* defaultPreferredTeams() { return NHL_DEFAULT_PREFERRED_TEAMS; }

void setup() {
  // Initialize hardware: penalty LEDs, MAX7219 matrices, and the TM1637
  // clock (game clock when live, wall clock between games), then the TFT
  // panel over software SPI (bit-bangs SCK/MOSI on exactly the pins
  // passed in — see common/hal/tft_panel.h for why the hardware-SPI
  // variant must not be used on this board).
  const int countLedPins[7] = {PENALTY_HOME1_PIN, PENALTY_HOME2_PIN,
                               PENALTY_FILL_A,    PENALTY_AWAY1_PIN,
                               PENALTY_AWAY2_PIN, PENALTY_FILL_B1,
                               PENALTY_FILL_B2};
  initCountLeds(countLedPins);
  initLedMatrix(MAX7219_DIN_PIN, MAX7219_CLK_PIN, MAX7219_CS_PIN);
  initTm1637(TM1637_CLK_PIN, TM1637_DIO_PIN, TM1637_BRIGHTNESS);
  initAudio(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);

  tftPanel.begin(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                 TFT_RESET_PIN, TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT, 1);

  if (HARDWARE_TEST_MODE) {
    // Bare-metal bench mode: first frame right here — no splash, no LED
    // boot test, no network (the shell checks skipBootUi()). The matrix
    // chain diagnostic runs first (narrated over Serial), then the steady
    // test pattern.
    runMax7219ChainDiagnostic();
    runHardwareTestMode(millis());
    Serial.printf("[MAIN] NHL Scoreboard ready — TEST MODE (no network)\n");
    return;
  }

  // TEST ONLY: cycles the count LEDs one at a time at boot; leave enabled
  // during hardware validation.
  runCountLedTestLoop();

  renderBootSplash(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, BOOT_LOGO_NHL);
  // No blocking hold here: network services start immediately and connect
  // behind the logo. The shell's loop() enforces the minimum splash time.
  Serial.printf("[MAIN] NHL Scoreboard ready (ESPN news TTL %lu min)\n",
                (unsigned long)(NHL_NEWS_CACHE_TTL_MS / 60000UL));
}

void startDataTask() {
  startNhlDataTask();  // core-0 schedule/landing/news fetches
}

bool skipBootUi() {
  return HARDWARE_TEST_MODE;
}

bool hasInitialData() {
  if (HARDWARE_TEST_MODE) return true;  // skip the boot status page
  return getUpcomingSchedulePublishedAt() != 0;
}

void tick(const SportTickContext& ctx) {
  if (HARDWARE_TEST_MODE) {
    runHardwareTestMode(ctx.nowMs);
    return;  // normal scoreboard logic disabled while bench-testing wiring
  }

  if (consumeDisplayTestRequest()) {
    runDisplayTestCycle();
    return;  // restored screen; next tick resumes normal flow
  }

  if (consumeAudioTestRequest()) {
    if (isAudioEnabled()) {
      Serial.println("[AUDIO] goal horn test");
      startClip(GOAL_PCM, GOAL_SAMPLES, GOAL_RATE);
    } else {
      Serial.println("[AUDIO] disabled in settings — test skipped");
    }
  }

  // ---- Manual mode (portal page owns the values) ----
  static bool sInManual = false;
  if (isManualMode()) {
    if (!sInManual) {
      sInManual = true;
      invalidateManualScreen();
    }
    renderManual();
    return;
  }
  if (sInManual) {
    sInManual = false;
    // Manual mode handed the display back — repaint whatever was showing.
    int preferred[3];
    getPreferredTeamIds(preferred);
    if (scoreboardState == ScoreboardState::LIVE_GAME && sLastGame.valid) {
      forceLiveRepaint(sLastGame);
    } else {
      JsonObjectConst sched = getUpcomingScheduleJson();
      renderWaiting(sched, preferred);
      invalidateMax7219Clock();
      invalidateTm1637WallClock();
    }
  }

  rotateCarousel();
  tickLiveClock();  // game clock counts down between the 5 s polls

  if (consumeScoreboardRelease()) {
    int preferredTeams[3];
    getPreferredTeamIds(preferredTeams);
    scoreboardState = ScoreboardState::WAITING;
    setActiveGameId(0);
    postgameGraceStartedAt = 0;
    JsonObjectConst sched = getUpcomingScheduleJson();
    renderWaiting(sched, preferredTeams);
  }

  if (!ctx.isOnline) {
    return;
  }

  uint32_t now = ctx.nowMs;
  int preferredTeams[3];
  getPreferredTeamIds(preferredTeams);

  // Idle clock lives on the TM1637 only — the score matrices stay dark
  // between games (both honor the "show idle clock" portal preference).
  if (scoreboardState == ScoreboardState::WAITING) {
    updateTm1637WallClock(isClockDisplayEnabled());
  }

  // ---- Consume new snapshots from core 0 ----
  GameSnapshot game{};
  bool newGame = takeGameSnapshot(game, sLastGameGen);
  ScheduleSnapshot sch{};
  bool newSchedule = takeScheduleSnapshot(sch, sLastScheduleGen);

  // ---- Followed-game selection (from the day slate only) ----
  if (newSchedule && sch.valid) {
    long newId = selectLiveGameId(sch, preferredTeams);
    updateOtherGames(sch, newId != 0 ? newId : getActiveGameId());
    if (newId != 0 && newId != getActiveGameId()) {
      setActiveGameId(newId);
      scoreboardState = ScoreboardState::LIVE_GAME;
      postgameGraceStartedAt = 0;
    }
  }

  // ---- Render by state ----
  if (scoreboardState == ScoreboardState::WAITING) {
    if (sLastScheduleRenderAt == 0 || newSchedule) {
      sLastScheduleRenderAt = now;
      JsonObjectConst sched = getUpcomingScheduleJson();
      renderWaiting(sched, preferredTeams);
    }
  } else {
    if (newGame && game.valid) {
      int prevSum = (sLastGame.valid && sLastGame.gameId == game.gameId)
                        ? sLastGame.homeScore + sLastGame.awayScore
                        : -1;
      sLastGame = game;
      renderLiveGame(game);
      int sum = game.homeScore + game.awayScore;
      if (prevSum >= 0 && sum > prevSum && isAudioEnabled()) {
        Serial.printf("[AUDIO] GOAL! %s %d - %s %d\n", game.awayAbbrev,
                      game.awayScore, game.homeAbbrev, game.homeScore);
        startClip(GOAL_PCM, GOAL_SAMPLES, GOAL_RATE);
      }
      bool over = strcmp(game.gameState, "FINAL") == 0 ||
                  strcmp(game.gameState, "OFF") == 0 ||
                  strcmp(game.gameState, "OVER") == 0;
      if (over) {
        if (postgameGraceStartedAt == 0) postgameGraceStartedAt = now;
        if (now - postgameGraceStartedAt > NHL_POSTGAME_GRACE_MS) {
          scoreboardState = ScoreboardState::WAITING;
          setActiveGameId(0);
          postgameGraceStartedAt = 0;
          JsonObjectConst sched = getUpcomingScheduleJson();
          renderWaiting(sched, preferredTeams);
          sLastScheduleRenderAt = now;
        }
      } else {
        postgameGraceStartedAt = 0;
      }
    }
  }
}

}  // namespace sport
