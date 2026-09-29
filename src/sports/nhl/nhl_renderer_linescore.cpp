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

GFXcanvas16& canvas() { return tftPanel.canvas(); }

// Layout (320x240):
//   y  0..26  header: state chip | period | clock
//   y 30..122 away card (logo+abbrev) + score box at x228
//   y126..218 home card + score box
//   y222..239 SOG + penalty strip
const int CARD_X = 8, CARD_W = 214, CARD_H = 92;
const int AWAY_Y = 30, HOME_Y = 126;
const int SCORE_X = 228, SCORE_W = 84;
const int STRIP_Y = 222;

// Last-drawn cache — anything that differs triggers that region's repaint.
struct Drawn {
  long gameId = 0;
  char homeAbbrev[4] = "", awayAbbrev[4] = "";
  int homeTeamId = 0, awayTeamId = 0;
  int homeScore = -1, awayScore = -1;
  int period = -1;
  char periodType[4] = "";
  char state[8] = "";
  int clockSec = -1;
  bool inIntermission = false;
  int sogH = -1, sogA = -1;
  char penaltySig[64] = "";
} d;

void drawHeader(bool all) {
  (void)all;
  canvas().fillRect(0, 0, 320, 26, COLOR_CARD);
  // state chip
  bool final = strcmp(d.state, "FINAL") == 0 || strcmp(d.state, "OFF") == 0;
  canvas().setTextSize(1);
  canvas().setTextColor(final ? COLOR_GOLD : COLOR_LED_RED);
  canvas().setCursor(8, 9);
  canvas().print(final ? "FINAL" : (d.state[0] ? d.state : "NHL"));
  // period
  char per[10] = "";
  if (final) strlcpy(per, "GAME OVER", sizeof(per));
  else if (d.period >= 1) {
    if (strcmp(d.periodType, "SO") == 0) strlcpy(per, "SHOOTOUT", sizeof(per));
    else if (strcmp(d.periodType, "OT") == 0)
      snprintf(per, sizeof(per), "%dOT", d.period - 3);
    else snprintf(per, sizeof(per), "%d%s", d.period,
                  d.period == 1 ? "st" : d.period == 2 ? "nd" : "rd");
  }
  canvas().setTextSize(2);
  canvas().setTextColor(ST77XX_WHITE);
  drawCenteredText(canvas(), per, 160, 6);
  // clock (mm:ss, or intermission countdown)
  int t = d.clockSec < 0 ? 0 : d.clockSec;
  char clk[10];
  snprintf(clk, sizeof(clk), "%s%d:%02d",
           d.inIntermission ? "INT " : "", t / 60, t % 60);
  canvas().setTextColor(d.inIntermission ? COLOR_GOLD : ST77XX_WHITE);
  int w = strlen(clk) * 12;
  canvas().setCursor(312 - w, 6);
  canvas().print(clk);
  tftPanel.pushRows(0, 0, 320, 26);
}

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

void drawTeamRow(int y, const char* abbrev, int teamId) {
  canvas().fillRoundRect(CARD_X, y, CARD_W, CARD_H, 6, COLOR_CARD);
  canvas().drawRoundRect(CARD_X, y, CARD_W, CARD_H, 6, COLOR_MUTED);
  ensureLogoCached(teamId, abbrev);
  drawTeamLogo64(canvas(), CARD_X + 10, y + 14, teamId, abbrev);
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(4);  // 24px glyphs
  canvas().setCursor(CARD_X + 92, y + 34);
  printClipped(canvas(), abbrev, 4);
}

void drawScoreBox(int y, int score, bool inverted = false) {
  // inverted = the goal flash: gold fill with dark digits instead of the
  // dark card with gold digits.
  canvas().fillRoundRect(SCORE_X, y, SCORE_W, CARD_H, 6,
                         inverted ? COLOR_GOLD : COLOR_CARD);
  canvas().drawRoundRect(SCORE_X, y, SCORE_W, CARD_H, 6, COLOR_MUTED);
  char s[4];
  snprintf(s, sizeof(s), "%d", score < 0 ? 0 : score);
  canvas().setTextColor(inverted ? COLOR_BG : COLOR_GOLD);
  canvas().setTextSize(6);  // 36px glyphs, up to 2 digits
  int w = strlen(s) * 36;
  canvas().setCursor(SCORE_X + (SCORE_W - w) / 2, y + 30);
  canvas().print(s);
  tftPanel.pushRows(SCORE_X, y, SCORE_W, CARD_H);
}

void drawBottomStrip(const GameSnapshot& g) {
  canvas().fillRect(0, STRIP_Y, 320, 240 - STRIP_Y, COLOR_CARD);
  char sog[14];
  snprintf(sog, sizeof(sog), "SOG %d-%d", g.awaySog, g.homeSog);
  canvas().setTextSize(2);
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setCursor(8, STRIP_Y + 2);
  canvas().print(sog);
  // penalty detail (right): "TOR slashing 1:23" xN or strength note
  canvas().setTextSize(1);
  if (g.penaltyCount == 0) {
    canvas().setTextColor(COLOR_MUTED);
    canvas().setCursor(120, STRIP_Y + 5);
    canvas().print(g.homePenaltyCount + g.awayPenaltyCount > 0
                       ? "POWER PLAY" : "EVEN STRENGTH");
  } else {
    int x = 112;
    for (int i = 0; i < g.penaltyCount && i < 2; ++i) {
      const NhlPenalty& p = g.penalties[i];
      char buf[26];
      snprintf(buf, sizeof(buf), "%s %s %d:%02d", p.teamAbbrev, p.desc,
               p.remainSec / 60, p.remainSec % 60);
      canvas().setTextColor(COLOR_GOLD);
      canvas().setCursor(x, STRIP_Y + 5);
      printClipped(canvas(), buf, 25);
      x += 26 * 6 + 8;
    }
  }
  tftPanel.pushRows(0, STRIP_Y, 320, 240 - STRIP_Y);
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
    drawTeamRow(AWAY_Y, g.awayAbbrev, g.awayTeamId);
    drawTeamRow(HOME_Y, g.homeAbbrev, g.homeTeamId);
    d.homeScore = d.awayScore = -1;  // force score box redraw
    tftPanel.pushFull();
  }

  // score boxes + matrices — a score INCREASE (goal) draws the box
  // inverted (gold fill, dark digits) and schedules the 500 ms flash;
  // the per-tick hook restores the normal box when it expires.
  if (g.awayScore != d.awayScore) {
    bool goal = g.awayScore > d.awayScore && d.awayScore >= 0;
    d.awayScore = g.awayScore;
    drawScoreBox(AWAY_Y, g.awayScore, goal);
    sFlashAwayUntil = goal ? millis() + NHL_SCORE_FLASH_MS : 0;
  }
  if (g.homeScore != d.homeScore) {
    bool goal = g.homeScore > d.homeScore && d.homeScore >= 0;
    d.homeScore = g.homeScore;
    drawScoreBox(HOME_Y, g.homeScore, goal);
    sFlashHomeUntil = goal ? millis() + NHL_SCORE_FLASH_MS : 0;
  }
  setMax7219Scores(g.awayScore, g.homeScore);
  setCountLeds(g.homePenaltyCount, g.awayPenaltyCount, 0);

  // TM1637 + TFT header clock: period clock MM:SS — colon always lit
  // (board convention: the clock display keeps its colon in every mode
  // and reading). The model re-syncs from this poll; between polls
  // tickLiveClock() counts it down while the game clock runs.
  syncClockModel(g);
  int clk = liveClockSec();
  tm1637ShowPair(clk / 60, clk % 60, true);

  // header (period / clock / state)
  if (g.period != d.period || strcmp(g.periodType, d.periodType) != 0 ||
      strcmp(g.gameState, d.state) != 0 || clk != d.clockSec ||
      g.inIntermission != d.inIntermission) {
    d.period = g.period;
    strlcpy(d.periodType, g.periodType, sizeof(d.periodType));
    strlcpy(d.state, g.gameState, sizeof(d.state));
    d.clockSec = clk;
    d.inIntermission = g.inIntermission;
    drawHeader(true);
  }

  // bottom strip (SOG + penalties)
  char sig[64] = "";
  for (int i = 0; i < g.penaltyCount && i < 2; ++i)
    snprintf(sig + strlen(sig), sizeof(sig) - strlen(sig), "%s%d",
             g.penalties[i].teamAbbrev, g.penalties[i].remainSec);
  if (g.homeSog != d.sogH || g.awaySog != d.sogA ||
      strcmp(sig, d.penaltySig) != 0 ||
      g.homePenaltyCount + g.awayPenaltyCount > 0) {
    d.sogH = g.homeSog; d.sogA = g.awaySog;
    strlcpy(d.penaltySig, sig, sizeof(d.penaltySig));
    drawBottomStrip(g);
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
    drawScoreBox(AWAY_Y, d.awayScore);
  }
  if (sFlashHomeUntil != 0 && (int32_t)(now - sFlashHomeUntil) >= 0) {
    sFlashHomeUntil = 0;
    drawScoreBox(HOME_Y, d.homeScore);
  }
  int clk = liveClockSec();
  if (clk == d.clockSec) return;
  d.clockSec = clk;
  tm1637ShowPair(clk / 60, clk % 60, true);
  drawHeader(true);
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
