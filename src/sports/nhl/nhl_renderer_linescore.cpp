#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/comms/network_service.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/gfx.h"
#include "nhl_logos.h"
#include "nhl_renderer.h"
#include "nhl_renderer_internal.h"

// Live-game screen. Everything the scoreboard tracks for a followed game:
//   TFT   — period + clock header, both team cards (logo/abbrev), big
//           scores, SOG + active-penalty strip.
//   LEDs  — score matrices (away/home), penalty count LEDs (2 per team).
// Value-cached dirty regions: a 5 s landing snapshot only repaints what
// changed (clock header, score digits, bottom strip), so the software-SPI
// panel never flickers on a routine poll.

namespace nhl_render {
GameSnapshot currentGame{};
bool hasCurrentLiveGame = false;
uint8_t tickerSlide = 0;
uint32_t lastCarouselTime = 0;
OtherGameInfo otherGames[MAX_OTHER_GAMES];
size_t otherGameCount = 0;
}  // namespace nhl_render

namespace {

using namespace nhl_render;  // otherGames ticker shared with the waiting renderer

GFXcanvas16& canvas() { return tftPanel.canvas(); }

// Layout (320x240) — no header band; the clock lives on the TM1637 and
// the period on the middle matrix module:
//   y   8..92  team logos: HOME left, GUEST right
//   y 100..144 SHOTS row: big count under each logo, "SHOTS" caption
//                centered between the two boxes
//   y 158..238 penalties (active: player + remaining) or, penalty-free,
//                the alternating linescore / league-scores strip
const int LOGO_Y = 6, LOGO_SIZE = 67;  // ~20% down from 84
// Column centers stay put (68 / 252); logos re-centered on them.
const int HOME_LOGO_X = 35, GUEST_LOGO_X = 219;  // home left, guest right
// Scoreboard-style shots box under each logo (white border, big gold
// count; the "SHOTS" caption sits between the boxes) — PAD widens the
// box past the smaller logo and Y leaves a clear gap below the abbrev.
const int ABBREV_Y = 76;  // white 3-letter abbrev between logo and box
const int SHOTS_BOX_Y = 100, SHOTS_BOX_H = 44, SHOTS_BOX_PAD = 8;
const int SHOTS_NUM_Y = 106;
const int BOTTOM_Y = 158;

// Last-drawn cache — anything that differs triggers that region's repaint.
struct Drawn {
  long gameId = 0;
  char homeAbbrev[4] = "", awayAbbrev[4] = "";
  int homeTeamId = 0, awayTeamId = 0;
  int homeScore = -1, awayScore = -1;
  int period = -1;  // last shown on the period matrix
  int clockSec = -1;  // tickLiveClock's per-second tracker (TM1637)
  int sogH = -1, sogA = -1;
  char penaltySig[256] = "";
} d;

// ---- Local game-clock ticker ----
// Between the 5 s landing polls the clock counts down locally so the
// display tracks the real game clock; it freezes when the game clock is
// stopped (whistle, intermission, final) and re-syncs on every poll. A
// 2 s deadband while continuously running hides network jitter (a snap
// every poll would stutter the display a second back and forth); real
// jumps — period start, a stoppage the poll caught — exceed it and snap,
// and any run/stop transition always snaps.
uint32_t sClockSyncedAt = 0;
int      sClockBasisSec = 0;
bool     sClockRunning = false;

enum class GoalDisplayPhase : uint8_t { NONE, ANNOUNCEMENT, DETAILS };
GoalDisplayPhase sGoalDisplayPhase = GoalDisplayPhase::NONE;
uint32_t sGoalDisplayStartedAt = 0;
NhlGoal sDisplayedGoal{};

bool goalDisplayActive() {
  return sGoalDisplayPhase != GoalDisplayPhase::NONE;
}

// Goal blink: the scoring team's NEW score flashes on its matrix
// (NHL_SCORE_BLINK_FLASHES x 250 ms on / 250 ms off), then returns to
// the steady display. bit 1 = home, bit 2 = guest.
uint32_t sBlinkStart = 0;
uint8_t sBlinkSide = 0;    // 0 = idle
uint8_t sBlinkPhase = 0;   // last applied half-period index

void applyScoreMatrices(bool blankBlinkSide) {
  const GameSnapshot& g = nhl_render::currentGame;
  int dispPeriod = g.inIntermission ? g.period + 1 : g.period;
  int home = d.homeScore, away = d.awayScore;
  if (blankBlinkSide) {
    if (sBlinkSide & 0x1) home = MAX7219_SCORE_BLANK;
    if (sBlinkSide & 0x2) away = MAX7219_SCORE_BLANK;
  }
  if (strcmp(g.periodType, "SO") == 0) {
    setMax7219Shootout(home, away);  // shootout: SO on the period module
    return;
  }
  setMax7219Display(home, away,
                    dispPeriod >= 1 ? dispPeriod : MAX7219_SCORE_BLANK);
}

void startScoreBlink(uint8_t side) {
  sBlinkSide |= side;
  sBlinkStart = millis();
  sBlinkPhase = 0;   // begins in the ON (score showing) half
}

// ---- Penalty countdown tick ----
// Penalty remaining times ride the period clock: they count down only
// while the game clock runs, freeze with it, and re-anchor to the feed's
// remainSec on every 5 s poll (the data task derives remainSec from the
// same game-elapsed math below).
int sPenElapsedBase = 0;  // game elapsed seconds at the last poll
int sPenElapsedTick = 0;  // locally ticked game elapsed seconds

int periodLengthOf(int period) { return period <= 3 ? 1200 : 300; }

int gameElapsedOf(int period, int secRemaining) {
  int e = 0;
  for (int p = 1; p < period; ++p) e += periodLengthOf(p);
  return e + (periodLengthOf(period) - secRemaining);
}

// Displayed remaining seconds for a penalty: the polled remainSec minus
// the game time ticked since that poll (never negative).
int displayRemainOf(const NhlPenalty& p) {
  int r = p.remainSec - (sPenElapsedTick - sPenElapsedBase);
  return r > 0 ? r : 0;
}

// Repaints just the right-aligned time cell of each penalty row (called
// once per second from tickLiveClock while penalties are on screen).
void refreshPenaltyTimes() {
  const GameSnapshot& g = nhl_render::currentGame;
  int y = BOTTOM_Y + 24;
  for (int i = 0; i < g.penaltyCount && i < 3; ++i) {
    char rem[8];
    snprintf(rem, sizeof(rem), "%d:%02d", displayRemainOf(g.penalties[i]) / 60,
             displayRemainOf(g.penalties[i]) % 60);
    canvas().fillRect(244, y, 76, 18, COLOR_BG);
    canvas().setTextColor(COLOR_GOLD);
    canvas().setTextSize(2);
    canvas().setCursor(308 - (int)strlen(rem) * 12, y);
    canvas().print(rem);
    tftPanel.pushRows(244, y - 2, 80, 22);
    y += 20;
  }
}

// Clock sync strategy (the 1 Hz display tick itself is tickLiveClock):
// - While feed and display agree play is running, the display ticks on
//   wall time and is NEVER re-anchored — transport staleness (the polled
//   value is ~0-3 s old, i.e. slightly ABOVE the display) is absorbed,
//   not corrected, so the clock cannot stutter mid-play.
// - A stoppage is caught within one poll: the polled value stops
//   dropping against the ticking display (drift turns positive and
//   grows). The display freezes immediately, capping any over-tick at
//   CLOCK_NORMAL_DRIFT seconds.
// - The displayed value never increases ("rewinds") except one
//   deliberate case: accumulated error beyond CLOCK_MAX_LEAD snaps to
//   feed truth in a single visible correction. Small leads persist and
//   re-anchor for free at the next stoppage. Resumes anchor to the
//   lower of feed/display so puck drop never rewinds the clock.
const int CLOCK_NORMAL_DRIFT = 3;  // s: feed transport-latency allowance
const int CLOCK_MAX_LEAD = 5;      // s: max display-ahead before a snap

void syncClockModel(const GameSnapshot& g) {
  uint32_t now = millis();
  if (g.inIntermission) {
    // Between periods: display the NEXT period already — its opening
    // clock (20:00 regulation / 5:00 OT), frozen until puck drop.
    sClockBasisSec = periodLengthOf((g.period > 0 ? g.period : 3) + 1);
    sClockSyncedAt = now;
    sClockRunning = false;
    return;
  }
  int fresh = g.clockSec < 0 ? 0 : g.clockSec;
  int local = sClockRunning
                  ? sClockBasisSec - (int)((now - sClockSyncedAt) / 1000)
                  : sClockBasisSec;
  if (local < 0) local = 0;
  int drift = fresh - local;  // > 0: feed value above the display

  if (!g.clockRunning) {
    // Stopped per feed: freeze. Hold the displayed value when the gap is
    // small (no visible jump); snap only past CLOCK_MAX_LEAD.
    sClockBasisSec = (drift > CLOCK_MAX_LEAD)
                         ? fresh
                         : (sClockRunning ? local : sClockBasisSec);
    sClockRunning = false;
  } else if (!sClockRunning) {
    // Resume: take whichever is lower so the display never rewinds.
    sClockBasisSec = (drift > CLOCK_MAX_LEAD) ? fresh : local;
    sClockRunning = true;
  } else if (drift > CLOCK_NORMAL_DRIFT) {
    // Feed well above the ticking display: the clock actually stopped
    // while the running flag lagged. Freeze NOW (caps the over-tick);
    // snap only past CLOCK_MAX_LEAD.
    sClockBasisSec = (drift > CLOCK_MAX_LEAD) ? fresh : local;
    sClockRunning = false;
  } else {
    // Normal running: keep the ticking estimate untouched — smoothness.
    sClockBasisSec = local;
  }
  sClockSyncedAt = now;
}

int liveClockSec() {
  if (!sClockRunning) return sClockBasisSec;
  int v = sClockBasisSec - (int)((millis() - sClockSyncedAt) / 1000);
  return v > 0 ? v : 0;
}

// One team column: logo at the top, then the scoreboard-style shots box
// — white rounded border, big gold count. The "SHOTS" caption lives
// between the two boxes (drawShotsCaption), not inside them. Deliberately
// static in every state: goal feedback blinks the score MATRIX instead.
void drawTeamColumn(int logoX, const char* abbrev, int teamId, int shots) {
  ensureLogoCached(teamId, abbrev);
  drawTeamLogoScaled(canvas(), logoX, LOGO_Y, teamId, abbrev, LOGO_SIZE);
  const int bx = logoX - SHOTS_BOX_PAD, bw = LOGO_SIZE + 2 * SHOTS_BOX_PAD;
  const int cx = logoX + LOGO_SIZE / 2;
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), abbrev, cx, ABBREV_Y);
  // 2-px white border (arena-stat-panel look)
  canvas().drawRoundRect(bx, SHOTS_BOX_Y, bw, SHOTS_BOX_H, 8, ST77XX_WHITE);
  canvas().drawRoundRect(bx + 1, SHOTS_BOX_Y + 1, bw - 2, SHOTS_BOX_H - 2, 8,
                         ST77XX_WHITE);
  // Count: wipe the number region first, then draw each digit in its own
  // fixed cell. Text renders transparently on this canvas, so without the
  // wipe every value change (9 -> 10, 12 -> 13) smears the old glyph
  // remnants into the new number; fixed cells keep multi-digit counts
  // evenly spaced.
  char n[4];
  snprintf(n, sizeof(n), "%d", shots < 0 ? 0 : shots);
  canvas().fillRect(bx + 3, SHOTS_NUM_Y - 2, bw - 6,
                    SHOTS_BOX_Y + SHOTS_BOX_H - 3 - (SHOTS_NUM_Y - 2),
                    COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(4);
  const int kDigitCell = 30;  // 20-px size-4 glyph + breathing room
  int digits = (int)strlen(n);
  int x0 = cx - (digits * kDigitCell) / 2;
  for (int i = 0; i < digits; ++i) {
    canvas().setCursor(x0 + i * kDigitCell + (kDigitCell - 20) / 2,
                       SHOTS_NUM_Y);
    canvas().print(n[i]);
  }
  tftPanel.pushRows(logoX - SHOTS_BOX_PAD - 2, LOGO_Y - 2,
                    LOGO_SIZE + 2 * SHOTS_BOX_PAD + 4,
                    SHOTS_BOX_Y + SHOTS_BOX_H + 2 - (LOGO_Y - 2));
}

// "SHOTS" centered between the two boxes, vertically aligned with them.
// Static text, so it is only repainted on full paints (new game, or the
// restore after the goal announcement owned the screen) — the column
// redraws above never touch this region.
void drawShotsCaption() {
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), "SHOTS", 160,
                   SHOTS_BOX_Y + (SHOTS_BOX_H - 16) / 2);
}

bool liveish(const char* state) {
  return strcmp(state, "LIVE") == 0 || strcmp(state, "CRIT") == 0;
}

bool gameOverState(const char* state) {
  return strcmp(state, "FINAL") == 0 || strcmp(state, "OFF") == 0 ||
         strcmp(state, "OVER") == 0;
}

// Bottom half once the game ends: GAME OVER centered in white plus, once
// the feed publishes them, the three stars of the game — for the
// postgame grace window.
void drawGameOverHalf(const GameSnapshot& g) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  if (g.threeStarCount == 0) {
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(3);
    drawCenteredText(canvas(), "GAME OVER", 160, BOTTOM_Y + 26);
    tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
    return;
  }
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), "GAME OVER", 160, BOTTOM_Y + 2);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  drawCenteredText(canvas(), "THREE STARS", 160, BOTTOM_Y + 20);
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  int y = BOTTOM_Y + 32;
  for (int i = 0; i < g.threeStarCount && i < 3; ++i) {
    const NhlThreeStar& st = g.threeStars[i];
    char line[36];
    char who[24];
    if (st.number > 0) snprintf(who, sizeof(who), "#%d %s", st.number, st.name);
    else strlcpy(who, st.name, sizeof(who));
    snprintf(line, sizeof(line), "%d. %s %s %dG-%dA", i + 1, who,
             st.teamAbbrev, st.goals, st.assists);
    int chars = strlen(line) > 26 ? 26 : strlen(line);
    canvas().setCursor(160 - chars * 6, y);
    printClipped(canvas(), line, 26);
    y += 18;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

void drawGoalAnnouncement() {
  canvas().fillScreen(COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(5);
  drawCenteredText(canvas(), "GOAL!!!", 160, 104);
  tftPanel.pushFull();
}

void drawGoalDetails() {
  canvas().fillScreen(COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(2);
  if (!sDisplayedGoal.valid) {
    drawCenteredText(canvas(), "SCORER DETAILS", 160, 72);
    drawCenteredText(canvas(), "UNAVAILABLE", 160, 106);
    tftPanel.pushFull();
    return;
  }
  drawCenteredText(canvas(), "GOAL SCORED BY", 160, 38);

  char line[52];
  snprintf(line, sizeof(line), "#%d %s",
           sDisplayedGoal.scorer.number > 0
               ? sDisplayedGoal.scorer.number : 0,
           sDisplayedGoal.scorer.name[0] != '\0'
               ? sDisplayedGoal.scorer.name : "Unknown");
  if (sDisplayedGoal.scorer.number <= 0) {
    snprintf(line, sizeof(line), "#? %s",
             sDisplayedGoal.scorer.name[0] != '\0'
                 ? sDisplayedGoal.scorer.name : "Unknown");
  }
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  int scorerChars = strlen(line) > 25 ? 25 : strlen(line);
  canvas().setCursor(160 - scorerChars * 6, 82);
  printClipped(canvas(), line, 25);

  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  drawCenteredText(canvas(), sDisplayedGoal.assistCount ? "ASSISTS" : "UNASSISTED",
                   160, 130);
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  if (sDisplayedGoal.assistCount == 0) {
    drawCenteredText(canvas(), "No assists", 160, 164);
  } else {
    for (int i = 0; i < sDisplayedGoal.assistCount; ++i) {
      const NhlGoalPlayer& assist = sDisplayedGoal.assists[i];
      if (assist.name[0] == '\0') continue;
      if (assist.number > 0) {
        snprintf(line, sizeof(line), "#%d %s", assist.number, assist.name);
      } else {
        snprintf(line, sizeof(line), "#? %s", assist.name);
      }
      int chars = strlen(line) > 25 ? 25 : strlen(line);
      canvas().setCursor(160 - chars * 6, 150 + i * 24);
      printClipped(canvas(), line, 25);
    }
  }
  tftPanel.pushFull();
}

// Bottom half, penalty view: team, sweater number + last name, and the
// remaining penalty time for every active penalty.
void drawPenaltiesHalf(const GameSnapshot& g) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, BOTTOM_Y + 4);
  canvas().print("PENALTIES");
  int y = BOTTOM_Y + 24;
  for (int i = 0; i < g.penaltyCount && i < 3; ++i) {
    const NhlPenalty& p = g.penalties[i];
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    canvas().setCursor(16, y + 6);
    canvas().print(p.teamAbbrev);
    char who[22];
    snprintf(who, sizeof(who), "#%d %s", p.number, p.lastName);
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    canvas().setCursor(52, y);
    printClipped(canvas(), who, 14);
    char rem[8];
    snprintf(rem, sizeof(rem), "%d:%02d", displayRemainOf(p) / 60,
             displayRemainOf(p) % 60);
    canvas().setTextColor(COLOR_GOLD);
    canvas().setCursor(308 - (int)strlen(rem) * 12, y);
    canvas().print(rem);
    y += 20;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Bottom half, even-strength view: scores of the league's in-progress
// games (the followed game excluded by updateOtherGames). Three rows per
// page; when more than three games are on, the window rotates through
// them one game per ~5 s (driven by tickLiveClock).
size_t sLeagueStart = 0;        // first live game on the current page
size_t sLeagueLiveCount = 0;    // live games this draw
uint32_t sLeagueRotateAt = 0;   // millis deadline for the next rotation

// Bottom-half mode. Priority: GAME OVER > penalties > linescore/league.
// During penalty-free play the linescore and the league view alternate
// (sRotatePair, once other games are live); intermission locks the
// linescore in — penalties are hidden between periods, and without this
// the strip used to sit on a bare PENALTIES header all intermission.
enum class BottomMode : uint8_t { OVER, PENS, LINESCORE, LEAGUE };
BottomMode sBottomMode = BottomMode::LINESCORE;
bool sRotatePair = false;       // penalty-free play: alternate the pair
uint32_t sViewFlipAt = 0;       // millis deadline for the next flip

void drawLeagueHalf() {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, BOTTOM_Y + 4);
  canvas().print("AROUND THE LEAGUE");
  // Collect the live games once so the start index can wrap cleanly.
  const OtherGameInfo* live[MAX_OTHER_GAMES];
  size_t n = 0;
  for (size_t i = 0; i < otherGameCount; ++i) {
    if (liveish(otherGames[i].gameState)) live[n++] = &otherGames[i];
  }
  sLeagueLiveCount = n;
  sLeagueRotateAt = millis() + NHL_CAROUSEL_ROTATE_MS;
  if (sLeagueStart >= n) sLeagueStart = 0;
  int y = BOTTOM_Y + 24;
  for (size_t r = 0; r < 3 && n > 0; ++r) {
    const OtherGameInfo& o = *live[(sLeagueStart + r) % n];
    char line[20];
    snprintf(line, sizeof(line), "%s %2d - %2d %s", o.awayAbbrev, o.awayScore,
             o.homeScore, o.homeAbbrev);
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), line, 160, y);
    y += 20;
  }
  if (n == 0) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    drawCenteredText(canvas(), "No other games in progress", 160,
                     BOTTOM_Y + 44);
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Bottom half, linescore view: per-period goals for both teams plus the
// game total (the authoritative scores — shootout-decided games show a
// total the period columns don't sum to). Columns 1/2/3 always, OT/SO
// only once they exist. Away row on top, home below (broadcast order).
void drawLinescoreHalf(const GameSnapshot& g, bool intermission) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  if (intermission) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    drawCenteredText(canvas(), "INTERMISSION", 160, BOTTOM_Y + 2);
  }
  const char* labels[5] = {"1", "2", "3", "OT", "SO"};
  const uint8_t* rowGoals[2] = {g.awayPeriodGoals, g.homePeriodGoals};
  const char* abbrevs[2] = {g.awayAbbrev, g.homeAbbrev};
  int totals[2] = {g.awayScore, g.homeScore};
  int cols = 3 + (g.otColumn ? 1 : 0) + (g.soColumn ? 1 : 0);
  int colX[5];
  for (int i = 0; i < cols; ++i)
    colX[i] = 76 + i * (252 - 76) / (cols - 1);
  int y0 = BOTTOM_Y + (intermission ? 18 : 6);
  canvas().setTextColor(COLOR_MUTED);
  canvas().setTextSize(1);
  for (int i = 0; i < cols; ++i)
    drawCenteredText(canvas(), labels[i], colX[i], y0);
  drawCenteredText(canvas(), "T", 284, y0);
  for (int r = 0; r < 2; ++r) {
    int y = y0 + 16 + r * 22;
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    canvas().setCursor(12, y);
    canvas().print(abbrevs[r]);
    for (int i = 0; i < cols; ++i) {
      char v[4];
      snprintf(v, sizeof(v), "%d", (int)rowGoals[r][i]);
      drawCenteredText(canvas(), v, colX[i], y);
    }
    char t[4];
    snprintf(t, sizeof(t), "%d", totals[r]);
    canvas().setTextColor(COLOR_GOLD);
    canvas().setCursor(296 - (int)strlen(t) * 12, y);
    canvas().print(t);
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Picks and draws the current bottom-half mode. Called from the
// signature-driven redraw in renderLiveGame (and the initial full paint),
// so every state and content change funnels through here.
void drawBottomHalf(const GameSnapshot& g) {
  if (gameOverState(g.gameState)) {
    sBottomMode = BottomMode::OVER;
    sRotatePair = false;
    drawGameOverHalf(g);
    return;
  }
  if (!g.inIntermission && g.penaltyCount > 0) {
    sBottomMode = BottomMode::PENS;
    sRotatePair = false;
    drawPenaltiesHalf(g);
    return;
  }
  size_t live = 0;
  for (size_t i = 0; i < otherGameCount; ++i)
    if (liveish(otherGames[i].gameState)) ++live;
  sRotatePair = !g.inIntermission && live > 0;
  if (sRotatePair) {
    // Stay on whichever view of the pair is up across content refreshes;
    // arm the flip timer only when entering the pair from another mode.
    if (sBottomMode != BottomMode::LINESCORE &&
        sBottomMode != BottomMode::LEAGUE) {
      sViewFlipAt = millis() + NHL_BOTTOM_VIEW_FLIP_MS;
    }
    if (sBottomMode == BottomMode::LEAGUE) {
      drawLeagueHalf();
      return;
    }
  }
  sBottomMode = BottomMode::LINESCORE;
  drawLinescoreHalf(g, g.inIntermission);
}

}  // namespace

void renderLiveGame(const GameSnapshot& g) {  nhl_render::currentGame = g;
  nhl_render::hasCurrentLiveGame = true;

  bool newGame = (g.gameId != d.gameId);
  bool teamsChanged = newGame || strcmp(g.homeAbbrev, d.homeAbbrev) != 0 ||
                      strcmp(g.awayAbbrev, d.awayAbbrev) != 0;

  if (newGame || teamsChanged) {
    d.gameId = g.gameId;
    strlcpy(d.homeAbbrev, g.homeAbbrev, sizeof(d.homeAbbrev));
    strlcpy(d.awayAbbrev, g.awayAbbrev, sizeof(d.awayAbbrev));
    d.homeTeamId = g.homeTeamId;
    d.awayTeamId = g.awayTeamId;
    sBlinkSide = 0;
    d.homeScore = d.awayScore = -1;   // force change-driven redraws
    d.sogH = d.sogA = -1;
    d.penaltySig[0] = '\0';
    if (!goalDisplayActive()) {
      canvas().fillScreen(COLOR_BG);
      drawTeamColumn(HOME_LOGO_X, g.homeAbbrev, g.homeTeamId, g.homeSog);
      drawTeamColumn(GUEST_LOGO_X, g.awayAbbrev, g.awayTeamId, g.awaySog);
      drawShotsCaption();
      drawBottomHalf(g);
      tftPanel.pushFull();
    }
  }

  // matrices + goal flash — the score lives on the matrices; a score
  // INCREASE (goal) blinks that team's NEW score on its matrix (the
  // SOG panels never change appearance — goal feedback is matrix-only).
  bool homeGoal = false, awayGoal = false;
  if (g.awayScore != d.awayScore) {
    awayGoal = g.awayScore > d.awayScore && d.awayScore >= 0;
    d.awayScore = g.awayScore;
  }
  if (g.homeScore != d.homeScore) {
    homeGoal = g.homeScore > d.homeScore && d.homeScore >= 0;
    d.homeScore = g.homeScore;
  }
  // A goal starts the score blink on that side's matrix (the blink
  // driver in tickLiveClock owns the matrices until it finishes);
  // otherwise scores on the outer modules, period on the middle one.
  if (awayGoal) startScoreBlink(0x2);
  if (homeGoal) startScoreBlink(0x1);
  if (sBlinkSide == 0) applyScoreMatrices(false);
  setCountLeds(g.inIntermission ? 0 : g.homePenaltyCount,
               g.inIntermission ? 0 : g.awayPenaltyCount, 0);

  // TM1637 + TFT header clock: period clock MM:SS — colon always lit
  // (board convention: the clock display keeps its colon in every mode
  // and reading). The model re-syncs from this poll; between polls
  // tickLiveClock() counts it down while the game clock runs.
  syncClockModel(g);
  int clk = liveClockSec();
  tm1637ShowPair(clk / 60, clk % 60, true);
  // Penalty countdown re-anchors to the feed every poll.
  sPenElapsedBase = sPenElapsedTick =
      gameElapsedOf(g.period > 0 ? g.period : 1, g.clockSec);

  d.clockSec = clk;  // second tracker for tickLiveClock's TM1637 updates

  // SOG panels — redrawn only when the value changes; the look never
  // changes (goal feedback lives on the matrices, not here).
  if (g.homeSog != d.sogH) {
    d.sogH = g.homeSog;
    if (!goalDisplayActive())
      drawTeamColumn(HOME_LOGO_X, g.homeAbbrev, g.homeTeamId, g.homeSog);
  }
  if (g.awaySog != d.sogA) {
    d.sogA = g.awaySog;
    if (!goalDisplayActive())
      drawTeamColumn(GUEST_LOGO_X, g.awayAbbrev, g.awayTeamId, g.awaySog);
  }

  // bottom half: penalties while any are active, the linescore during
  // intermission, and during penalty-free play the alternating
  // linescore / league-scores pair. Signature covers every mode's
  // contents so the view flips and refreshes only on real changes.
  char sig[256] = "";
  if (gameOverState(g.gameState)) {
    strlcpy(sig, "G", sizeof(sig));  // postgame: GAME OVER + three stars
    for (int i = 0; i < g.threeStarCount; ++i) {
      const NhlThreeStar& st = g.threeStars[i];
      snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%d%d",
               st.name, st.number, st.goals, st.assists);
    }
  } else {
    int penCount = g.inIntermission ? 0 : g.penaltyCount;  // hidden between periods
    if (penCount > 0) {
      strlcpy(sig, "P", sizeof(sig));
      for (int i = 0; i < g.penaltyCount && i < 4; ++i) {
        const NhlPenalty& p = g.penalties[i];
        snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%s%d",
                 p.teamAbbrev, p.number, p.lastName, p.remainSec);
      }
    } else {
      snprintf(sig, sizeof(sig), "X%s%d%d",
               g.inIntermission ? "I" : "R", g.homeScore, g.awayScore);
      for (int i = 0; i < 5; ++i)
        snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "%d%d",
                 (int)g.homePeriodGoals[i], (int)g.awayPeriodGoals[i]);
      snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "%d%d",
               g.otColumn ? 1 : 0, g.soColumn ? 1 : 0);
      if (!g.inIntermission) {
        for (size_t i = 0; i < otherGameCount; ++i) {
          const OtherGameInfo& o = otherGames[i];
          if (!liveish(o.gameState)) continue;
          snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%d%s",
                   o.awayAbbrev, o.awayScore, o.homeScore, o.homeAbbrev);
        }
      }
    }
  }
  if (strcmp(sig, d.penaltySig) != 0) {
    strlcpy(d.penaltySig, sig, sizeof(d.penaltySig));
    if (!goalDisplayActive()) drawBottomHalf(g);
  }

  // Display-binding trace (DBG-gated): one line per fresh snapshot showing
  // exactly what each output driver received — including the period-matrix
  // and TM1637 clock values reserved for the topography wiring to come.
  DBG_PRINTF("[BIND] %s %s %d-%s %d | matrices A%d H%d | clock P%d%s %02d:%02d"
             " | TM1637 %02d:%02d | periodMx %d | LEDs H%d/A%d | SOG %d-%d | pens %d\n",
             g.gameState, g.awayAbbrev, g.awayScore, g.homeAbbrev, g.homeScore,
             g.awayScore, g.homeScore, g.period, g.periodType, g.clockSec / 60,
             g.clockSec % 60, g.clockSec / 60, g.clockSec % 60, g.period,
             g.homePenaltyCount, g.awayPenaltyCount, g.awaySog, g.homeSog,
             g.penaltyCount);
}

void startGoalAnnouncement(const NhlGoal& goal) {
  sDisplayedGoal = goal;
  sGoalDisplayPhase = GoalDisplayPhase::ANNOUNCEMENT;
  sGoalDisplayStartedAt = millis();
  drawGoalAnnouncement();
}

void updateGoalAnnouncement(const NhlGoal& goal) {
  if (!goalDisplayActive() || sDisplayedGoal.eventId != goal.eventId) return;
  sDisplayedGoal = goal;
  if (sGoalDisplayPhase == GoalDisplayPhase::DETAILS) drawGoalDetails();
}

void forceLiveRepaint(const GameSnapshot& g) {
  sGoalDisplayPhase = GoalDisplayPhase::NONE;
  d = Drawn{};  // drop every cached value so the whole screen repaints
  renderLiveGame(g);
}

void tickLiveClock() {
  // Called every loop pass during a live game: advances the displayed
  // clock one second at a time between the 5 s landing polls (frozen
  // while the game clock is stopped), and lets the goal flash expire
  // back to the normal score box. No-op without a live game.
  if (!nhl_render::hasCurrentLiveGame) {
    sBlinkSide = 0;
    sGoalDisplayPhase = GoalDisplayPhase::NONE;
    return;
  }
  uint32_t now = millis();
  if (sGoalDisplayPhase == GoalDisplayPhase::ANNOUNCEMENT &&
      now - sGoalDisplayStartedAt >= NHL_GOAL_ANNOUNCEMENT_MS) {
    sGoalDisplayPhase = GoalDisplayPhase::DETAILS;
    sGoalDisplayStartedAt = now;
    drawGoalDetails();
  } else if (sGoalDisplayPhase == GoalDisplayPhase::DETAILS &&
             now - sGoalDisplayStartedAt >= NHL_GOAL_DETAILS_MS) {
    sGoalDisplayPhase = GoalDisplayPhase::NONE;
    forceLiveRepaint(nhl_render::currentGame);
  }
  // Score blink: 250 ms on / 250 ms off x3 on the scoring side's matrix,
  // then back to the steady display.
  if (sBlinkSide != 0) {
    uint8_t phase = (now - sBlinkStart) / NHL_SCORE_BLINK_HALF_MS;
    if (phase >= 2 * NHL_SCORE_BLINK_FLASHES) {
      sBlinkSide = 0;
      applyScoreMatrices(false);
    } else if (phase != sBlinkPhase) {
      sBlinkPhase = phase;
      applyScoreMatrices(phase % 2 == 1);
    }
  }
  // Stale-snapshot continuation: with no fresh landing data for 90 s
  // (18 missed polls), the frozen display is worse than a guess — resume
  // the clock and let penalties expire on their own until data returns.
  // Real stoppages never trip this: every successful poll refreshes
  // sClockSyncedAt.
  if (!sClockRunning && sClockSyncedAt != 0 &&
      now - sClockSyncedAt > NHL_LANDING_STALE_MS) {
    sClockRunning = true;
    sClockSyncedAt = now;
  }
  // Penalty-free play alternates the linescore and league views on the
  // bottom half. The flip timer is only armed when the pair is entered
  // (drawBottomHalf), so league-score refreshes never postpone a flip.
  if (!goalDisplayActive() && sRotatePair &&
      (int32_t)(now - sViewFlipAt) >= 0) {
    sBottomMode = (sBottomMode == BottomMode::LEAGUE) ? BottomMode::LINESCORE
                                                      : BottomMode::LEAGUE;
    sViewFlipAt = now + NHL_BOTTOM_VIEW_FLIP_MS;
    if (sBottomMode == BottomMode::LEAGUE) drawLeagueHalf();
    else drawLinescoreHalf(nhl_render::currentGame, false);
  }
  // League page rotation (>3 live games): only while the league view is
  // actually on screen — the timer re-arms inside drawLeagueHalf.
  if (!goalDisplayActive() && sBottomMode == BottomMode::LEAGUE &&
      sLeagueLiveCount > 3 && (int32_t)(now - sLeagueRotateAt) >= 0) {
    sLeagueStart = (sLeagueStart + 1) % sLeagueLiveCount;
    drawLeagueHalf();
  }
  int clk = liveClockSec();
  if (clk == d.clockSec) return;
  d.clockSec = clk;
  tm1637ShowPair(clk / 60, clk % 60, true);
  if (sClockRunning) {
    ++sPenElapsedTick;  // penalties run with the period clock
    if (!goalDisplayActive() && sBottomMode == BottomMode::PENS)
      refreshPenaltyTimes();
  }
}

// ---- Manual mode ------------------------------------------------------------
// TFT shows ONLY the two per-team shot cards — no mode header, no period
// or clock text (those live on the matrices / TM1637).
namespace {
struct ManualDrawn {
  bool drawn = false;
  int homeShots = -1, guestShots = -1;
  int homeScore = -1, guestScore = -1, period = -1, penMask = -1;
} md;
}  // namespace

void invalidateManualScreen() { md = ManualDrawn{}; }

void renderManual() {
  GFXcanvas16& c = tftPanel.canvas();

  if (!md.drawn) {
    md.drawn = true;
    c.fillScreen(COLOR_BG);
    for (int t = 0; t < 2; ++t) {
      int x = t ? 166 : 8;
      c.fillRoundRect(x, 8, 146, 224, 8, COLOR_CARD);
      c.drawRoundRect(x, 8, 146, 224, 8, COLOR_MUTED);
      c.setTextColor(ST77XX_WHITE);
      c.setTextSize(3);
      drawCenteredText(c, t ? "GUEST" : "HOME", x + 73, 26);
      c.setTextColor(COLOR_MUTED);
      c.setTextSize(2);
      drawCenteredText(c, "SHOTS", x + 73, 62);
    }
    tftPanel.pushFull();
  }

  // big per-team shot counts (the manual screen's whole content)
  auto drawShots = [&](int cardX, int shots, int& cache) {
    if (shots == cache) return;
    cache = shots;
    char s[4];
    snprintf(s, sizeof(s), "%d", shots);
    c.fillRect(cardX + 8, 92, 130, 90, COLOR_CARD);
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(7);
    int w = strlen(s) * 42;
    c.setCursor(cardX + (146 - w) / 2, 108);
    c.print(s);
    tftPanel.pushRows(cardX + 4, 88, 138, 98);
  };
  drawShots(8, getManualHomeShots(), md.homeShots);
  drawShots(166, getManualGuestShots(), md.guestShots);

  // hardware: matrices (scores + period), TM1637 clock, penalty LEDs
  int sec = getManualClockSec();
  int per = getManualPeriod();
  int hs = getManualHomeScore(), gs = getManualGuestScore();
  if (hs != md.homeScore || gs != md.guestScore || per != md.period) {
    md.homeScore = hs;
    md.guestScore = gs;
    md.period = per;
    setMax7219Display(hs, gs, per);
  }
  tm1637ShowPair(sec / 60, sec % 60, true);
  int mask = getManualPenaltyMask();
  if (mask != md.penMask) {
    md.penMask = mask;
    digitalWrite(PENALTY_HOME1_PIN, mask & 0x1 ? HIGH : LOW);
    digitalWrite(PENALTY_HOME2_PIN, mask & 0x2 ? HIGH : LOW);
    digitalWrite(PENALTY_AWAY1_PIN, mask & 0x4 ? HIGH : LOW);
    digitalWrite(PENALTY_AWAY2_PIN, mask & 0x8 ? HIGH : LOW);
  }
}
