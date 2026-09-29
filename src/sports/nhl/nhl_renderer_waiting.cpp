#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>

#include "config.h"
#include "common/data/time_util.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/gfx.h"
#include "nhl_logos.h"
#include "nhl_renderer.h"
#include "nhl_renderer_internal.h"
#include "nhl_state.h"
#include "news_font.h"

// Waiting-mode screen, MLB-style carousel: the around-the-league slide
// when games are on, then upcoming-game cards and news story pages
// interleaved — card, story, card, story — running the longer tail of
// whichever side has more entries. Each card covers one upcoming game
// from the week look-ahead (away @ home with logos + 3-letter
// abbreviations, local date/time, STARTS IN countdown, GAME X OF Y
// footer); each story page shows the headline as large centered white
// text over a scrolling detail strip that plays through the whole
// description before the slide advances (short stories fall back to a
// timed dwell, and the scroll keeps its position when a schedule
// publish repaints the same slide). The app re-renders whenever a
// fresh day-score publish lands. Matrices/penalty LEDs are cleared on
// entry (the idle clock on the TM1637 is driven by the app via
// updateTm1637WallClock).

namespace {

using namespace nhl_render;

GFXcanvas16& canvas() { return tftPanel.canvas(); }

// ---- News slot storage (renderer-owned; core-0 publishes into it) ----
NewsStory sNews[MAX_NEWS_STORIES];
size_t sNewsCount = 0;
size_t sNewsIndex = 0;

JsonObjectConst sSched;      // view into nhl_state's cached day-score doc

bool isLiveish(const char* state) {
  return strcmp(state, "LIVE") == 0 || strcmp(state, "CRIT") == 0;
}
bool isUpcoming(const char* state) {
  return strcmp(state, "FUT") == 0 || strcmp(state, "PREVIEW") == 0;
}

// ---- Upcoming-game cards (one per game, MLB-style) ----
// Every upcoming game in the week look-ahead gets its own detailed card;
// the carousel interleaves them with the news stories.
const size_t MAX_UPCOMING_CARDS = 64;

struct UpcomingCard {
  char awayAbbrev[4];
  char homeAbbrev[4];
  int  awayId, homeId;
  time_t startUtc;
};
UpcomingCard sCards[MAX_UPCOMING_CARDS];
size_t sCardCount = 0;

// Rebuilds the card list from the cached upcoming doc: one card per
// upcoming game, in the doc's chronological order (today's fresh slate
// followed by the rest of the week). Games without a parseable start
// time are skipped.
void buildUpcomingCards(JsonObjectConst sched) {
  sCardCount = 0;
  JsonArrayConst games = sched["games"].as<JsonArrayConst>();
  if (games.isNull()) return;
  for (JsonObjectConst g : games) {
    if (sCardCount >= MAX_UPCOMING_CARDS) break;
    if (!isUpcoming(g["gameState"] | "")) continue;
    time_t start = 0;
    if (!isoDateToEpoch(g["startTimeUTC"] | "", start) || start == 0) continue;
    UpcomingCard& c = sCards[sCardCount++];
    strlcpy(c.awayAbbrev, g["awayTeam"]["abbrev"] | "??", 4);
    strlcpy(c.homeAbbrev, g["homeTeam"]["abbrev"] | "??", 4);
    c.awayId = g["awayTeam"]["id"] | 0;
    c.homeId = g["homeTeam"]["id"] | 0;
    c.startUtc = start;
  }
}

// MLB-style upcoming-game card: gold date/time and white STARTS IN
// countdown up top, away logo + abbrev left, "@" between, home logo +
// abbrev right, GAME X OF Y footer so the carousel position is visible.
void drawUpcomingCard(size_t idx) {
  const UpcomingCard& g = sCards[idx];

  tm lt = {};
  localtime_r(&g.startUtc, &lt);
  char when[28];
  strftime(when, sizeof(when), "%a, %b %d - %I:%M %p", &lt);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), when, 160, 15);

  if (timeIsSynced() && g.startUtc > time(nullptr)) {
    long remain = (long)(g.startUtc - time(nullptr));
    char countdown[32];
    snprintf(countdown, sizeof(countdown), "STARTS IN %02ld:%02ld:%02ld",
             remain / 86400, (remain % 86400) / 3600, (remain % 3600) / 60);
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    drawCenteredText(canvas(), countdown, 160, 48);
  }

  drawTeamLogoScaled(canvas(), 20, 82, g.awayId, g.awayAbbrev, 88);
  drawTeamLogoScaled(canvas(), 212, 82, g.homeId, g.homeAbbrev, 88);

  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(4);
  drawCenteredText(canvas(), "@", 160, 110);

  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(3);
  drawCenteredText(canvas(), g.awayAbbrev, 64, 178);
  drawCenteredText(canvas(), g.homeAbbrev, 256, 178);

  canvas().drawLine(22, 204, 298, 204, COLOR_CARD);
  char position[20];
  snprintf(position, sizeof(position), "GAME %u OF %u",
           (unsigned)(idx + 1), (unsigned)sCardCount);
  canvas().setTextColor(COLOR_MUTED);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), position, 160, 216);
}

void drawNoGamesPage() {
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), "No upcoming games", 160, 100);
  // Diagnostics: proves the data task is publishing a schedule document.
  char diag[60];
  uint32_t ago = (getScheduleLastFetchAt() == 0)
                     ? 0
                     : (millis() - getScheduleLastFetchAt()) / 1000;
  snprintf(diag, sizeof(diag), "sched:%u ok:%u %s code:%d %us ago",
           (unsigned)getScheduleFetchAttempts(),
           (unsigned)getScheduleFetchSuccesses(), getScheduleLastError(),
           getScheduleLastHttpCode(), (unsigned)ago);
  canvas().setTextColor(COLOR_MUTED);
  canvas().setTextSize(1);
  drawCenteredText(canvas(), diag, 160, 132);
}

void drawLeaguePage() {
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, 8);
  canvas().print("AROUND THE LEAGUE");
  int y = 30;
  for (size_t i = 0; i < otherGameCount && i < 9; ++i) {
    const OtherGameInfo& g = otherGames[i];
    char line[36];
    snprintf(line, sizeof(line), "%s %2d  %2d %s", g.awayAbbrev, g.awayScore,
             g.homeScore, g.homeAbbrev);
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    canvas().setCursor(16, y);
    canvas().print(line);
    canvas().setTextColor(isLiveish(g.gameState) ? COLOR_LED_RED : COLOR_MUTED);
    canvas().setTextSize(1);
    canvas().setCursor(220, y + 4);
    canvas().print(g.gameState);
    y += 22;
  }
}

// ---- News story page: the headline as large centered white text filling
// the upper section, story details scrolling through a centered window
// (80% of the screen width); one story per page ----
int32_t sScrollPx = 0;
uint32_t sScrollMilliPx = 0;   // fractional scroll accumulator (milli-px)
uint32_t sLastScrollAt = 0;

const int STRIP_WIN_X = 32;   // centered 80%-width window
const int STRIP_WIN_W = 256;
const int STRIP_WIN_Y = 184;
const int STRIP_WIN_H = 34;
const int HEADLINE_TOP_Y = 14;
const int HEADLINE_MAX_W = 296;
const int HEADLINE_DIVIDER_Y = 172;
const int STRIP_CHARS = (STRIP_WIN_W - 16) / 12;  // text size 2 chars in window

// Greedy word-wrap line count (mirrors drawCenteredWrapped's algorithm;
// over-long words hard-split at the line width).
int countWrapLines(const char* s, int charsPerLine) {
  int lines = 1, cur = 0;
  const char* p = s;
  while (*p) {
    int wl = 0;
    while (p[wl] && p[wl] != ' ') ++wl;
    int add = (cur == 0) ? wl : wl + 1;
    if (cur + add > charsPerLine) {
      ++lines;
      cur = (wl > charsPerLine) ? charsPerLine : wl;
    } else {
      cur += add;
    }
    p += wl;
    if (*p == ' ') ++p;
  }
  return lines;
}

// Word-wrapped text with every line horizontally centered (the classic
// font is fixed-width, so wrapping by character count is exact).
void drawCenteredWrapped(const char* text, int topY, int maxW, int size) {
  const int charsPerLine = maxW / (6 * size);
  const int lineH = 8 * size + 4;
  char line[48];
  int lineLen = 0;
  const char* p = text;
  while (*p) {
    int wl = 0;
    while (p[wl] && p[wl] != ' ') ++wl;
    int add = (lineLen == 0) ? wl : wl + 1;
    if (lineLen + add > charsPerLine || wl > charsPerLine) {
      if (lineLen > 0) {  // flush the full line, retry the word below
        line[lineLen] = '\0';
        canvas().setTextSize(size);
        drawCenteredText(canvas(), line, 160, topY);
        topY += lineH;
        lineLen = 0;
        continue;
      }
      while (wl > charsPerLine) {  // word longer than a line: hard-split
        memcpy(line, p, charsPerLine);
        line[charsPerLine] = '\0';
        canvas().setTextSize(size);
        drawCenteredText(canvas(), line, 160, topY);
        topY += lineH;
        p += charsPerLine;
        wl -= charsPerLine;
      }
      lineLen = wl < (int)sizeof(line) - 1 ? wl : (int)sizeof(line) - 1;
      memcpy(line, p, lineLen);
    } else {
      if (lineLen > 0) line[lineLen++] = ' ';
      if (wl > (int)sizeof(line) - 1 - lineLen) wl = (int)sizeof(line) - 1 - lineLen;
      memcpy(line + lineLen, p, wl);
      lineLen += wl;
    }
    p += wl;
    if (*p == ' ') ++p;
  }
  if (lineLen > 0) {
    line[lineLen] = '\0';
    canvas().setTextSize(size);
    drawCenteredText(canvas(), line, 160, topY);
  }
}

// Largest text size whose wrapped headline fits the headline section
// (headline top to the divider). Short headlines get big type, long ones
// fall to 2.
int headlineSizeFor(const char* headline) {
  const int AVAIL_H = HEADLINE_DIVIDER_Y - 8 - HEADLINE_TOP_Y;
  const int sizes[] = {5, 4, 3, 2};
  for (int size : sizes) {
    int charW = 6 * size;
    if (countWrapLines(headline, HEADLINE_MAX_W / charW) * (8 * size + 4) <= AVAIL_H) {
      return size;
    }
  }
  return 2;
}

// Stock-ticker glyph renderer: the classic 5x7 font stretched vertically
// to 20 px (8 font rows -> 20, the ~25% bump over text size 2 the ticker
// asked for) with 2-px-wide columns — the same 12-px advance as size 2,
// so the scroll math is unchanged. Drawing is clipped to the strip
// window's inner area (columns slide in from the right during a step).
// Bit 0 of each font byte is the top pixel row.
void drawTickerText(int x, int y, const char* s, size_t maxChars,
                    uint16_t color) {
  static const int8_t kRowY[8] = {0, 2, 5, 7, 10, 12, 15, 17};  // floor(r*20/8)
  static const int8_t kRowH[8] = {2, 3, 2, 2, 3, 2, 3, 3};      // sum = 20
  const int xMin = STRIP_WIN_X + 1;
  const int xMax = STRIP_WIN_X + STRIP_WIN_W - 1;
  for (size_t k = 0; k < maxChars && s[k]; ++k) {
    unsigned char c = (unsigned char)s[k];
    if (c < 0x20 || c > 0x7E) c = ' ';
    const uint8_t* glyph = NEWS_FONT[c - 0x20];
    for (int col = 0; col < 5; ++col) {
      uint8_t bits = pgm_read_byte(&glyph[col]);
      if (!bits) continue;
      int px = x + (int)k * 12 + col * 2;
      if (px < xMin || px + 2 > xMax) continue;
      for (int r = 0; r < 8; ++r, bits >>= 1) {
        if (bits & 1) {
          canvas().fillRect(px, y + kRowY[r], 2, kRowH[r], color);
        }
      }
    }
  }
}

void drawStoryStrip(size_t idx) {
  const char* d = getNewsStory(idx).description;
  const int CHAR_W = 12;              // ticker glyph advance (see drawTickerText)
  canvas().fillRoundRect(STRIP_WIN_X, STRIP_WIN_Y, STRIP_WIN_W, STRIP_WIN_H,
                         4, COLOR_CARD);
  canvas().drawRoundRect(STRIP_WIN_X, STRIP_WIN_Y, STRIP_WIN_W, STRIP_WIN_H,
                         4, COLOR_MUTED);
  int first = sScrollPx / CHAR_W;
  if (first < (int)strlen(d)) {
    drawTickerText(STRIP_WIN_X + 8 - (sScrollPx % CHAR_W), STRIP_WIN_Y + 7,
                   d + first, STRIP_CHARS, COLOR_LED_RED);
  }
  tftPanel.pushRows(STRIP_WIN_X - 2, STRIP_WIN_Y - 2, STRIP_WIN_W + 4,
                    STRIP_WIN_H + 4);
}

void drawNewsStory(size_t idx) {
  const NewsStory& story = getNewsStory(idx);
  canvas().setTextColor(ST77XX_WHITE);
  drawCenteredWrapped(story.headline, HEADLINE_TOP_Y, HEADLINE_MAX_W,
                      headlineSizeFor(story.headline));
  canvas().drawLine(20, HEADLINE_DIVIDER_Y, 300, HEADLINE_DIVIDER_Y, COLOR_MUTED);
  drawStoryStrip(idx);
}

// ---- Upcoming-games list page: REMOVED — every upcoming game renders as
// its own detailed card in the interleaved carousel instead. ----

enum SlideKind { SLIDE_LEAGUE, SLIDE_CARD, SLIDE_STORY, SLIDE_DIAG };

// Slide 0 is the around-the-league page when games are on; the rest of
// the carousel interleaves upcoming-game cards with news stories (card,
// story, card, story...) and runs the longer tail of whichever side has
// more entries.
size_t leagueBase() { return otherGameCount > 0 ? 1 : 0; }

SlideKind slideKind(size_t slide) {
  if (leagueBase() > 0 && slide == 0) return SLIDE_LEAGUE;
  size_t i = slide - leagueBase();
  size_t cards = sCardCount, stories = getNewsStoryCount();
  if (cards == 0 && stories == 0) return SLIDE_DIAG;
  size_t m = cards < stories ? cards : stories;
  if (i < 2 * m) return (i % 2 == 0) ? SLIDE_CARD : SLIDE_STORY;
  return (cards > stories) ? SLIDE_CARD : SLIDE_STORY;
}

size_t cardIndexForSlide(size_t slide) {
  size_t i = slide - leagueBase();
  size_t m = sCardCount < getNewsStoryCount() ? sCardCount : getNewsStoryCount();
  return (i < 2 * m) ? (i / 2) : (i - m);
}

size_t storyIndexForSlide(size_t slide) {
  size_t i = slide - leagueBase();
  size_t m = sCardCount < getNewsStoryCount() ? sCardCount : getNewsStoryCount();
  return (i < 2 * m) ? (i / 2) : (i - m);
}

size_t pageCount() {
  size_t n = leagueBase();
  if (sCardCount == 0 && getNewsStoryCount() == 0) {
    return n + 1;  // plus the no-games diagnostic page
  }
  return n + sCardCount + getNewsStoryCount();
}

// Draws the current carousel page. resetScroll=false is used when a
// schedule publish repaints the SAME slide: the story scroll continues
// where it was, so frequent republishes can't restart (and thereby never
// finish) a story. Rotation passes true — a new slide starts at scroll 0.
void drawCurrentPage(bool resetScroll) {
  canvas().fillScreen(COLOR_BG);
  size_t pages = pageCount();
  size_t slide = pages ? tickerSlide % pages : 0;
  SlideKind kind = slideKind(slide);
  size_t storyIdx = (kind == SLIDE_STORY) ? storyIndexForSlide(slide)
                                          : (size_t)-1;
  static size_t sLastStoryIdx = (size_t)-1;
  if (resetScroll || storyIdx != sLastStoryIdx) {
    sScrollPx = 0;
    sScrollMilliPx = 0;
    DBG_PRINTF("[CAR] draw slide=%u/%u kind=%d scroll=reset\n",
               (unsigned)slide, (unsigned)pages, (int)kind);
  } else {
    DBG_PRINTF("[CAR] repaint slide=%u/%u kind=%d scroll=%ldpx kept\n",
               (unsigned)slide, (unsigned)pages, (int)kind, (long)sScrollPx);
  }
  sLastStoryIdx = storyIdx;
  switch (kind) {
    case SLIDE_LEAGUE: drawLeaguePage(); break;
    case SLIDE_CARD:   drawUpcomingCard(cardIndexForSlide(slide)); break;
    case SLIDE_STORY:  drawNewsStory(storyIdx); break;
    case SLIDE_DIAG:   drawNoGamesPage(); break;
  }
  tftPanel.pushFull();
}

}  // namespace

void publishNewsStory(const NewsSlotUpdate& slot) {
  if (slot.index >= MAX_NEWS_STORIES) return;
  strlcpy(sNews[slot.index].headline, slot.headline,
           sizeof(sNews[slot.index].headline));
  strlcpy(sNews[slot.index].description, slot.description,
           sizeof(sNews[slot.index].description));
}
void setNewsStoryCount(size_t count) {
  sNewsCount = count > MAX_NEWS_STORIES ? MAX_NEWS_STORIES : count;
  sNewsIndex = 0;
}
size_t getNewsStoryCount() { return sNewsCount; }
size_t getNewsStoryIndex() { return sNewsIndex; }
void advanceNewsStoryIndex() {
  if (sNewsCount > 0) sNewsIndex = (sNewsIndex + 1) % sNewsCount;
}
const NewsStory& getNewsStory(size_t index) {
  return sNews[index % MAX_NEWS_STORIES];
}

void renderWaiting(JsonObjectConst dayScoreJson, const int preferredTeamIds[3]) {
  (void)preferredTeamIds;  // cards cover every upcoming game, not just these
  nhl_render::hasCurrentLiveGame = false;
  sSched = dayScoreJson;
  buildUpcomingCards(sSched);
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);
  invalidateTm1637WallClock();  // wall clock repaints on the next tick
  drawCurrentPage(false);  // schedule repaint: same slide keeps its scroll
}

void rotateCarousel() {
  uint32_t now = millis();
  size_t pages = pageCount();
  if (pages == 0) return;
  size_t slide = nhl_render::tickerSlide % pages;
  SlideKind kind = slideKind(slide);

  bool shouldAdvance = false;
  if (kind == SLIDE_STORY) {
    // Story pages scroll their detail strip continuously (called every
    // loop pass) and advance only once the WHOLE description has passed
    // through the window, plus a short end hold. Descriptions shorter
    // than the window have nothing to scroll and fall back to the timed
    // dwell. The milli-pixel accumulator keeps the speed exact even when
    // individual loop passes are too short to yield a whole pixel.
    uint32_t dt = now - (sLastScrollAt == 0 ? now : sLastScrollAt);
    sLastScrollAt = now;
    sScrollMilliPx += dt * NHL_NEWS_SCROLL_PX_PER_SEC;
    sScrollPx = (int32_t)(sScrollMilliPx / 1000);
    const char* d = getNewsStory(storyIndexForSlide(slide)).description;
    int maxScroll = (int)strlen(d) * 12 - STRIP_CHARS * 12;
    if (maxScroll < 0) maxScroll = 0;
    int endHold = maxScroll + NHL_NEWS_SCROLL_END_HOLD_PX;
    if (sScrollPx > endHold) {
      sScrollPx = endHold;
      sScrollMilliPx = (uint32_t)endHold * 1000;
    }
    if (maxScroll > 0) {
      static int32_t lastDrawn = -9999;
      if (sScrollPx - lastDrawn >= 6 || lastDrawn - sScrollPx >= 6) {
        lastDrawn = sScrollPx;
        drawStoryStrip(storyIndexForSlide(slide));
      }
    }
    shouldAdvance = (maxScroll > 0) ? sScrollPx >= endHold
        : now - nhl_render::lastCarouselTime >= NHL_NEWS_STORY_DWELL_MS;
  } else {
    sLastScrollAt = now;
    uint32_t dwell = (kind == SLIDE_CARD) ? NHL_UPCOMING_GAMES_ROTATE_MS
                                          : NHL_CAROUSEL_ROTATE_MS;
    shouldAdvance = now - nhl_render::lastCarouselTime >= dwell;
  }

  if (!shouldAdvance) return;
  nhl_render::lastCarouselTime = now;
  DBG_PRINTF("[CAR] advance from kind=%d slide=%u\n", (int)kind,
             (unsigned)slide);
  nhl_render::tickerSlide++;
  if (!nhl_render::hasCurrentLiveGame) drawCurrentPage(true);  // new slide
}

void updateOtherGames(const ScheduleSnapshot& schedule, long excludeGameId) {
  size_t n = 0;
  for (size_t i = 0; i < schedule.count && n < MAX_OTHER_GAMES; ++i) {
    const NhlDayGame& g = schedule.games[i];
    if (g.gameId == excludeGameId) continue;
    OtherGameInfo& o = nhl_render::otherGames[n++];
    strlcpy(o.awayAbbrev, g.awayAbbrev, sizeof(o.awayAbbrev));
    strlcpy(o.homeAbbrev, g.homeAbbrev, sizeof(o.homeAbbrev));
    o.awayScore = g.awayScore;
    o.homeScore = g.homeScore;
    strlcpy(o.gameState, g.gameState, sizeof(o.gameState));
  }
  nhl_render::otherGameCount = n;
}
