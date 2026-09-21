#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/data/time_util.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/gfx.h"
#include "mlb_renderer.h"
#include "mlb_renderer_internal.h"
#include "mlb_logos.h"
#include "mlb_state.h"
#include "mlb_teams.h"

// Live-game renderer: inning header, team cards, base diamond, bottom panel
// (On Deck / Around the League), the at-bat result card, and the dirty-rect
// tracker that keeps the 5 s linescore repaint flicker-free. Also carries
// the parked (#if 0) division-standings screens.

namespace mlb_render {
LinescoreSnapshot currentLinescore{};
bool hasCurrentLiveGame = false;
JsonObjectConst currentStandings;
uint8_t tickerSlide = 0;
uint32_t lastCarouselTime = 0;
OtherGameInfo otherGames[MAX_OTHER_GAMES];
size_t otherGameCount = 0;
}  // namespace mlb_render

namespace {
using namespace mlb_render;

// Latest snapshot copy time (diagnostic only; not part of the dirty diff).
uint32_t currentLinescoreGen = 0;

// At-bat result card state.
bool atBatResultVisible = false;
uint32_t atBatResultStartedAt = 0;

// Tracks the most recent linescore/play inputs so the renderer can skip
// redrawing sections that haven't changed (logos, cards, inning header, base
// diamond). Rebuilt whenever any of those inputs change.
struct RenderedFrame {
  int  gamePk;
  int  awayScore;
  int  homeScore;
  int  balls;
  int  strikes;
  int  outs;
  int  currentInning;
  uint8_t inningState;
  bool isTopInning;
  int  awayTeamId;
  int  homeTeamId;
  bool offenseFirst;
  bool offenseSecond;
  bool offenseThird;
};
RenderedFrame gRendered{};
bool gHasRendered = false;

// 5x7 Dot Matrix Font table for digits 0-9
const uint8_t LED_DIGIT_5X7[10][7] = {
  {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 0
  {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
  {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
  {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
  {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
  {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
  {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
  {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
  {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
  {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}  // 9
};

// 5x5 LED Dot Matrix Arrow patterns (Up ▲ and Down ▼)
const uint8_t LED_ARROW_UP[5] = {
  0b00100,
  0b01110,
  0b11111,
  0b00100,
  0b00100
};

const uint8_t LED_ARROW_DOWN[5] = {
  0b00100,
  0b00100,
  0b11111,
  0b01110,
  0b00100
};

// "Bottom" -> "Bot", "Middle" -> "Mid", etc. for compact ticker rows.
const char* shortInningState(const char* state) {
  if (state == nullptr || state[0] == '\0') return "";
  if (strcmp(state, "Top") == 0) return "Top";
  if (strcmp(state, "Bottom") == 0) return "Bot";
  if (strcmp(state, "Middle") == 0) return "Mid";
  if (strcmp(state, "End") == 0) return "End";
  return "";
}

void drawLedDigit(Adafruit_GFX& target, int startX, int startY, int digit) {
  if (digit < 0 || digit > 9) return;
  const int spacing = 8;
  for (int row = 0; row < 7; row++) {
    uint8_t line = LED_DIGIT_5X7[digit][row];
    for (int col = 0; col < 5; col++) {
      int cx = startX + (col * spacing);
      int cy = startY + (row * spacing);
      bool active = (line & (1 << (4 - col))) != 0;
      if (active) {
        target.fillCircle(cx, cy, 3, COLOR_LED_RED);
      } else {
        target.fillCircle(cx, cy, 1, COLOR_LED_OFF);
      }
    }
  }
}

void drawLedArrow(Adafruit_GFX& target, int startX, int startY, bool isTop) {
  const int spacing = 8;
  const uint8_t* pattern = isTop ? LED_ARROW_UP : LED_ARROW_DOWN;
  for (int row = 0; row < 5; row++) {
    uint8_t line = pattern[row];
    for (int col = 0; col < 5; col++) {
      int cx = startX + (col * spacing);
      int cy = startY + (row * spacing) + 8; // Align vertically with 5x7 digits
      bool active = (line & (1 << (4 - col))) != 0;
      if (active) {
        target.fillCircle(cx, cy, 3, COLOR_LED_RED);
      } else {
        target.fillCircle(cx, cy, 1, COLOR_LED_OFF);
      }
    }
  }
}

// Snapshot-driven inning header: same pixel layout as the retired
// JSON-driven variant, reading from the pre-parsed snapshot.
void drawInningHeaderFromSnapshot(Adafruit_GFX& target, const LinescoreSnapshot& ls) {
  int currentInning = ls.currentInning;
  if (currentInning <= 0) currentInning = 1;
  bool betweenHalfInnings = (ls.inningState == 2) || (ls.inningState == 3);
  bool isTop = ls.isTopInning;
  int topY = 8;
  if (currentInning < 10) {
    int digitX = betweenHalfInnings ? 146 : 162;
    if (!betweenHalfInnings) drawLedArrow(target, 118, topY, isTop);
    drawLedDigit(target, digitX, topY, currentInning);
  } else {
    int digit1X = betweenHalfInnings ? 122 : 140;
    int digit2X = betweenHalfInnings ? 166 : 184;
    if (!betweenHalfInnings) drawLedArrow(target, 98, topY, isTop);
    drawLedDigit(target, digit1X, topY, currentInning / 10);
    drawLedDigit(target, digit2X, topY, currentInning % 10);
  }
}

void drawDiamondShape(Adafruit_GFX& target, int cx, int cy, int size, uint16_t color) {
  target.fillTriangle(cx, cy - size, cx - size, cy, cx + size, cy, color);
  target.fillTriangle(cx, cy + size, cx - size, cy, cx + size, cy, color);
}

// Renderer-local color: dark unlit base on the diamond (the shared theme
// colors live in sport_config.h).
const uint16_t COLOR_BASE_EMPTY = 0x31A6;

void drawBaseDiamond(Adafruit_GFX& target, int centerX, int centerY, bool b1, bool b2, bool b3) {
  const int d = 14;  // Slightly smaller distance
  const int s = 5;   // Smaller base diamond radius

  // Basepaths outline
  target.drawLine(centerX, centerY + d, centerX + d, centerY, COLOR_MUTED);  // Home to 1B
  target.drawLine(centerX + d, centerY, centerX, centerY - d, COLOR_MUTED);  // 1B to 2B
  target.drawLine(centerX, centerY - d, centerX - d, centerY, COLOR_MUTED);  // 2B to 3B
  target.drawLine(centerX - d, centerY, centerX, centerY + d, COLOR_MUTED);  // 3B to Home

  // Home plate icon (small white pentagon)
  target.fillTriangle(centerX, centerY + d + 3, centerX - 3, centerY + d, centerX + 3, centerY + d, ST77XX_WHITE);

  // 2nd Base (Top)
  drawDiamondShape(target, centerX, centerY - d, s, b2 ? COLOR_YELLOW : COLOR_BASE_EMPTY);

  // 1st Base (Right)
  drawDiamondShape(target, centerX + d, centerY, s, b1 ? COLOR_YELLOW : COLOR_BASE_EMPTY);

  // 3rd Base (Left)
  drawDiamondShape(target, centerX - d, centerY, s, b3 ? COLOR_YELLOW : COLOR_BASE_EMPTY);
}

void drawTeamCard(Adafruit_GFX& target, int x, int y, const char* label,
                  int teamId, const char* abbrev) {
  target.fillRoundRect(x, y, 88, 94, 6, COLOR_CARD);
  target.drawRoundRect(x, y, 88, 94, 6, COLOR_MUTED);

  // Card Top Label (HOME / AWAY)
  target.setTextColor(COLOR_MUTED);
  target.setTextSize(1);
  target.setCursor(x + 6, y + 4);
  target.print(label);

  // 64x64 Team Logo Centered Horizontally (RAM-cached on subsequent paints)
  int logoX = x + 12; // (88 - 64) / 2 = 12
  int logoY = y + 14;
  drawTeamLogo64(target, logoX, logoY, teamId, abbrev);

  // 3-Digit Team Abbreviation Centered Below Logo (red, matching inning display)
  target.setTextColor(COLOR_LED_RED);
  target.setTextSize(2);
  int abbrevX = x + 44 - (strlen(abbrev) * 6);
  target.setCursor(abbrevX, y + 76);
  target.print(abbrev);
}

// Standings sub-pages are disabled for now (see matching #if 0 block near
// renderDivisionStandings) in favor of the MLB news slides; kept for easy re-enable.
#if 0
const char* divisionName(int divisionId) {
  switch (divisionId) {
    case 200: return "AL WEST";
    case 201: return "AL EAST";
    case 202: return "AL CENTRAL";
    case 203: return "NL WEST";
    case 204: return "NL EAST";
    case 205: return "NL CENTRAL";
    default: return "DIVISION";
  }
}

// Finds the division record set (all teams in that division) containing teamId.
JsonObjectConst findDivisionRecordsForTeam(JsonObjectConst standingsRoot, int teamId) {
  if (teamId == 0) return JsonObjectConst();
  for (JsonObjectConst recordSet : standingsRoot["records"].as<JsonArrayConst>()) {
    for (JsonObjectConst teamRecord : recordSet["teamRecords"].as<JsonArrayConst>()) {
      if ((teamRecord["team"]["id"] | 0) == teamId) return recordSet;
    }
  }
  return JsonObjectConst();
}
#endif

void drawBottomPanel(Adafruit_GFX& target, const LinescoreSnapshot& ls) {
  // Expanded Bottom Section (Fills empty vertical space from Y=108 to Y=232)
  target.fillRect(8, 108, 304, 124, COLOR_CARD);
  target.drawRoundRect(8, 108, 304, 124, 6, ST77XX_WHITE);

  // Fixed row: current batter (left) and current pitcher (right)
  target.setTextSize(1);
  target.setTextColor(COLOR_GOLD);
  target.setCursor(18, 116);
  target.print("AT BAT");
  target.setCursor(166, 116);
  target.print("PITCHING");
  target.drawLine(161, 112, 161, 158, COLOR_MUTED);

  target.setTextColor(ST77XX_WHITE);
  target.setTextSize(2);
  target.setCursor(18, 132);
  printClipped(target, ls.batter[0] ? ls.batter : "Unknown Batter", 11);
  target.setCursor(166, 132);
  printClipped(target, ls.pitcher[0] ? ls.pitcher : "Unknown Pitcher", 11);

  // Scrolling detail zone below the fixed row
  target.drawLine(14, 160, 306, 160, COLOR_MUTED);
  if (tickerSlide >= tickerSlideCount()) tickerSlide = 0;

  if (tickerSlide == 0) {
    // Lineup context: on deck (large), in the hole + catcher (compact)
    target.setTextSize(1);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(18, 168);
    target.print("ON DECK");

    target.setTextColor(ST77XX_WHITE);
    target.setTextSize(2);
    target.setCursor(18, 182);
    printClipped(target, ls.onDeck[0] ? ls.onDeck : "--", 23);

    char line[64];
    snprintf(line, sizeof(line), "In the hole: %s   Catcher: %s",
             ls.inHole[0] ? ls.inHole : "--",
             ls.catcher[0] ? ls.catcher : "--");
    target.setTextSize(1);
    target.setTextColor(COLOR_MUTED);
    target.setCursor(18, 208);
    printClipped(target, line, 47);
  } else {
    // Around the league: ONE other live game per slide, in large type —
    // the earlier 3-rows-of-size-1 layout was unreadable at a glance.
    const OtherGameInfo& game = otherGames[tickerSlide - 1];

    // Header twice the old size, horizontally centered; inning tag small,
    // right-aligned on the same baseline.
    target.setTextSize(2);
    target.setTextColor(COLOR_GOLD);
    drawCenteredText(target, "AROUND THE LEAGUE", 160, 163);

    char tag[24] = "";
    const char* half = shortInningState(game.inningState);
    if (half[0] != '\0' && game.inningOrdinal[0] != '\0') {
      snprintf(tag, sizeof(tag), "%s %s", half, game.inningOrdinal);
    }
    if (tag[0] != '\0') {
      int16_t x1, y1;
      uint16_t tw, th;
      target.getTextBounds(tag, 0, 0, &x1, &y1, &tw, &th);
      target.setTextSize(1);
      target.setCursor(302 - (int)tw, 166);
      target.print(tag);
    }

    // Away row then home row: abbreviation left, score right-aligned, both
    // at textSize 3 (18 px per char cell, 24 px tall).
    char score[8];
    target.setTextSize(3);
    target.setTextColor(ST77XX_WHITE);
    target.setCursor(18, 181);
    target.print(game.awayAbbrev);
    snprintf(score, sizeof(score), "%d", game.awayScore);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(302 - (int)(strlen(score) * 18), 181);
    target.print(score);

    target.setTextColor(ST77XX_WHITE);
    target.setCursor(18, 207);
    target.print(game.homeAbbrev);
    snprintf(score, sizeof(score), "%d", game.homeScore);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(302 - (int)(strlen(score) * 18), 207);
    target.print(score);
  }
}
} // namespace

size_t mlb_render::tickerSlideCount() {
  return 1 + otherGameCount; // On Deck slide + one league game per slide
}

// Standings sub-pages are disabled for now in favor of the MLB news slides (see the
// matching #if 0 block near divisionName/findDivisionRecordsForTeam); kept for easy re-enable.
#if 0
// Small padlock glyph shown in the playoff column once a team has clinched a berth.
void drawLockIcon(Adafruit_GFX& target, int x, int y, uint16_t color) {
  target.drawFastVLine(x + 2, y, 5, color);
  target.drawFastVLine(x + 7, y, 5, color);
  target.drawFastHLine(x + 2, y, 6, color);
  target.fillRoundRect(x, y + 5, 10, 8, 2, color);
}

// Renders the division standings for highlightTeamId, one row per division team.
void renderDivisionStandings(int highlightTeamId, const char* subtitle) {
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  JsonObjectConst recordSet = findDivisionRecordsForTeam(mlb_render::currentStandings, highlightTeamId);
  if (recordSet.isNull()) {
    Serial.printf("[STANDINGS] No division record found for teamId=%d (doc null=%d)\n",
                  highlightTeamId, mlb_render::currentStandings.isNull() ? 1 : 0);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, "Standings unavailable", 160, 104);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "Standings refresh every 5 min", 160, 124);
    tftPanel.pushFull();
    return;
  }

  int divisionId = recordSet["division"]["id"] | 0;
  char header[24];
  snprintf(header, sizeof(header), "%s STANDINGS", divisionName(divisionId));

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, header, 160, 12);

  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(1);
  drawCenteredText(canvas, subtitle, 160, 34);

  // Column x-positions: rank, team (narrow), W, L, GB, PO (playoff distance / clinched lock).
  const int colRank = 18;
  const int colTeam = 44;
  const int colWins = 148;
  const int colLosses = 182;
  const int colGamesBack = 214;
  const int colPlayoff = 258;

  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(1);
  canvas.setCursor(colTeam, 48);
  canvas.print("TEAM");
  canvas.setCursor(colWins, 48);
  canvas.print("W");
  canvas.setCursor(colLosses, 48);
  canvas.print("L");
  canvas.setCursor(colGamesBack, 48);
  canvas.print("GB");
  canvas.setCursor(colPlayoff, 48);
  canvas.print("PO");

  int rowY = 66;
  const int rowHeight = 30;
  for (JsonObjectConst teamRecord : recordSet["teamRecords"].as<JsonArrayConst>()) {
    int teamId = teamRecord["team"]["id"] | 0;
    bool isHighlighted = (teamId == highlightTeamId);
    const char* teamName = teamRecord["team"]["name"] | "";
    const char* abbrev = mlbTeamAbbrev(teamId, teamName);
    int wins = teamRecord["wins"] | 0;
    int losses = teamRecord["losses"] | 0;
    const char* gamesBack = teamRecord["gamesBack"] | "-";
    const char* rank = teamRecord["divisionRank"] | "";
    bool clinched = teamRecord["clinched"] | false;
    const char* wildCardGamesBack = teamRecord["wildCardGamesBack"] | "-";

    if (isHighlighted) {
      canvas.fillRect(16, rowY - 3, 288, rowHeight - 4, COLOR_CARD);
    }

    uint16_t rowColor = isHighlighted ? COLOR_GOLD : ST77XX_WHITE;
    canvas.setTextColor(rowColor);
    canvas.setTextSize(2);

    canvas.setCursor(colRank, rowY);
    canvas.print(rank);

    canvas.setCursor(colTeam, rowY);
    canvas.print(abbrev);

    canvas.setCursor(colWins, rowY);
    canvas.print(wins);

    canvas.setCursor(colLosses, rowY);
    canvas.print(losses);

    canvas.setCursor(colGamesBack, rowY);
    canvas.print(gamesBack);

    if (clinched) {
      drawLockIcon(canvas, colPlayoff, rowY + 2, rowColor);
    } else {
      canvas.setCursor(colPlayoff, rowY);
      canvas.print(wildCardGamesBack);
    }

    rowY += rowHeight;
  }

  tftPanel.pushFull();
}
#endif

void showAtBatResult(const char* batterName, const char* event,
                     const char* description) {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "AT-BAT RESULT", 160, 18);
  canvas.drawFastHLine(24, 44, 272, COLOR_GOLD);

  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(2);
  drawWrappedText(canvas, batterName, 20, 62, 280, 12, 22, 2);

  // The short outcome ("Single", "Strikeout", "Home Run") is the headline
  // the card exists for; the sentence below carries the detail.
  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(3);
  drawCenteredText(canvas, event[0] != '\0' ? event : "", 160, 116);

  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  drawWrappedText(canvas, description, 20, 160, 280, 6, 12, 5);

  tftPanel.pushFull();
  // The card clobbered the whole screen: invalidate the linescore's
  // dirty-rect tracker so the return render repaints everything. Without
  // this the tracker saw "nothing changed" and the card stayed stuck on
  // screen after its dwell expired.
  gHasRendered = false;
  atBatResultStartedAt = millis();
  atBatResultVisible = true;
  DBG_PRINTF("[DISPLAY] at-bat result: %s — %s\n", batterName,
             event[0] != '\0' ? event : description);
}

void updateAtBatResultDisplay() {
  if (!atBatResultVisible ||
      millis() - atBatResultStartedAt < MLB_AT_BAT_RESULT_DISPLAY_MS) {
    return;
  }
  atBatResultVisible = false;
  if (mlb_render::hasCurrentLiveGame && mlb_render::currentLinescore.valid) {
    renderLinescore(mlb_render::currentLinescore);
  }
}

void resetAtBatResultDisplay() {
  atBatResultVisible = false;
  atBatResultStartedAt = 0;
}

bool isAtBatResultVisible() { return atBatResultVisible; }

void renderLinescore(const LinescoreSnapshot& ls) {
  mlb_render::currentLinescore    = ls;
  mlb_render::hasCurrentLiveGame  = true;
  currentLinescoreGen = millis();  // local-only; not used for dirty diff

  bool betweenHalfInnings = (ls.inningState == 2) || (ls.inningState == 3);

  // Determine what's different from the last rendered frame. Static portions
  // (team cards, logos, base diamond, inning header) only repaint when one of
  // their inputs actually changes; the bottom panel + score LEDs always update.
  bool teamsChanged    = !gHasRendered ||
      ls.awayTeamId != gRendered.awayTeamId || ls.homeTeamId != gRendered.homeTeamId;
  bool scoreChanged    = !gHasRendered ||
      ls.awayScore != gRendered.awayScore || ls.homeScore != gRendered.homeScore;
  bool countChanged    = !gHasRendered ||
      ls.balls != gRendered.balls || ls.strikes != gRendered.strikes ||
      ls.outs != gRendered.outs;
  bool inningChanged   = !gHasRendered ||
      ls.currentInning != gRendered.currentInning ||
      ls.inningState != gRendered.inningState ||
      ls.isTopInning != gRendered.isTopInning;
  bool basesChanged    = !gHasRendered ||
      ls.offenseFirst != gRendered.offenseFirst ||
      ls.offenseSecond != gRendered.offenseSecond ||
      ls.offenseThird != gRendered.offenseThird;

  bool needsFullRepaint = teamsChanged || inningChanged || basesChanged;

  GFXcanvas16& canvas = tftPanel.canvas();
  if (needsFullRepaint) {
    canvas.fillScreen(COLOR_BG);
  }

  // Hardware outputs (scores + 7 count LEDs) update independently of the TFT
  // buffer so even a no-pixels-changed frame keeps the LEDs accurate.
  if (scoreChanged || teamsChanged || inningChanged) {
    setMax7219Scores(ls.awayScore, ls.homeScore);
  }
  if (countChanged || teamsChanged || inningChanged) {
    if (betweenHalfInnings) setCountLeds(0, 0, 0);
    else                    setCountLeds(ls.balls, ls.strikes, ls.outs);
  }

  // Pre-warm logo cache for the two teams shown this frame so the first
  // drawTeamCard call is the cached memcpy path.
  if (teamsChanged) {
    // We don't have abbreviations in the snapshot; mlbTeamAbbrev() does the
    // id->abbrev lookup, so we call it twice to warm the cache with the
    // resolved abbreviation strings.
    const char* aAb = mlbTeamAbbrev(ls.awayTeamId, nullptr);
    const char* hAb = mlbTeamAbbrev(ls.homeTeamId, nullptr);
    ensureLogoCached(ls.awayTeamId, aAb);
    ensureLogoCached(ls.homeTeamId, hAb);
  }

  // Static portions (only when their inputs changed).
  if (needsFullRepaint) {
    int awayTeamId = ls.awayTeamId ? ls.awayTeamId : ls.offenseTeamId;
    int homeTeamId = ls.homeTeamId ? ls.homeTeamId : ls.defenseTeamId;
    if (ls.isTopInning) {
      // Offense is batting away in the top, home in the bottom; the snapshot
      // already encodes this via awayTeamId/homeTeamId when present.
    } else if (awayTeamId == 0) {
      awayTeamId = ls.defenseTeamId;
      homeTeamId = ls.offenseTeamId;
    }

    const char* awayAbbrev = mlbTeamAbbrev(awayTeamId, nullptr);
    const char* homeAbbrev = mlbTeamAbbrev(homeTeamId, nullptr);

    // HOME Team Card on LEFT (X=8), AWAY Team Card on RIGHT (X=224)
    drawTeamCard(canvas, 8,   6, "HOME", homeTeamId, homeAbbrev);
    drawTeamCard(canvas, 224, 6, "AWAY", awayTeamId, awayAbbrev);

    // Center Base Runners Diagram
    drawBaseDiamond(canvas, 160, 74,
                    ls.offenseFirst, ls.offenseSecond, ls.offenseThird);

    // Top Red LED Dot Matrix Inning Header
    drawInningHeaderFromSnapshot(canvas, ls);
  }

  // Bottom panel + ticker always re-render (it changes every cycle anyway).
  drawBottomPanel(canvas, ls);
  mlb_render::lastCarouselTime = millis();

  // Single-pass push to physical TFT display (flicker-free)
  tftPanel.pushFull();

  gRendered.gamePk        = ls.gamePk;
  gRendered.awayScore     = ls.awayScore;
  gRendered.homeScore     = ls.homeScore;
  gRendered.balls         = ls.balls;
  gRendered.strikes       = ls.strikes;
  gRendered.outs          = ls.outs;
  gRendered.currentInning = ls.currentInning;
  gRendered.inningState   = ls.inningState;
  gRendered.isTopInning   = ls.isTopInning;
  gRendered.awayTeamId    = ls.awayTeamId ? ls.awayTeamId : ls.offenseTeamId;
  gRendered.homeTeamId    = ls.homeTeamId ? ls.homeTeamId : ls.defenseTeamId;
  gRendered.offenseFirst  = ls.offenseFirst;
  gRendered.offenseSecond = ls.offenseSecond;
  gRendered.offenseThird  = ls.offenseThird;
  gHasRendered = true;
}

void updateOtherGames(const ScheduleSnapshot& sched, int excludeGamePk) {
  mlb_render::otherGameCount = 0;
  for (size_t i = 0; i < sched.otherCount && mlb_render::otherGameCount < mlb_render::MAX_OTHER_GAMES; ++i) {
    const OtherGameLite& g = sched.others[i];
    // The snapshot stores team-id digits in the abbrev field as a placeholder;
    // resolve the canonical abbreviation through mlbTeamAbbrev() here so the
    // ticker rendering code is unchanged.
    int awayTeamId = atoi(g.awayAbbrev);
    int homeTeamId = atoi(g.homeAbbrev);
    // TODO(template): compares team IDs against a gamePk — latent quirk
    // preserved verbatim from the pre-refactor code; the exclusion never
    // fires in practice (IDs and gamePks never collide).
    if (awayTeamId == excludeGamePk || homeTeamId == excludeGamePk) continue;
    mlb_render::OtherGameInfo& info = mlb_render::otherGames[mlb_render::otherGameCount++];
    info.awayTeamId   = awayTeamId;
    info.homeTeamId   = homeTeamId;
    strlcpy(info.awayAbbrev, mlbTeamAbbrev(awayTeamId, nullptr), sizeof(info.awayAbbrev));
    strlcpy(info.homeAbbrev, mlbTeamAbbrev(homeTeamId, nullptr), sizeof(info.homeAbbrev));
    info.awayScore    = g.awayScore;
    info.homeScore    = g.homeScore;
    strlcpy(info.inningState,   g.inningState,   sizeof(info.inningState));
    strlcpy(info.inningOrdinal,  g.inningOrdinal, sizeof(info.inningOrdinal));
  }
}

void logLiveDisplayState(const LinescoreSnapshot& ls, int gamePk) {
  const char* inningStateName = "Unknown";
  switch (ls.inningState) {
    case 0: inningStateName = "Top";    break;
    case 1: inningStateName = "Bottom"; break;
    case 2: inningStateName = "Middle"; break;
    case 3: inningStateName = "End";    break;
  }
  DBG_PRINTF(
    "[DISPLAY] state=LIVE_GAME gamePk=%d | TFT=live panel, %s %d, ticker=%u "
    "| matrices: home=%d away=%d | count LEDs: balls=%u strikes=%u outs=%u\n",
    gamePk, inningStateName, ls.currentInning, mlb_render::tickerSlide,
    ls.homeScore, ls.awayScore, ls.balls, ls.strikes, ls.outs);
}
