#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "boot_logo.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/comms/ota_update.h"
#include "common/data/time_util.h"
#include "common/hal/audio.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"
#include "common/trivia/trivia_engine.h"
#include "nhl_renderer.h"
#include "nhl_snapshot.h"
#include "nhl_state.h"
#include "nhl_teams.h"
#include "trivia_questions.h"

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
GameSnapshot sDemoGame{};
uint32_t sDemoStartedAtMs = 0;
uint32_t sDemoLastSecond = UINT32_MAX;

void startGameDemo(uint32_t nowMs) {
  sDemoGame = GameSnapshot{};
  sDemoGame.valid = true;
  sDemoGame.gameId = -1;
  strlcpy(sDemoGame.gameState, "LIVE", sizeof(sDemoGame.gameState));
  sDemoGame.homeTeamId = 10;
  sDemoGame.awayTeamId = 22;
  strlcpy(sDemoGame.homeAbbrev, "TOR", sizeof(sDemoGame.homeAbbrev));
  strlcpy(sDemoGame.awayAbbrev, "EDM", sizeof(sDemoGame.awayAbbrev));
  sDemoGame.homeScore = 2;
  sDemoGame.awayScore = 3;
  sDemoGame.homeSog = 20;
  sDemoGame.awaySog = 23;
  sDemoGame.period = 2;
  strlcpy(sDemoGame.periodType, "REG", sizeof(sDemoGame.periodType));
  sDemoGame.clockSec = 12 * 60 + 34;
  sDemoGame.clockRunning = true;
  sDemoGame.homePenaltyCount = 1;
  sDemoGame.penaltyCount = 1;
  strlcpy(sDemoGame.penalties[0].teamAbbrev, "TOR",
          sizeof(sDemoGame.penalties[0].teamAbbrev));
  strlcpy(sDemoGame.penalties[0].lastName, "RIELLY",
          sizeof(sDemoGame.penalties[0].lastName));
  sDemoGame.penalties[0].number = 44;
  strlcpy(sDemoGame.penalties[0].desc, "tripping",
          sizeof(sDemoGame.penalties[0].desc));
  sDemoGame.penalties[0].remainSec = 86;
  sDemoGame.penalties[0].durMin = 2;
  sDemoGame.homePeriodGoals[0] = 1;
  sDemoGame.homePeriodGoals[1] = 1;
  sDemoGame.awayPeriodGoals[0] = 2;
  sDemoGame.awayPeriodGoals[1] = 1;
  sDemoStartedAtMs = nowMs;
  sDemoLastSecond = 0;
  forceLiveRepaint(sDemoGame);
}

bool advanceGameDemo(uint32_t nowMs) {
  uint32_t elapsedSec = (nowMs - sDemoStartedAtMs) / 1000;
  if (elapsedSec == sDemoLastSecond) return false;
  sDemoLastSecond = elapsedSec;

  if (elapsedSec < 12 * 60 + 34) {
    sDemoGame.period = 2;
    sDemoGame.clockSec = 12 * 60 + 34 - elapsedSec;
  } else {
    uint32_t afterPeriod = elapsedSec - (12 * 60 + 34);
    uint32_t completedPeriods = afterPeriod / 1200;
    if (completedPeriods == 0) {
      sDemoGame.period = 3;
      sDemoGame.clockSec = 1200 - afterPeriod;
    } else {
      sDemoGame.period = 4;
      strlcpy(sDemoGame.periodType, "OT", sizeof(sDemoGame.periodType));
      sDemoGame.clockSec = 300 - (afterPeriod % 300);
      sDemoGame.otColumn = true;
    }
  }

  sDemoGame.homeSog = 20 + (elapsedSec / 8);
  sDemoGame.awaySog = 23 + (elapsedSec / 7);
  if (sDemoGame.homeSog > 99) sDemoGame.homeSog = 99;
  if (sDemoGame.awaySog > 99) sDemoGame.awaySog = 99;

  uint32_t penaltyCycle = elapsedSec / 24;
  bool penaltyActive = (penaltyCycle % 2) != 0;
  sDemoGame.penaltyCount = penaltyActive ? 1 : 0;
  sDemoGame.homePenaltyCount = 0;
  sDemoGame.awayPenaltyCount = 0;
  if (penaltyActive) {
    bool homePenalized = (penaltyCycle % 4) == 1;
    sDemoGame.homePenaltyCount = homePenalized ? 1 : 0;
    sDemoGame.awayPenaltyCount = homePenalized ? 0 : 1;
    NhlPenalty& penalty = sDemoGame.penalties[0];
    strlcpy(penalty.teamAbbrev, homePenalized ? "TOR" : "EDM",
            sizeof(penalty.teamAbbrev));
    strlcpy(penalty.lastName, homePenalized ? "RIELLY" : "NURSE",
            sizeof(penalty.lastName));
    penalty.number = homePenalized ? 44 : 25;
    strlcpy(penalty.desc, "tripping", sizeof(penalty.desc));
    penalty.remainSec = 90;
    penalty.durMin = 2;
  }

  bool goalScored = elapsedSec != 0 && elapsedSec % 20 == 0;
  if (goalScored) {
    bool homeScored = ((elapsedSec / 20) % 2) == 0;
    int column = sDemoGame.period <= 3 ? sDemoGame.period - 1 : 3;
    if (homeScored) {
      ++sDemoGame.homeScore;
      ++sDemoGame.homePeriodGoals[column];
    } else {
      ++sDemoGame.awayScore;
      ++sDemoGame.awayPeriodGoals[column];
    }
    NhlGoal& goal = sDemoGame.latestGoal;
    goal = NhlGoal{};
    goal.valid = true;
    goal.eventId = 1000 + elapsedSec;
    goal.homeScore = sDemoGame.homeScore;
    goal.awayScore = sDemoGame.awayScore;
    strlcpy(goal.teamAbbrev, homeScored ? "TOR" : "EDM",
            sizeof(goal.teamAbbrev));
    goal.scorer.number = homeScored ? 34 : 97;
    strlcpy(goal.scorer.name, homeScored ? "AUSTON MATTHEWS"
                                         : "CONNOR MCDAVID",
            sizeof(goal.scorer.name));
  }

  renderLiveGame(sDemoGame);
  if (goalScored) {
    startGoalAnnouncement(sDemoGame.latestGoal);
    if (isAudioEnabled()) startClip(GOAL_PCM, GOAL_SAMPLES, GOAL_RATE);
  }
  return true;
}

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

// ---- Hibernate display control ----
// "Off" as far as firmware can take it: penalty LEDs dark, matrices in
// shutdown, TM1637 blank, TFT blacked and sent into panel sleep. The
// backlight has no firmware control on this wiring — it stays dimly lit
// (hardware limit; a switch or PWM gate would be needed to kill it).
void enterDisplayHibernate() {
  setCountLeds(0, 0, 0);
  setMax7219Display(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK,
                    MAX7219_SCORE_BLANK);
  setMax7219Power(false);
  tm1637SetBrightness(0);  // 0 = display blank
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(0x0000);
  tftPanel.pushFull();
  tftPanel.raw().sendCommand(0x10);  // SLPIN
  DBG_PRINTF("[HIB] dark after %lu s idle\n",
             (unsigned long)(millis() / 1000));
}

void restoreDisplaysFromHibernate() {
  tftPanel.raw().sendCommand(0x11);  // SLPOUT
  delay(120);  // ST7789 wake time before the first write
  setMax7219Power(true);
  tm1637SetBrightness(TM1637_BRIGHTNESS);
  // Content repaint is the caller's job (mode/state aware).
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
  const int boardVersion = getBoardVersion();
  const int ledOrder[4] = {awayPenalty2Pin(boardVersion),
                           awayPenalty1Pin(boardVersion),
                           homePenalty2Pin(boardVersion),
                           homePenalty1Pin(boardVersion)};
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
  if (isGameDemoMode() && sDemoGame.valid) {
    forceLiveRepaint(sDemoGame);
  } else if (scoreboardState == ScoreboardState::LIVE_GAME && sLastGame.valid) {
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
  const int boardVersion = getBoardVersion();
  const int ledOrder[4] = {awayPenalty2Pin(boardVersion),
                           awayPenalty1Pin(boardVersion),
                           homePenalty2Pin(boardVersion),
                           homePenalty1Pin(boardVersion)};
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

const NetworkBoardOption* boardOptions(size_t& count) {
  static const NetworkBoardOption options[] = {
    {BOARD_VERSION_V0_1, "Board V0.1"},
    {BOARD_VERSION_V1_0, "Board V1.0"},
    {BOARD_VERSION_V1_1, "Board V1.1 (battery sense)"},
  };
  count = sizeof(options) / sizeof(options[0]);
  return options;
}

// Boards default to V1.1 (battery sense): never-configured hardware
// assumes the current PCB rev. Older boards opt out once in the portal —
// the saved NVS value always wins over this default.
int defaultBoardVersion() { return BOARD_VERSION_V1_1; }

TriviaBank triviaBank() {
  static const TriviaBank bank = {
      TRIVIA_QUESTIONS_EASY,
      sizeof(TRIVIA_QUESTIONS_EASY) / sizeof(TriviaQuestion),
      TRIVIA_QUESTIONS_MEDIUM,
      sizeof(TRIVIA_QUESTIONS_MEDIUM) / sizeof(TriviaQuestion),
      TRIVIA_QUESTIONS_HARD,
      sizeof(TRIVIA_QUESTIONS_HARD) / sizeof(TriviaQuestion),
  };
  return bank;
}

void setup() {
  // The TFT panel comes up FIRST: begin() raises it clean black (no boot
  // snow — see TftPanel::begin) and the boot splash paints within moments
  // of power-on. Everything else initializes behind the logo. The panel
  // itself is software SPI (bit-bangs SCK/MOSI on exactly the pins passed
  // in — see common/hal/tft_panel.h for why the hardware-SPI variant must
  // not be used on this board).
  tftPanel.begin(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                 TFT_RESET_PIN, TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT, 1);

  // Initialize hardware: penalty LEDs, MAX7219 matrices, and the TM1637
  // clock (game clock when live, wall clock between games).
  const int boardVersion = getBoardVersion();
  const int countLedPins[7] = {homePenalty1Pin(boardVersion),
                               homePenalty2Pin(boardVersion), PENALTY_FILL_A,
                               awayPenalty1Pin(boardVersion),
                               awayPenalty2Pin(boardVersion), PENALTY_FILL_B1,
                               PENALTY_FILL_B2};
  initCountLeds(countLedPins);
  // V1.1 inherits the V1.0 pin map (matrix order/orientation, TM1637 on
  // 15/16) and adds the battery-sense divider — hence the >= comparisons.
  const bool boardV1 = boardVersion >= BOARD_VERSION_V1_0;
  initLedMatrix(MAX7219_DIN_PIN, MAX7219_CLK_PIN, MAX7219_CS_PIN, boardV1,
                boardV1);
  const int tm1637ClkPin = boardVersion >= BOARD_VERSION_V1_0
                               ? TM1637_CLK_V1_0_PIN
                               : TM1637_CLK_V0_1_PIN;
  const int tm1637DioPin = boardVersion >= BOARD_VERSION_V1_0
                               ? TM1637_DIO_V1_0_PIN
                               : TM1637_DIO_V0_1_PIN;
  initTm1637(tm1637ClkPin, tm1637DioPin, TM1637_BRIGHTNESS);
  DBG_PRINTF("[BOARD] V%d.%d TM1637 CLK=%d DIO=%d\n",
             boardVersion / 10, boardVersion % 10,
             tm1637ClkPin, tm1637DioPin);
  initAudio(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);

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

  renderBootSplash(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, BOOT_LOGO_NHL);

  // TEST ONLY: cycles the count LEDs one at a time at boot; leave enabled
  // during hardware validation. Runs behind the already-painted splash.
  runCountLedTestLoop();

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

  // Settings-page game demo owns the display and hardware until stopped.
  static bool sInManual = false;
  static bool sInDemo = false;
  static bool sInTrivia = false;
  if (isGameDemoMode()) {
    if (!sInDemo) {
      sInDemo = true;
      sInManual = false;
      startGameDemo(ctx.nowMs);
    } else {
      advanceGameDemo(ctx.nowMs);
    }
    tickLiveClock();
    return;
  }
  if (sInDemo) {
    sInDemo = false;
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

  // ---- Manual mode (portal page owns the values) ----
  if (isManualMode()) {
    if (!sInManual) {
      sInManual = true;
      invalidateManualScreen();
    }
    if (consumeManualPeriodEndBuzzer() && isAudioEnabled()) {
      Serial.println("[AUDIO] manual end-of-period buzzer");
      startClip(BUZZER_PCM, BUZZER_SAMPLES, BUZZER_RATE);
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

  // ---- Trivia mode (engine on core 0, screen drawn here) ----
  if (trivia::isModeActive()) {
    sInTrivia = true;
    if (trivia::consumeWrongAnswerCue() && isAudioEnabled()) {
      Serial.println("[AUDIO] trivia wrong-answer buzzer");
      startClip(BUZZER_PCM, BUZZER_SAMPLES, BUZZER_RATE);
    }
    trivia::renderScreen(ctx.nowMs);
    return;
  }
  if (sInTrivia) {
    sInTrivia = false;
    // Trivia mode handed the display back — repaint whatever was showing.
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

  // ---- Hibernate between games (portal setting) ----
  // After HIBERNATE_IDLE_GRACE_MS with no live game the board goes dark
  // and appears off: TFT panel asleep (the backlight has no firmware
  // control — it keeps glowing dimly, a hardware limit), MAX7219 chain
  // in shutdown, TM1637 blank, penalty LEDs off. While dark, tick()
  // only watches for wake reasons: the 5-min silent schedule check (the
  // data task keeps running it) showing a live preferred-team game,
  // portal activity, or an OTA download. It re-hibernates HIBERNATE_
  // IDLE_GRACE_MS after the next idle period starts.
  static bool sHibernating = false;
  static uint32_t sIdleSinceMs = 0;
  if (sHibernating) {
    long wakeGameId = 0;
    ScheduleSnapshot sch{};
    if (takeScheduleSnapshot(sch, sLastScheduleGen) && sch.valid) {
      int preferred[3];
      getPreferredTeamIds(preferred);
      wakeGameId = selectLiveGameId(sch, preferred);
    }
    if (wakeGameId == 0 && (portalEngaged() || otaUpdateInProgress())) {
      wakeGameId = -1;  // wake without a game: someone needs the board up
    }
    if (wakeGameId != 0) {
      sHibernating = false;
      setGameHibernateActive(false);
      restoreDisplaysFromHibernate();
      DBG_PRINTF("[HIB] awake%s\n", wakeGameId > 0 ? " — game on" : "");
      if (wakeGameId > 0) {
        setActiveGameId(wakeGameId);
        scoreboardState = ScoreboardState::LIVE_GAME;
        postgameGraceStartedAt = 0;
        sIdleSinceMs = ctx.nowMs;
        if (sLastGame.valid && sLastGame.gameId == wakeGameId) {
          forceLiveRepaint(sLastGame);
        } else {
          int preferred[3];
          getPreferredTeamIds(preferred);
          JsonObjectConst sched = getUpcomingScheduleJson();
          renderWaiting(sched, preferred);  // placeholder until the first landing snapshot
          sLastScheduleRenderAt = ctx.nowMs;
        }
      } else {
        sIdleSinceMs = ctx.nowMs;  // portal/OTA wake: idle clock restarts on their exit
      }
      invalidateMax7219Clock();
      invalidateTm1637WallClock();
      return;
    }
    return;  // still dark — nothing visual happens this pass
  }
  if (isHibernateEnabled()) {
    // Manual/demo/trivia own the display earlier in tick() and return
    // before this point — a live game, the portal, or an OTA download
    // are the only things that keep a waiting board lit.
    bool busy = scoreboardState == ScoreboardState::LIVE_GAME ||
                portalEngaged() || otaUpdateInProgress();
    if (busy) {
      sIdleSinceMs = ctx.nowMs;
    } else if (sIdleSinceMs == 0) {
      sIdleSinceMs = ctx.nowMs;
    } else if (ctx.nowMs - sIdleSinceMs >= HIBERNATE_IDLE_GRACE_MS) {
      sHibernating = true;
      setGameHibernateActive(true);
      enterDisplayHibernate();
      return;
    }
  } else {
    sIdleSinceMs = 0;
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
      bool sameGame = sLastGame.valid && sLastGame.gameId == game.gameId;
      int prevHome = sameGame ? sLastGame.homeScore : -1;
      int prevAway = sameGame ? sLastGame.awayScore : -1;
      bool goalDetailsUpdated = sameGame && sLastGame.latestGoal.valid &&
          game.latestGoal.valid &&
          sLastGame.latestGoal.eventId == game.latestGoal.eventId &&
          sLastGame.latestGoal.scorer.number <= 0 &&
          game.latestGoal.scorer.number > 0;
      // Edge triggers for the end-of-period/game buzzer (once per
      // transition; sound setting still applies).
      bool periodEnded = sameGame && !sLastGame.inIntermission &&
                         game.inIntermission;
      bool gameEnded = sameGame &&
          !(strcmp(sLastGame.gameState, "FINAL") == 0 ||
            strcmp(sLastGame.gameState, "OFF") == 0 ||
            strcmp(sLastGame.gameState, "OVER") == 0) &&
          (strcmp(game.gameState, "FINAL") == 0 ||
           strcmp(game.gameState, "OFF") == 0 ||
           strcmp(game.gameState, "OVER") == 0);
      sLastGame = game;
      renderLiveGame(game);
      if (goalDetailsUpdated) updateGoalAnnouncement(game.latestGoal);
      if ((periodEnded || gameEnded) && isAudioEnabled()) {
        Serial.printf("[AUDIO] %s buzzer\n", gameEnded ? "game" : "period");
        startClip(BUZZER_PCM, BUZZER_SAMPLES, BUZZER_RATE);
      }
      bool homeScored = prevHome >= 0 && game.homeScore > prevHome;
      bool awayScored = prevAway >= 0 && game.awayScore > prevAway;
      if (homeScored || awayScored) {
        startGoalAnnouncement(game.latestGoal);
        // Goal horn fires only when one of the user's own teams scored
        // (the matrix score blink fires for either side).
        bool favorite = false;
        for (int i = 0; i < 3 && !favorite; ++i) {
          if (preferredTeams[i] == 0) continue;
          if (homeScored && game.homeTeamId == preferredTeams[i]) favorite = true;
          if (awayScored && game.awayTeamId == preferredTeams[i]) favorite = true;
        }
        if (favorite && isAudioEnabled()) {
          Serial.printf("[AUDIO] GOAL! %s %d - %s %d\n", game.awayAbbrev,
                        game.awayScore, game.homeAbbrev, game.homeScore);
          startClip(GOAL_PCM, GOAL_SAMPLES, GOAL_RATE);
        }
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
