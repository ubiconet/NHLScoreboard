#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/app/power_monitor.h"
#include "common/comms/network_service.h"
#include "common/hal/audio.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/battery_indicator.h"
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
//                the alternating linescore / league-scores strip; between
//                periods and after the final, the break-page rotation
//                (linescore, three stars, scoring, team stats, league)
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
  int battQuarters = -1;  // last battery-gauge state (-1 = hidden/unknown)
  char penaltySig[384] = "";
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

// Goal blink: the scoring team's NEW score flashes INVERTED (reverse
// video) on its matrix alone (NHL_SCORE_BLINK_FLASHES x 250 ms inverted /
// 250 ms normal), then returns to the steady display. bit 1 = home,
// bit 2 = guest.
uint32_t sBlinkStart = 0;
uint8_t sBlinkSide = 0;    // 0 = idle
uint8_t sBlinkPhase = 0;   // last applied half-period index

void applyScoreMatrices(bool invertBlinkSide) {
  const GameSnapshot& g = nhl_render::currentGame;
  // The period module holds the period being (or last) played: through an
  // intermission and after the final it stays on the period just ended.
  int per = g.period >= 1 ? g.period : MAX7219_SCORE_BLANK;
  int home = d.homeScore, away = d.awayScore;
  bool invHome = invertBlinkSide && (sBlinkSide & 0x1);
  bool invGuest = invertBlinkSide && (sBlinkSide & 0x2);
  if (strcmp(g.periodType, "SO") == 0) {
    setMax7219Shootout(home, away, invHome, invGuest);  // shootout: SO on the period module
    return;
  }
  if (invHome || invGuest) {
    setMax7219ScoreFlash(home, away, per, invHome, invGuest);
  } else {
    setMax7219Display(home, away, per);
  }
}

void startScoreBlink(uint8_t side) {
  sBlinkSide |= side;
  sBlinkStart = millis();
  sBlinkPhase = 0;   // begins in the normal (un-inverted) half
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

bool gameOverState(const char* state);  // defined below the draw helpers

void syncClockModel(const GameSnapshot& g) {
  uint32_t now = millis();
  if (gameOverState(g.gameState) || g.inIntermission) {
    // Period or game over: the timer reads 00:00 — never the frozen last
    // tick (the feed can stop the clock above zero when the winning goal
    // comes with time remaining) and not the next period's opening clock.
    // The first running snapshot at the next puck drop re-anchors past
    // CLOCK_MAX_LEAD, so 20:00 snaps in cleanly.
    sClockBasisSec = 0;
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
void drawShotCount(GFXcanvas16& c, int centerX, int y, int shots) {
  char n[4];
  snprintf(n, sizeof(n), "%d", shots < 0 ? 0 : shots);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(4);
  const int kDigitCell = 30;
  int digits = (int)strlen(n);
  int x0 = centerX - (digits * kDigitCell) / 2;
  for (int i = 0; i < digits; ++i) {
    c.setCursor(x0 + i * kDigitCell + (kDigitCell - 20) / 2, y);
    c.print(n[i]);
  }
}

void drawShotsBox(GFXcanvas16& c, int centerX) {
  const int boxWidth = LOGO_SIZE + 2 * SHOTS_BOX_PAD;
  const int boxX = centerX - boxWidth / 2;
  c.drawRoundRect(boxX, SHOTS_BOX_Y, boxWidth, SHOTS_BOX_H, 8, ST77XX_WHITE);
  c.drawRoundRect(boxX + 1, SHOTS_BOX_Y + 1, boxWidth - 2,
                  SHOTS_BOX_H - 2, 8, ST77XX_WHITE);
}

void drawTeamColumn(int logoX, const char* abbrev, int teamId, int shots) {
  ensureLogoCached(teamId, abbrev);
  drawTeamLogoScaled(canvas(), logoX, LOGO_Y, teamId, abbrev, LOGO_SIZE);
  const int bx = logoX - SHOTS_BOX_PAD, bw = LOGO_SIZE + 2 * SHOTS_BOX_PAD;
  const int cx = logoX + LOGO_SIZE / 2;
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), abbrev, cx, ABBREV_Y);
  drawShotsBox(canvas(), cx);
  // Count: wipe the number region first, then draw each digit in its own
  // fixed cell. Text renders transparently on this canvas, so without the
  // wipe every value change (9 -> 10, 12 -> 13) smears the old glyph
  // remnants into the new number; fixed cells keep multi-digit counts
  // evenly spaced.
  canvas().fillRect(bx + 3, SHOTS_NUM_Y - 2, bw - 6,
                    SHOTS_BOX_Y + SHOTS_BOX_H - 3 - (SHOTS_NUM_Y - 2),
                    COLOR_BG);
  drawShotCount(canvas(), cx, SHOTS_NUM_Y, shots);
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

// Postgame break page: the three stars of the game (only in the rotation
// once the feed publishes them, at or near game end).
void drawThreeStarsHalf(const GameSnapshot& g) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
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

// Bottom half, even-strength view: every other game of the day (the
// followed game excluded by updateOtherGames) — live ones highlighted,
// previews and finals included so all teams playing today appear. Each
// game is one line; three rows per page, and when more than three games
// are on the window rotates through them one game per ~5 s (driven by
// tickLiveClock).
size_t sLeagueStart = 0;        // first game on the current page
size_t sLeagueGameCount = 0;    // games in the rotation set this draw
uint32_t sLeagueRotateAt = 0;   // millis deadline for the next rotation

// Bottom-half mode. Priority: break pages > penalties > linescore/league.
// During penalty-free play the linescore and the league view alternate
// (sRotatePair, once other games are on the slate). Between periods and
// after the final the strip belongs to the BREAK rotation (drawBreakPage):
// linescore, three stars (postgame), scoring summary, team stats, and
// tonight's league scores, one page every NHL_BREAK_PAGE_MS.
enum class BottomMode : uint8_t { BREAK, PENS, LINESCORE, LEAGUE };
BottomMode sBottomMode = BottomMode::LINESCORE;
bool sRotatePair = false;       // penalty-free play: alternate the pair
uint32_t sViewFlipAt = 0;       // millis deadline for the next flip
size_t sBreakPage = 0;          // current page of the break rotation
uint32_t sBreakFlipAt = 0;      // millis deadline for the next break page

void drawLeagueRow(const OtherGameInfo& o, int y) {
  char line[20];
  snprintf(line, sizeof(line), "%s %2d - %2d %s", o.awayAbbrev, o.awayScore,
           o.homeScore, o.homeAbbrev);
  bool live = liveish(o.gameState);
  canvas().setTextColor(live ? ST77XX_WHITE : COLOR_MUTED);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), line, 160, y);
  canvas().setTextSize(1);
  if (live) {
    canvas().setTextColor(COLOR_LED_RED);
    canvas().setCursor(288, y + 4);
    canvas().print("LIVE");
  } else if (gameOverState(o.gameState)) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setCursor(280, y + 4);
    canvas().print("FINAL");
  }
}

void drawLeagueHalf() {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, BOTTOM_Y + 4);
  canvas().print("AROUND THE LEAGUE");
  size_t n = otherGameCount;  // whole slate minus the followed game
  sLeagueGameCount = n;
  sLeagueRotateAt = millis() + NHL_CAROUSEL_ROTATE_MS;
  if (sLeagueStart >= n) sLeagueStart = 0;
  int y = BOTTOM_Y + 24;
  for (size_t r = 0; r < 3 && r < n; ++r) {
    drawLeagueRow(otherGames[(sLeagueStart + r) % n], y);
    y += 20;
  }
  if (n == 0) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    drawCenteredText(canvas(), "No other games today", 160,
                     BOTTOM_Y + 44);
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Bottom half, linescore view: per-period goals for both teams plus the
// game total (the authoritative scores — shootout-decided games show a
// total the period columns don't sum to). Columns 1/2/3 always, OT/SO
// only once they exist. Away row on top, home below (broadcast order).
// A break label ("INTERMISSION" / "GAME OVER") heads the strip when set.
void drawLinescoreHalf(const GameSnapshot& g, const char* label) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  if (label != nullptr) {
    canvas().setTextColor(gameOverState(g.gameState) ? ST77XX_WHITE
                                                     : COLOR_GOLD);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), label, 160, BOTTOM_Y + 2);
  }
  const char* labels[5] = {"1", "2", "3", "OT", "SO"};
  const uint8_t* rowGoals[2] = {g.awayPeriodGoals, g.homePeriodGoals};
  const char* abbrevs[2] = {g.awayAbbrev, g.homeAbbrev};
  int totals[2] = {g.awayScore, g.homeScore};
  int cols = 3 + (g.otColumn ? 1 : 0) + (g.soColumn ? 1 : 0);
  int colX[5];
  for (int i = 0; i < cols; ++i)
    colX[i] = 76 + i * (252 - 76) / (cols - 1);
  int y0 = BOTTOM_Y + (label != nullptr ? 22 : 6);
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

// ---- Break pages (intermission + postgame) ----
// Shared header: page title on the left, the break state on the right.
void drawBreakHeader(const GameSnapshot& g, const char* title) {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  canvas().setTextSize(1);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setCursor(12, BOTTOM_Y + 4);
  canvas().print(title);
  const char* state =
      gameOverState(g.gameState) ? "GAME OVER" : "INTERMISSION";
  canvas().setTextColor(COLOR_MUTED);
  canvas().setCursor(308 - (int)strlen(state) * 6, BOTTOM_Y + 4);
  canvas().print(state);
}

// Scoring summary, three goals a page in game order: period-time, team
// (with a PP/SH/EN/PS tag beneath it), scorer.
void drawScoringPage(const GameSnapshot& g, size_t page, size_t pages) {
  char title[20];
  if (pages > 1) {
    snprintf(title, sizeof(title), "SCORING %u/%u", (unsigned)(page + 1),
             (unsigned)pages);
  } else {
    strlcpy(title, "SCORING", sizeof(title));
  }
  drawBreakHeader(g, title);
  if (g.goalCount == 0) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), "No goals yet", 160, BOTTOM_Y + 40);
  }
  int y = BOTTOM_Y + 24;
  for (size_t i = page * 3; i < g.goalCount && i < page * 3 + 3; ++i) {
    const NhlGoalLine& gl = g.goals[i];
    char when[12];
    if (strcmp(gl.periodType, "SO") == 0) {
      strlcpy(when, "SO", sizeof(when));
    } else if (strcmp(gl.periodType, "OT") == 0 || gl.period >= 4) {
      snprintf(when, sizeof(when), "OT-%s", gl.time);
    } else {
      snprintf(when, sizeof(when), "%d-%s", (int)gl.period, gl.time);
    }
    canvas().setTextColor(COLOR_GOLD);
    canvas().setTextSize(2);
    canvas().setCursor(8, y);
    canvas().print(when);
    canvas().setTextSize(1);
    canvas().setTextColor(COLOR_MUTED);
    canvas().setCursor(110, y - 1);
    canvas().print(gl.teamAbbrev);
    if (gl.tag[0] != '\0') {
      canvas().setTextColor(COLOR_YELLOW);
      canvas().setCursor(113, y + 8);
      canvas().print(gl.tag);
    }
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    canvas().setCursor(134, y);
    printClipped(canvas(), gl.name, 15);
    y += 20;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Team comparison: home values under the home logo (left), guest under
// the guest logo (right), stat label between them.
void drawTeamStatsPage(const GameSnapshot& g) {
  drawBreakHeader(g, "TEAM STATS");
  const NhlTeamStats& st = g.teamStats;
  const char* labels[4] = {"POWER PLAY", "FACEOFFS", "HITS", "BLOCKS"};
  char home[4][8], away[4][8];
  strlcpy(home[0], st.homePowerPlay[0] != '\0' ? st.homePowerPlay : "-",
          sizeof(home[0]));
  strlcpy(away[0], st.awayPowerPlay[0] != '\0' ? st.awayPowerPlay : "-",
          sizeof(away[0]));
  snprintf(home[1], sizeof(home[1]), "%d%%", (int)st.homeFaceoffPct);
  snprintf(away[1], sizeof(away[1]), "%d%%", (int)st.awayFaceoffPct);
  snprintf(home[2], sizeof(home[2]), "%d", (int)st.homeHits);
  snprintf(away[2], sizeof(away[2]), "%d", (int)st.awayHits);
  snprintf(home[3], sizeof(home[3]), "%d", (int)st.homeBlocks);
  snprintf(away[3], sizeof(away[3]), "%d", (int)st.awayBlocks);
  int y = BOTTOM_Y + 16;
  for (int r = 0; r < 4; ++r) {
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), home[r], HOME_LOGO_X + LOGO_SIZE / 2, y);
    drawCenteredText(canvas(), away[r], GUEST_LOGO_X + LOGO_SIZE / 2, y);
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    drawCenteredText(canvas(), labels[r], 160, y + 4);
    y += 16;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Tonight's league scores: only games that have started (live or final).
// The slate snapshot also carries later days' look-ahead games, which
// have no score to show.
bool otherGameStarted(const OtherGameInfo& o) {
  return liveish(o.gameState) || gameOverState(o.gameState);
}

size_t startedOtherGameCount() {
  size_t n = 0;
  for (size_t i = 0; i < otherGameCount; ++i)
    if (otherGameStarted(otherGames[i])) ++n;
  return n;
}

void drawLeagueBreakPage(const GameSnapshot& g, size_t page, size_t pages) {
  char title[28];
  if (pages > 1) {
    snprintf(title, sizeof(title), "AROUND THE LEAGUE %u/%u",
             (unsigned)(page + 1), (unsigned)pages);
  } else {
    strlcpy(title, "AROUND THE LEAGUE", sizeof(title));
  }
  drawBreakHeader(g, title);
  size_t idx = 0;
  int y = BOTTOM_Y + 24;
  for (size_t i = 0; i < otherGameCount; ++i) {
    if (!otherGameStarted(otherGames[i])) continue;
    if (idx >= page * 3 && idx < page * 3 + 3) {
      drawLeagueRow(otherGames[i], y);
      y += 20;
    }
    ++idx;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

size_t scoringPageCount(const GameSnapshot& g) {
  return g.goalCount == 0 ? 1 : (g.goalCount + 2) / 3;
}

bool hasThreeStarsPage(const GameSnapshot& g) {
  return gameOverState(g.gameState) && g.threeStarCount > 0;
}

// Draws the current page of the break rotation: linescore, three stars
// (postgame, once published), scoring summary, team stats (once fetched),
// league scores. Pages that have no content yet simply aren't in the
// rotation; the index wraps here when the page set shrinks or runs out.
void drawBreakPage(const GameSnapshot& g) {
  size_t leaguePages = (startedOtherGameCount() + 2) / 3;
  size_t scoringPages = scoringPageCount(g);
  size_t count = 1 + (hasThreeStarsPage(g) ? 1 : 0) + scoringPages +
                 (g.teamStats.valid ? 1 : 0) + leaguePages;
  if (sBreakPage >= count) sBreakPage = 0;
  size_t p = sBreakPage;
  if (p == 0) {
    drawLinescoreHalf(g, gameOverState(g.gameState) ? "GAME OVER"
                                                    : "INTERMISSION");
    return;
  }
  --p;
  if (hasThreeStarsPage(g)) {
    if (p == 0) {
      drawThreeStarsHalf(g);
      return;
    }
    --p;
  }
  if (p < scoringPages) {
    drawScoringPage(g, p, scoringPages);
    return;
  }
  p -= scoringPages;
  if (g.teamStats.valid) {
    if (p == 0) {
      drawTeamStatsPage(g);
      return;
    }
    --p;
  }
  drawLeagueBreakPage(g, p, leaguePages);
}

// Content fingerprint of everything the break pages show, so a poll
// repaints the strip only when something on it actually changed.
uint32_t hashBytes(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
uint32_t hashStr(uint32_t h, const char* s) {
  return hashBytes(h, s, strlen(s) + 1);
}
uint32_t hashInt(uint32_t h, int v) { return hashBytes(h, &v, sizeof(v)); }

uint32_t breakContentHash(const GameSnapshot& g) {
  uint32_t h = 2166136261u;
  h = hashInt(h, g.homeScore);
  h = hashInt(h, g.awayScore);
  h = hashInt(h, (g.otColumn ? 1 : 0) | (g.soColumn ? 2 : 0));
  h = hashBytes(h, g.homePeriodGoals, sizeof(g.homePeriodGoals));
  h = hashBytes(h, g.awayPeriodGoals, sizeof(g.awayPeriodGoals));
  for (size_t i = 0; i < g.goalCount; ++i) {
    const NhlGoalLine& gl = g.goals[i];
    h = hashInt(h, gl.period);
    h = hashStr(h, gl.periodType);
    h = hashStr(h, gl.time);
    h = hashStr(h, gl.teamAbbrev);
    h = hashStr(h, gl.name);
    h = hashStr(h, gl.tag);
  }
  for (int i = 0; i < g.threeStarCount && i < 3; ++i) {
    const NhlThreeStar& st = g.threeStars[i];
    h = hashStr(h, st.name);
    h = hashStr(h, st.teamAbbrev);
    h = hashInt(h, st.number);
    h = hashInt(h, st.goals);
    h = hashInt(h, st.assists);
  }
  const NhlTeamStats& ts = g.teamStats;
  h = hashInt(h, ts.valid ? 1 : 0);
  h = hashStr(h, ts.homePowerPlay);
  h = hashStr(h, ts.awayPowerPlay);
  h = hashInt(h, ts.homeFaceoffPct);
  h = hashInt(h, ts.awayFaceoffPct);
  h = hashInt(h, ts.homeHits);
  h = hashInt(h, ts.awayHits);
  h = hashInt(h, ts.homeBlocks);
  h = hashInt(h, ts.awayBlocks);
  for (size_t i = 0; i < otherGameCount; ++i) {
    const OtherGameInfo& o = otherGames[i];
    if (!otherGameStarted(o)) continue;
    h = hashStr(h, o.awayAbbrev);
    h = hashStr(h, o.homeAbbrev);
    h = hashInt(h, o.awayScore);
    h = hashInt(h, o.homeScore);
    h = hashStr(h, o.gameState);
  }
  return h;
}

// Picks and draws the current bottom-half mode. Called from the
// signature-driven redraw in renderLiveGame (and the initial full paint),
// so every state and content change funnels through here.
void drawBottomHalf(const GameSnapshot& g) {
  if (gameOverState(g.gameState) || g.inIntermission) {
    // Entering a break starts the rotation on the linescore page; content
    // refreshes during the break repaint the page that is already up.
    if (sBottomMode != BottomMode::BREAK) {
      sBottomMode = BottomMode::BREAK;
      sBreakPage = 0;
      sBreakFlipAt = millis() + NHL_BREAK_PAGE_MS;
    }
    sRotatePair = false;
    drawBreakPage(g);
    return;
  }
  if (g.penaltyCount > 0) {
    sBottomMode = BottomMode::PENS;
    sRotatePair = false;
    drawPenaltiesHalf(g);
    return;
  }
  // Any other game on the slate gives the league view content to flip
  // with — previews and finals count too now (they show with scores and
  // state tags).
  sRotatePair = otherGameCount > 0;
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
  drawLinescoreHalf(g, nullptr);
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
      uint8_t bq = power::batteryQuarters();
      if (bq <= 4) drawBatteryIndicator(canvas(), 296, 4, bq);
      d.battQuarters = (bq <= 4) ? (int)bq : -1;
      tftPanel.pushFull();
    }
  }

  // matrices + goal flash — the score lives on the matrices; a score
  // INCREASE (goal) flashes that team's NEW score INVERTED on its matrix
  // alone (the SOG panels never change appearance — goal feedback is
  // matrix-only).
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

  // bottom half: penalties while any are active, the break-page rotation
  // between periods and after the final, and during penalty-free play
  // the alternating linescore / league-scores pair. Signature covers
  // every mode's contents so the view flips and refreshes only on real
  // changes.
  char sig[384] = "";
  if (gameOverState(g.gameState) || g.inIntermission) {
    snprintf(sig, sizeof(sig), "B%c%08lx",
             g.inIntermission && !gameOverState(g.gameState) ? 'I' : 'G',
             (unsigned long)breakContentHash(g));
  } else {
    int penCount = g.penaltyCount;
    if (penCount > 0) {
      strlcpy(sig, "P", sizeof(sig));
      for (int i = 0; i < g.penaltyCount && i < 4; ++i) {
        const NhlPenalty& p = g.penalties[i];
        snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%s%d",
                 p.teamAbbrev, p.number, p.lastName, p.remainSec);
      }
    } else {
      snprintf(sig, sizeof(sig), "XR%d%d", g.homeScore, g.awayScore);
      for (int i = 0; i < 5; ++i)
        snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "%d%d",
                 (int)g.homePeriodGoals[i], (int)g.awayPeriodGoals[i]);
      snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "%d%d",
               g.otColumn ? 1 : 0, g.soColumn ? 1 : 0);
      for (size_t i = 0; i < otherGameCount; ++i) {
        const OtherGameInfo& o = otherGames[i];
        snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%d%s%c",
                 o.awayAbbrev, o.awayScore, o.homeScore, o.homeAbbrev,
                 o.gameState[0]);
      }
    }
  }
  if (strcmp(sig, d.penaltySig) != 0) {
    strlcpy(d.penaltySig, sig, sizeof(d.penaltySig));
    if (!goalDisplayActive()) drawBottomHalf(g);
  }

  // Battery gauge (V1.1 boards only — quarters 255/unknown hides it):
  // quarter changes are rare, so this tiny region repaints on its own
  // instead of riding every full redraw.
  {
    uint8_t bq = power::batteryQuarters();
    int bqDrawn = (bq <= 4) ? (int)bq : -1;
    if (bqDrawn != d.battQuarters && !goalDisplayActive()) {
      d.battQuarters = bqDrawn;
      if (bqDrawn >= 0) drawBatteryIndicator(canvas(), 296, 4, bq);
      tftPanel.pushRows(294, 2, 25, 14);
    }
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

void runGoalCelebration(const GameSnapshot& g, bool homeScored) {
  // Preferred-team goal celebration (also fired by the game demo): while
  // the goal horn plays, the SCORING team's matrix alone alternates
  // regular / inverted (reverse-video) frames — the other side holds its
  // steady score — and the four penalty LEDs strobe on the same 250 ms
  // rhythm. The TFT goal announcement stays up throughout (drawn before
  // this runs). Blocking on the render core, same style as the display
  // test: feeds keep flowing on core 0, and the loop exits with the horn
  // (audioPlaying()), bounded by NHL_GOAL_CELEBRATION_MIN/MAX_MS.
  const bool shootout = strcmp(g.periodType, "SO") == 0;
  const int dispPeriod = g.period;
  const int home = g.homeScore, away = g.awayScore;

  // The celebration replaces the plain score blink for this goal and
  // restores the matrices itself.
  sBlinkSide = 0;

  uint32_t start = millis();
  bool invert = false;
  while (millis() - start < NHL_GOAL_CELEBRATION_MAX_MS &&
         (audioPlaying() || millis() - start < NHL_GOAL_CELEBRATION_MIN_MS)) {
    invert = !invert;  // first frame inverts the scoring side
    if (shootout) {
      // Shootout frames carry the "SO" glyph on the period module — only
      // the scoring side's outer module inverts.
      setMax7219Shootout(home, away, invert && homeScored,
                         invert && !homeScored);
    } else {
      setMax7219ScoreFlash(home, away,
                           dispPeriod >= 1 ? dispPeriod : MAX7219_SCORE_BLANK,
                           invert && homeScored, invert && !homeScored);
    }
    setCountLeds(invert ? 2 : 0, invert ? 2 : 0, 0);
    delay(NHL_SCORE_BLINK_HALF_MS);
  }

  // Steady state restored: matrices back to the snapshot's scores and
  // period, penalty LEDs back to the snapshot's counts — exactly what
  // renderLiveGame applies on this poll.
  if (shootout) {
    setMax7219Shootout(home, away);
  } else {
    setMax7219Display(home, away,
                      dispPeriod >= 1 ? dispPeriod : MAX7219_SCORE_BLANK);
  }
  setCountLeds(g.inIntermission ? 0 : g.homePenaltyCount,
               g.inIntermission ? 0 : g.awayPenaltyCount, 0);
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

void nhl_render::invalidateLiveScreen() {
  sGoalDisplayPhase = GoalDisplayPhase::NONE;
  sBottomMode = BottomMode::LINESCORE;  // next break starts on its first page
  d = Drawn{};
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
  // Score blink: 250 ms normal / 250 ms inverted x3 on the scoring side's
  // matrix alone, then back to the steady display.
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
    else drawLinescoreHalf(nhl_render::currentGame, nullptr);
  }
  // Intermission / postgame: step through the break pages.
  if (!goalDisplayActive() && sBottomMode == BottomMode::BREAK &&
      (int32_t)(now - sBreakFlipAt) >= 0) {
    ++sBreakPage;
    sBreakFlipAt = now + NHL_BREAK_PAGE_MS;
    drawBreakPage(nhl_render::currentGame);
  }
  // League page rotation (>3 games): only while the league view is
  // actually on screen — the timer re-arms inside drawLeagueHalf.
  if (!goalDisplayActive() && sBottomMode == BottomMode::LEAGUE &&
      sLeagueGameCount > 3 && (int32_t)(now - sLeagueRotateAt) >= 0) {
    sLeagueStart = (sLeagueStart + 1) % sLeagueGameCount;
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
  int battQuarters = -2;  // battery gauge on the guest card (-1 = hidden)
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
      drawShotsBox(c, x + 73);
    }
    drawShotsCaption();
    tftPanel.pushFull();
  }

  // Match the live screen's outlined shot box and size-4 digit font.
  auto drawShots = [&](int cardX, int shots, int& cache) {
    if (shots == cache) return;
    cache = shots;
    const int centerX = cardX + 73;
    const int boxWidth = LOGO_SIZE + 2 * SHOTS_BOX_PAD;
    const int boxX = centerX - boxWidth / 2;
    c.fillRect(boxX + 3, SHOTS_NUM_Y - 2, boxWidth - 6,
               SHOTS_BOX_Y + SHOTS_BOX_H - 3 - (SHOTS_NUM_Y - 2), COLOR_BG);
    drawShotCount(c, centerX, SHOTS_NUM_Y, shots);
    tftPanel.pushRows(boxX - 2, SHOTS_BOX_Y - 2, boxWidth + 4,
                      SHOTS_BOX_H + 4);
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
    const int boardVersion = getBoardVersion();
    digitalWrite(homePenalty1Pin(boardVersion), mask & 0x1 ? HIGH : LOW);
    digitalWrite(homePenalty2Pin(boardVersion), mask & 0x2 ? HIGH : LOW);
    digitalWrite(awayPenalty1Pin(boardVersion), mask & 0x4 ? HIGH : LOW);
    digitalWrite(awayPenalty2Pin(boardVersion), mask & 0x8 ? HIGH : LOW);
  }

  // Battery gauge (V1.1 boards): rides the guest card's top-right corner,
  // clear of the centered title. One tiny band push on quarter changes.
  {
    uint8_t bq = power::batteryQuarters();
    int bqNow = (bq <= 4) ? (int)bq : -1;
    if (bqNow != md.battQuarters) {
      md.battQuarters = bqNow;
      if (bqNow >= 0) {
        drawBatteryIndicator(c, 286, 14, bq);
      } else {
        c.fillRect(286, 14, 22, 12, COLOR_CARD);  // clear a stale icon
      }
      tftPanel.pushRows(284, 12, 26, 16);
    }
  }
}
