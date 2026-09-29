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
//   y  98..150 SHOTS row: label + big count under each logo
//   y 154..238 penalties (active: player + remaining) or, when even
//                strength, scores from the league's live games
const int LOGO_Y = 8, LOGO_SIZE = 84;
const int HOME_LOGO_X = 26, GUEST_LOGO_X = 210;  // home left, guest right
const int SHOTS_LBL_Y = 98, SHOTS_NUM_Y = 110;   // per-column shots text
const int BOTTOM_Y = 154;

// Last-drawn cache — anything that differs triggers that region's repaint.
struct Drawn {
  long gameId = 0;
  char homeAbbrev[4] = "", awayAbbrev[4] = "";
  int homeTeamId = 0, awayTeamId = 0;
  int homeScore = -1, awayScore = -1;
  int period = -1;  // last shown on the period matrix
  int clockSec = -1;  // tickLiveClock's per-second tracker (TM1637)
  int sogH = -1, sogA = -1;
  char penaltySig[128] = "";
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

// Goal flash: millis() deadline of each side's inverted score box
// (0 = not flashing). Set by renderLiveGame on a score increase.
uint32_t sFlashAwayUntil = 0;
uint32_t sFlashHomeUntil = 0;

void syncClockModel(const GameSnapshot& g) {
  uint32_t now = millis();
  int local = sClockRunning
                  ? sClockBasisSec - (int)((now - sClockSyncedAt) / 1000)
                  : sClockBasisSec;
  if (local < 0) local = 0;
  bool running = g.clockRunning && !g.inIntermission;
  int fresh = g.clockSec < 0 ? 0 : g.clockSec;
  int drift = fresh - local;
  if (!running || !sClockRunning || drift > 2 || drift < -2) {
    sClockBasisSec = fresh;  // re-sync from the poll
  } else {
    sClockBasisSec = local;  // keep local continuity within the deadband
  }
  sClockSyncedAt = now;
  sClockRunning = running;
}

int liveClockSec() {
  if (!sClockRunning) return sClockBasisSec;
  int v = sClockBasisSec - (int)((millis() - sClockSyncedAt) / 1000);
  return v > 0 ? v : 0;
}

// One team column: logo at the top, the shot count clearly labeled
// SHOTS beneath it. inverted = the goal flash — the shots box fills
// gold with dark text for the 500 ms window (the score itself lives on
// the matrices).
void drawTeamColumn(int logoX, const char* abbrev, int teamId, int shots,
                    bool inverted) {
  ensureLogoCached(teamId, abbrev);
  drawTeamLogoScaled(canvas(), logoX, LOGO_Y, teamId, abbrev, LOGO_SIZE);
  const int cx = logoX + LOGO_SIZE / 2;
  if (inverted) {
    canvas().fillRoundRect(cx - 66, SHOTS_LBL_Y - 4, 132, 48, 6, COLOR_GOLD);
  }
  canvas().setTextColor(inverted ? COLOR_BG : COLOR_MUTED);
  canvas().setTextSize(1);
  drawCenteredText(canvas(), "SHOTS", cx, SHOTS_LBL_Y);
  char n[4];
  snprintf(n, sizeof(n), "%d", shots < 0 ? 0 : shots);
  canvas().setTextColor(inverted ? COLOR_BG : COLOR_GOLD);
  canvas().setTextSize(5);
  drawCenteredText(canvas(), n, cx, SHOTS_NUM_Y);
  tftPanel.pushRows(logoX - 2, LOGO_Y - 2, LOGO_SIZE + 4, 148);
}

bool liveish(const char* state) {
  return strcmp(state, "LIVE") == 0 || strcmp(state, "CRIT") == 0;
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
    snprintf(rem, sizeof(rem), "%d:%02d", p.remainSec / 60, p.remainSec % 60);
    canvas().setTextColor(COLOR_GOLD);
    canvas().setCursor(308 - (int)strlen(rem) * 12, y);
    canvas().print(rem);
    y += 20;
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
}

// Bottom half, even-strength view: scores of the league's in-progress
// games (the followed game excluded by updateOtherGames).
void drawLeagueHalf() {
  canvas().fillRect(0, BOTTOM_Y, 320, 240 - BOTTOM_Y, COLOR_BG);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, BOTTOM_Y + 4);
  canvas().print("AROUND THE LEAGUE");
  int y = BOTTOM_Y + 24;
  int rows = 0;
  for (size_t i = 0; i < otherGameCount && rows < 3; ++i) {
    const OtherGameInfo& o = otherGames[i];
    if (!liveish(o.gameState)) continue;
    char line[20];
    snprintf(line, sizeof(line), "%s %2d - %2d %s", o.awayAbbrev, o.awayScore,
             o.homeScore, o.homeAbbrev);
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), line, 160, y);
    y += 20;
    ++rows;
  }
  if (rows == 0) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    drawCenteredText(canvas(), "No other games in progress", 160,
                     BOTTOM_Y + 44);
  }
  tftPanel.pushRows(0, BOTTOM_Y, 320, 240 - BOTTOM_Y);
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
    sFlashAwayUntil = sFlashHomeUntil = 0;
    canvas().fillScreen(COLOR_BG);
    d.homeScore = d.awayScore = -1;   // force change-driven redraws
    d.sogH = d.sogA = -1;
    d.penaltySig[0] = '\0';
    drawTeamColumn(HOME_LOGO_X, g.homeAbbrev, g.homeTeamId, g.homeSog, false);
    drawTeamColumn(GUEST_LOGO_X, g.awayAbbrev, g.awayTeamId, g.awaySog, false);
    drawPenaltiesHalf(g);
    tftPanel.pushFull();
  }

  // matrices + goal flash — the score lives on the matrices; a score
  // INCREASE (goal) inverts that team's SOG panel for the flash window
  // and the per-tick hook restores it.
  bool homeGoal = false, awayGoal = false;
  if (g.awayScore != d.awayScore) {
    awayGoal = g.awayScore > d.awayScore && d.awayScore >= 0;
    d.awayScore = g.awayScore;
    sFlashAwayUntil = awayGoal ? millis() + NHL_SCORE_FLASH_MS : 0;
  }
  if (g.homeScore != d.homeScore) {
    homeGoal = g.homeScore > d.homeScore && d.homeScore >= 0;
    d.homeScore = g.homeScore;
    sFlashHomeUntil = homeGoal ? millis() + NHL_SCORE_FLASH_MS : 0;
  }
  // Scores on the outer modules, period on the middle one (blank when
  // the feed has no period yet).
  setMax7219Display(g.homeScore, g.awayScore,
                    g.period >= 1 ? g.period : MAX7219_SCORE_BLANK);
  setCountLeds(g.homePenaltyCount, g.awayPenaltyCount, 0);

  // TM1637 + TFT header clock: period clock MM:SS — colon always lit
  // (board convention: the clock display keeps its colon in every mode
  // and reading). The model re-syncs from this poll; between polls
  // tickLiveClock() counts it down while the game clock runs.
  syncClockModel(g);
  int clk = liveClockSec();
  tm1637ShowPair(clk / 60, clk % 60, true);

  d.clockSec = clk;  // second tracker for tickLiveClock's TM1637 updates

  // SOG panels — a goal also bumps that side's shot count, so the
  // inverted flash panel rides along on the same redraw.
  if (g.homeSog != d.sogH || homeGoal) {
    d.sogH = g.homeSog;
    drawTeamColumn(HOME_LOGO_X, g.homeAbbrev, g.homeTeamId, g.homeSog,
                   sFlashHomeUntil != 0);
  }
  if (g.awaySog != d.sogA || awayGoal) {
    d.sogA = g.awaySog;
    drawTeamColumn(GUEST_LOGO_X, g.awayAbbrev, g.awayTeamId, g.awaySog,
                   sFlashAwayUntil != 0);
  }

  // bottom half: penalties while any are active, otherwise the league's
  // live scores. Signature covers both modes' contents so the view
  // flips and refreshes only on real changes.
  char sig[128] = "";
  if (g.penaltyCount > 0) {
    strlcpy(sig, "P", sizeof(sig));
    for (int i = 0; i < g.penaltyCount && i < 4; ++i) {
      const NhlPenalty& p = g.penalties[i];
      snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%s%d",
               p.teamAbbrev, p.number, p.lastName, p.remainSec);
    }
  } else {
    strlcpy(sig, "L", sizeof(sig));
    for (size_t i = 0; i < otherGameCount; ++i) {
      const OtherGameInfo& o = otherGames[i];
      if (!liveish(o.gameState)) continue;
      snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "|%s%d%d%s",
               o.awayAbbrev, o.awayScore, o.homeScore, o.homeAbbrev);
    }
  }
  if (strcmp(sig, d.penaltySig) != 0) {
    strlcpy(d.penaltySig, sig, sizeof(d.penaltySig));
    if (g.penaltyCount > 0) drawPenaltiesHalf(g);
    else drawLeagueHalf();
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

void forceLiveRepaint(const GameSnapshot& g) {
  d = Drawn{};  // drop every cached value so the whole screen repaints
  renderLiveGame(g);
}

void tickLiveClock() {
  // Called every loop pass during a live game: advances the displayed
  // clock one second at a time between the 5 s landing polls (frozen
  // while the game clock is stopped), and lets the goal flash expire
  // back to the normal score box. No-op without a live game.
  if (!nhl_render::hasCurrentLiveGame) {
    sFlashAwayUntil = sFlashHomeUntil = 0;
    return;
  }
  uint32_t now = millis();
  if (sFlashAwayUntil != 0 && (int32_t)(now - sFlashAwayUntil) >= 0) {
    sFlashAwayUntil = 0;
    drawTeamColumn(GUEST_LOGO_X, d.awayAbbrev, d.awayTeamId, d.sogA, false);
  }
  if (sFlashHomeUntil != 0 && (int32_t)(now - sFlashHomeUntil) >= 0) {
    sFlashHomeUntil = 0;
    drawTeamColumn(HOME_LOGO_X, d.homeAbbrev, d.homeTeamId, d.sogH, false);
  }
  int clk = liveClockSec();
  if (clk == d.clockSec) return;
  d.clockSec = clk;
  tm1637ShowPair(clk / 60, clk % 60, true);
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
