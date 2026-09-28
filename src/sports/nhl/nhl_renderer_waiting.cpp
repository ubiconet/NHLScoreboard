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

// Waiting-mode screen, MLB-style carousel: upcoming-game cards — one per
// preferred team, each showing away @ home with logos + 3-letter
// abbreviations, the local game date/time and a STARTS IN countdown — an
// around-the-league slide when games are on, then news story pages —
// white headline with the story details scrolling beneath, one story at a
// time — with an upcoming-games list sprinkled in after every third
// story. rotateCarousel() advances slides (stories dwell longer so their
// scroll can run) and animates the story scroll between advances; the app
// re-renders whenever a fresh day-score publish lands. Matrices/penalty
// LEDs are cleared on entry (the idle clock on the TM1637 is driven by
// the app via updateTm1637WallClock).

namespace {

using namespace nhl_render;

GFXcanvas16& canvas() { return tftPanel.canvas(); }

// ---- News slot storage (renderer-owned; core-0 publishes into it) ----
NewsStory sNews[MAX_NEWS_STORIES];
size_t sNewsCount = 0;
size_t sNewsIndex = 0;

JsonObjectConst sSched;      // view into nhl_state's cached day-score doc
int sPreferred[3] = {0, 0, 0};

bool isLiveish(const char* state) {
  return strcmp(state, "LIVE") == 0 || strcmp(state, "CRIT") == 0;
}
bool isUpcoming(const char* state) {
  return strcmp(state, "FUT") == 0 || strcmp(state, "PREVIEW") == 0;
}

// ---- Upcoming-game cards (one per preferred team, MLB-style) ----
const size_t MAX_UPCOMING_CARDS = 3;

struct UpcomingCard {
  char awayAbbrev[4];
  char homeAbbrev[4];
  int  awayId, homeId;
  long gameId;        // de-dupes when two preferred teams share a game
  time_t startUtc;
};
UpcomingCard sCards[MAX_UPCOMING_CARDS];
size_t sCardCount = 0;

// Rebuilds the card list from the cached day-score doc: for each preferred
// team (priority order) its next upcoming game; when no preferred team is
// scheduled, the slate's first upcoming games instead so the carousel still
// shows something. Games without a parseable start time are skipped.
void buildUpcomingCards(JsonObjectConst sched, const int preferred[3]) {
  sCardCount = 0;
  JsonArrayConst games = sched["games"].as<JsonArrayConst>();
  if (games.isNull()) return;
  for (int p = 0; p < 3 && sCardCount < MAX_UPCOMING_CARDS; ++p) {
    if (preferred[p] == 0) continue;
    for (JsonObjectConst g : games) {
      if (!isUpcoming(g["gameState"] | "")) continue;
      int a = g["awayTeam"]["id"] | 0, h = g["homeTeam"]["id"] | 0;
      if (a != preferred[p] && h != preferred[p]) continue;
      long gid = g["id"] | 0;
      bool dup = false;
      for (size_t i = 0; i < sCardCount; ++i) {
        if (sCards[i].gameId == gid) { dup = true; break; }
      }
      if (dup) break;  // this preference's game is already carded
      time_t start = 0;
      if (!isoDateToEpoch(g["startTimeUTC"] | "", start) || start == 0) continue;
      UpcomingCard& c = sCards[sCardCount++];
      strlcpy(c.awayAbbrev, g["awayTeam"]["abbrev"] | "??", 4);
      strlcpy(c.homeAbbrev, g["homeTeam"]["abbrev"] | "??", 4);
      c.awayId = a; c.homeId = h;
      c.gameId = gid; c.startUtc = start;
      break;
    }
  }
  if (sCardCount > 0) return;
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
    c.gameId = g["id"] | 0;
    c.startUtc = start;
  }
}

void drawHeaderBar() {
  canvas().fillRect(0, 0, 320, 24, COLOR_CARD);
  canvas().setTextSize(2);
  canvas().setTextColor(COLOR_GOLD);
  drawCenteredText(canvas(), "NHL SCOREBOARD", 160, 5);
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
  drawHeaderBar();
  canvas().setTextColor(ST77XX_WHITE);
  canvas().setTextSize(2);
  drawCenteredText(canvas(), "No upcoming games", 160, 110);
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
  drawCenteredText(canvas(), diag, 160, 140);
}

void drawLeaguePage() {
  drawHeaderBar();
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, 32);
  canvas().print("AROUND THE LEAGUE");
  int y = 50;
  for (size_t i = 0; i < otherGameCount && i < 8; ++i) {
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

// ---- News story page (MLB-style): white headline scaled to fill the
// main section, story details scrolling in a centered window (80% of the
// screen width); one story per page ----
int32_t sScrollPx = 0;
uint32_t sLastScrollAt = 0;

const int STRIP_WIN_X = 32;   // centered 80%-width window
const int STRIP_WIN_W = 256;
const int STRIP_WIN_Y = 148;
const int STRIP_WIN_H = 34;

// Greedy word-wrap line count (mirrors drawWrappedText's algorithm).
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

// Largest text size whose wrapped headline fits the main news section
// (y 46..~122). Bigger headlines get bigger type, long ones fall to 2.
int headlineSizeFor(const char* headline) {
  const int AVAIL_H = 76;
  const int sizes[] = {4, 3, 2};
  for (int size : sizes) {
    int charW = 6 * size;
    if (countWrapLines(headline, 296 / charW) * (8 * size) <= AVAIL_H) {
      return size;
    }
  }
  return 2;
}

void drawStoryStrip(size_t idx) {
  const char* d = getNewsStory(idx).description;
  const int CHAR_W = 12;              // text size 2
  const int MAX_CHARS = 20;           // 20 x 12 = 240 <= window inner width
  canvas().fillRoundRect(STRIP_WIN_X, STRIP_WIN_Y, STRIP_WIN_W, STRIP_WIN_H,
                         4, COLOR_CARD);
  canvas().drawRoundRect(STRIP_WIN_X, STRIP_WIN_Y, STRIP_WIN_W, STRIP_WIN_H,
                         4, COLOR_MUTED);
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(2);
  int first = sScrollPx / CHAR_W;
  if (first < (int)strlen(d)) {
    canvas().setCursor(STRIP_WIN_X + 8 - (sScrollPx % CHAR_W), STRIP_WIN_Y + 9);
    printClipped(canvas(), d + first, MAX_CHARS);
  }
  tftPanel.pushRows(STRIP_WIN_X - 2, STRIP_WIN_Y - 2, STRIP_WIN_W + 4,
                    STRIP_WIN_H + 4);
}

void drawNewsStory(size_t idx) {
  size_t n = getNewsStoryCount();
  drawHeaderBar();
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, 32);
  canvas().print("NHL NEWS");
  char tag[10];
  snprintf(tag, sizeof(tag), "%u/%u", (unsigned)(idx + 1), (unsigned)n);
  canvas().setTextColor(COLOR_MUTED);
  canvas().setCursor(300 - strlen(tag) * 6, 33);
  canvas().print(tag);
  int size = headlineSizeFor(getNewsStory(idx).headline);
  canvas().setTextColor(ST77XX_WHITE);
  drawWrappedText(canvas(), getNewsStory(idx).headline, 12, 46, 296, 6 * size,
                  8 * size, 4);
  canvas().drawLine(12, 130, 308, 130, COLOR_MUTED);
  drawStoryStrip(idx);
}

// ---- Upcoming-games list page ("sprinkled" between story pages) ----
void drawUpcomingListPage() {
  drawHeaderBar();
  canvas().setTextColor(COLOR_GOLD);
  canvas().setTextSize(1);
  canvas().setCursor(12, 32);
  canvas().print("UPCOMING GAMES");
  int y = 54;
  JsonArrayConst games = sSched["games"].as<JsonArrayConst>();
  if (games.isNull()) return;
  for (JsonObjectConst g : games) {
    if (y > 210) break;
    if (!isUpcoming(g["gameState"] | "")) continue;
    time_t start = 0;
    isoDateToEpoch(g["startTimeUTC"] | "", start);
    if (start == 0) continue;
    tm lt = {};
    localtime_r(&start, &lt);
    char when[16];
    strftime(when, sizeof(when), "%a %l:%M%p", &lt);
    canvas().setTextColor(COLOR_MUTED);
    canvas().setTextSize(1);
    canvas().setCursor(16, y + 6);
    canvas().print(when);
    char matchup[12];
    snprintf(matchup, sizeof(matchup), "%s @ %s",
             g["awayTeam"]["abbrev"] | "??", g["homeTeam"]["abbrev"] | "??");
    canvas().setTextColor(ST77XX_WHITE);
    canvas().setTextSize(2);
    canvas().setCursor(150, y);
    canvas().print(matchup);
    y += 26;
  }
}

enum SlideKind { SLIDE_CARD, SLIDE_DIAG, SLIDE_LEAGUE, SLIDE_STORY, SLIDE_LIST };

// Card slides lead the carousel; when there are none, the single slide 0
// is the no-games diagnostics page instead.
size_t cardSlideCount() { return sCardCount > 0 ? sCardCount : 1; }

size_t mixedBase() { return cardSlideCount() + (otherGameCount > 0 ? 1 : 0); }

SlideKind slideKind(size_t slide) {
  if (slide < cardSlideCount()) return sCardCount > 0 ? SLIDE_CARD : SLIDE_DIAG;
  if (otherGameCount > 0 && slide == cardSlideCount()) return SLIDE_LEAGUE;
  size_t k = slide - mixedBase();
  if (getNewsStoryCount() == 0) return SLIDE_LIST;
  // Groups of four slides: three stories, then the upcoming list.
  return (k % 4 == 3) ? SLIDE_LIST : SLIDE_STORY;
}

size_t storyIndexForSlide(size_t slide) {
  size_t k = slide - mixedBase();
  return (k / 4) * 3 + (k % 4);
}

size_t pageCount() {
  size_t n = cardSlideCount();  // cards (or the no-games diagnostic page)
  if (otherGameCount > 0) ++n;
  size_t news = getNewsStoryCount();
  if (news > 0) {
    n += news + news / 3;  // stories + one list page per full trio
  } else if (sCardCount > 0) {
    ++n;  // no stories: append one upcoming-list overview page
  }
  return n;
}

void drawCurrentPage() {
  canvas().fillScreen(COLOR_BG);
  size_t pages = pageCount();
  size_t slide = pages ? tickerSlide % pages : 0;
  sScrollPx = 0;
  switch (slideKind(slide)) {
    case SLIDE_CARD:   drawUpcomingCard(slide); break;
    case SLIDE_DIAG:   drawNoGamesPage(); break;
    case SLIDE_LEAGUE: drawLeaguePage(); break;
    case SLIDE_STORY:  drawNewsStory(storyIndexForSlide(slide)); break;
    case SLIDE_LIST:   drawUpcomingListPage(); break;
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
  nhl_render::hasCurrentLiveGame = false;
  sSched = dayScoreJson;
  for (int i = 0; i < 3; ++i) sPreferred[i] = preferredTeamIds[i];
  buildUpcomingCards(sSched, sPreferred);
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);
  invalidateTm1637WallClock();  // wall clock repaints on the next tick
  drawCurrentPage();
}

void rotateCarousel() {
  uint32_t now = millis();
  size_t pages = pageCount();
  if (pages == 0) return;
  size_t slide = nhl_render::tickerSlide % pages;

  // Story pages scroll their detail strip continuously (called every
  // loop pass); other pages just refresh the scroll clock.
  if (slideKind(slide) == SLIDE_STORY) {
    uint32_t dt = now - (sLastScrollAt == 0 ? now : sLastScrollAt);
    sLastScrollAt = now;
    sScrollPx += (int32_t)(dt * NHL_NEWS_SCROLL_PX_PER_SEC / 1000);
    const char* d = getNewsStory(storyIndexForSlide(slide)).description;
    int maxScroll = (int)strlen(d) * 12 - 240;  // last 20 chars visible
    if (maxScroll > 0) {
      if (sScrollPx > maxScroll + 30) sScrollPx = maxScroll + 30;  // end hold
      static int32_t lastDrawn = -9999;
      if (sScrollPx - lastDrawn >= 6 || lastDrawn - sScrollPx >= 6) {
        lastDrawn = sScrollPx;
        drawStoryStrip(storyIndexForSlide(slide));
      }
    }
  } else {
    sLastScrollAt = now;
  }

  SlideKind kind = slideKind(slide);
  uint32_t dwell = (kind == SLIDE_STORY) ? NHL_NEWS_STORY_DWELL_MS
                   : (kind == SLIDE_CARD) ? NHL_UPCOMING_GAMES_ROTATE_MS
                                          : NHL_CAROUSEL_ROTATE_MS;
  if (now - nhl_render::lastCarouselTime < dwell) return;
  nhl_render::lastCarouselTime = now;
  nhl_render::tickerSlide++;
  if (!nhl_render::hasCurrentLiveGame) drawCurrentPage();
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
