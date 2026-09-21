#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>

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

// Waiting-mode renderer: upcoming-game cards with countdowns, the MLB news
// marquee, the "no upcoming games" diagnostic screen, and the carousel that
// alternates them. Also owns the news story slots the core-0 data task
// publishes into.

namespace {
using namespace mlb_render;

// NewsStory slots live at file scope so the core-0 data task can publish into
// them through the public accessors at the bottom of this file.
NewsStory newsStories[MAX_NEWS_STORIES];
size_t    newsStoryCount = 0;
size_t    newsStoryIndex = 0;

// Upcoming-games carousel alternates a news-story slide with each game card:
// story 1, game 1, story 2, game 2, ...
enum class UpcomingSubPage : uint8_t { NEWS_STORY = 0, GAME_INFO = 1, COUNT = 2 };
UpcomingSubPage upcomingSubPage = UpcomingSubPage::NEWS_STORY;

int newsTickerX = 0;   // current marquee offset (derived from newsTickerStartedAt)
uint32_t newsTickerStartedAt = 0;  // millis() when this story's scroll began

struct UpcomingGameInfo {
  int awayTeamId;
  int homeTeamId;
  char awayName[40];
  char homeName[40];
  char gameDate[32];
};
UpcomingGameInfo upcomingGames[3];
size_t upcomingGameCount = 0;
size_t upcomingGameIndex = 0;

bool formatGameDateLocal(const char* gameDate, char* dateOutput, size_t dateOutputSize,
                         char* timeOutput, size_t timeOutputSize) {
  time_t epoch;
  if (!isoDateToEpoch(gameDate, epoch)) return false;

  tm localTime = {};
  localtime_r(&epoch, &localTime);
  return strftime(dateOutput, dateOutputSize, "%a, %b %d", &localTime) > 0 &&
         strftime(timeOutput, timeOutputSize, "%I:%M %p", &localTime) > 0;
}

// Formats the time remaining until gameDate as "DD:HH:MM"; false if it has already started
// or the clock hasn't been synced yet.
bool formatCountdown(const char* gameDate, char* output, size_t outputSize) {
  time_t epoch;
  if (!isoDateToEpoch(gameDate, epoch)) return false;

  time_t now = time(nullptr);
  if (!timeIsSynced()) return false; // clock not yet synced
  if (epoch <= now) return false;

  long remaining = static_cast<long>(epoch - now);
  int days = remaining / 86400;
  int hours = (remaining % 86400) / 3600;
  int minutes = (remaining % 3600) / 60;
  snprintf(output, outputSize, "%02d:%02d:%02d", days, hours, minutes);
  return true;
}

JsonObjectConst findNextGame(JsonObjectConst scheduleRoot, int teamId) {
  if (teamId == 0) return JsonObjectConst();

  for (JsonObjectConst date : scheduleRoot["dates"].as<JsonArrayConst>()) {
    for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
      const char* state = game["status"]["abstractGameState"] | "";
      if (strcmp(state, "Final") == 0) continue;

      int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
      int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
      if (awayTeamId == teamId || homeTeamId == teamId) return game;
    }
  }
  return JsonObjectConst();
}

void drawTeamName(Adafruit_GFX& target, const char* name, int centerX, int topY) {
  char line1[24] = {};
  char line2[24] = {};
  char words[40];
  strlcpy(words, name != nullptr ? name : "", sizeof(words));

  char* word = strtok(words, " ");
  while (word != nullptr) {
    char candidate[24];
    if (line1[0] == '\0') {
      snprintf(candidate, sizeof(candidate), "%s", word);
    } else {
      snprintf(candidate, sizeof(candidate), "%s %s", line1, word);
    }

    if (strlen(candidate) <= 18 && line2[0] == '\0') {
      strlcpy(line1, candidate, sizeof(line1));
    } else if (line2[0] == '\0') {
      strlcpy(line2, word, sizeof(line2));
    } else {
      size_t line2Length = strlen(line2);
      if (line2Length + strlen(word) + 1 < sizeof(line2)) {
        snprintf(line2 + line2Length, sizeof(line2) - line2Length,
                 "%s%s", line2Length > 0 ? " " : "", word);
      }
    }
    word = strtok(nullptr, " ");
  }

  target.setTextColor(ST77XX_WHITE);
  target.setTextSize(1);
  if (line1[0] != '\0') drawCenteredText(target, line1, centerX, topY);
  if (line2[0] != '\0') drawCenteredText(target, line2, centerX, topY + 12);
}

void drawGamePositionFooter(GFXcanvas16& canvas) {
  canvas.drawLine(22, 204, 298, 204, COLOR_CARD);
  char position[20];
  snprintf(position, sizeof(position), "GAME %u OF %u",
           static_cast<unsigned int>(upcomingGameIndex + 1),
           static_cast<unsigned int>(upcomingGameCount));
  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(2);
  drawCenteredText(canvas, position, 160, 216);
}

void renderUpcomingGame() {
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  if (upcomingGameCount == 0) {
    // Diagnostic: report what the data task has actually published so we can
    // tell at a glance whether the schedule JSON is empty, hasn't been
    // fetched yet, or just doesn't include the user's preferred teams.
    uint32_t publishedAt = getUpcomingSchedulePublishedAt();
    size_t    docSize    = getUpcomingScheduleDocSize();
    JsonObjectConst sched = getUpcomingScheduleJson();
    size_t dateCount = 0;
    size_t totalGames = 0;
    for (JsonObjectConst date : sched["dates"].as<JsonArrayConst>()) {
      ++dateCount;
      JsonArrayConst games = date["games"].as<JsonArrayConst>();
      if (!games.isNull()) totalGames += games.size();
    }
    uint32_t attempts   = getScheduleFetchAttempts();
    uint32_t successes  = getScheduleFetchSuccesses();
    const char* lastErr = getScheduleLastError();
    int         httpCode= getScheduleLastHttpCode();
    const char* lastUrl = getScheduleLastUrl();
    uint32_t    fetchAt = getScheduleLastFetchAt();

    char line1[40], line2[40], line3[40], line4[40], line5[40];
    if (publishedAt == 0) {
      // The data task hasn't successfully published yet. Surface whether
      // it's been trying at all and what the last failure was.
      snprintf(line1, sizeof(line1), "No upcoming games found");
      if (attempts == 0) {
        snprintf(line2, sizeof(line2), "Data task has not run yet");
        snprintf(line3, sizeof(line3), "Waiting for first online tick");
        line4[0] = '\0';
        line5[0] = '\0';
      } else {
        uint32_t sinceFetch = (fetchAt > 0) ? (millis() - fetchAt) / 1000 : 0;
        snprintf(line2, sizeof(line2), "att:%lu ok:%lu err:%s http:%d",
                 (unsigned long)attempts, (unsigned long)successes,
                 lastErr, httpCode);
        snprintf(line3, sizeof(line3), "%lu s since last fetch", (unsigned long)sinceFetch);
        snprintf(line4, sizeof(line4), "%s", lastUrl);
        snprintf(line5, sizeof(line5), "Waiting for first success");
      }
    } else {
      uint32_t ageSec = (millis() - publishedAt) / 1000;
      snprintf(line1, sizeof(line1), "No upcoming games found");
      snprintf(line2, sizeof(line2), "att:%lu ok:%lu err:%s http:%d",
               (unsigned long)attempts, (unsigned long)successes,
               lastErr, httpCode);
      snprintf(line3, sizeof(line3), "sched:%uB %udates %ugames %lus ago",
               (unsigned)docSize, (unsigned)dateCount, (unsigned)totalGames,
               (unsigned long)ageSec);
      snprintf(line4, sizeof(line4), "%s", lastUrl);
      snprintf(line5, sizeof(line5), "Carousel refreshes every 60s");
    }
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, line1, 160, 60);
    canvas.setTextColor(COLOR_GOLD);
    drawCenteredText(canvas, line2, 160, 80);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, line3, 160, 100);
    drawCenteredText(canvas, line4, 160, 120);
    drawCenteredText(canvas, line5, 160, 140);
    tftPanel.pushFull();
    return;
  }

  const UpcomingGameInfo& game = upcomingGames[upcomingGameIndex];
  const char* homeAbbrev = mlbTeamAbbrev(game.homeTeamId, nullptr);
  const char* awayAbbrev = mlbTeamAbbrev(game.awayTeamId, nullptr);
  char localDate[24];
  char localTime[16];
  bool hasLocalDateTime = formatGameDateLocal(game.gameDate, localDate, sizeof(localDate),
                                              localTime, sizeof(localTime));
  char dateTimeLine[48];
  if (hasLocalDateTime) {
    snprintf(dateTimeLine, sizeof(dateTimeLine), "%s - %s", localDate, localTime);
  } else {
    snprintf(dateTimeLine, sizeof(dateTimeLine), "DATE & TIME TBD");
  }

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, dateTimeLine, 160, 15);

  char countdown[16];
  if (formatCountdown(game.gameDate, countdown, sizeof(countdown))) {
    char countdownLine[32];
    snprintf(countdownLine, sizeof(countdownLine), "STARTS IN %s", countdown);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(2);
    drawCenteredText(canvas, countdownLine, 160, 48);
  }

  const int logoSize = 88;
  drawTeamLogoScaled(canvas, 20, 82, game.homeTeamId, homeAbbrev, logoSize);
  drawTeamLogoScaled(canvas, 212, 82, game.awayTeamId, awayAbbrev, logoSize);

  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "HOSTS", 160, 121);
  drawTeamName(canvas, game.homeName, 64, 176);
  drawTeamName(canvas, game.awayName, 256, 176);

  drawGamePositionFooter(canvas);

  tftPanel.pushFull();
}

// The news ticker occupies canvas rows 160..239 (TFT bottom strip). Only
// the text band inside it changes from frame to frame; the geometry
// follows the text size (classic 5x7 Adafruit font scaled by
// NEWS_TICKER_TEXT_SIZE). The scrolling text lives inside a 240-px window
// (margins show the static card background): the per-frame push — and
// with it the visible tear, which equals speed x push time — shrinks 25%
// compared to pushing the full 320-px width.
static const int NEWS_TICKER_TEXT_SIZE = 4;                        // 24x32 px char cells
static const int NEWS_TICKER_CHAR_W    = 6 * NEWS_TICKER_TEXT_SIZE;
static const int NEWS_TICKER_TEXT_Y    = 160 + (80 - 8 * NEWS_TICKER_TEXT_SIZE) / 2;  // 184
static const int NEWS_TICKER_BAND_TOP  = NEWS_TICKER_TEXT_Y - 1;   // 183
static const int NEWS_TICKER_BAND_H    = 8 * NEWS_TICKER_TEXT_SIZE + 2;  // 34
static const int NEWS_TICKER_WINDOW_X  = 40;                       // scrolling text window
static const int NEWS_TICKER_WINDOW_W  = 240;
static const int NEWS_TICKER_WINDOW_R  = NEWS_TICKER_WINDOW_X + NEWS_TICKER_WINDOW_W;

// Painted once per news slide (not per frame): the card background for the
// whole strip plus the red hairlines, which are static while the text
// scrolls past inside the band.
void drawNewsTickerStatic() {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillRect(0, 160, 320, 80, COLOR_CARD);
  canvas.drawFastHLine(0, 160, 320, COLOR_LED_RED);
  canvas.drawFastHLine(0, 239, 320, COLOR_LED_RED);
  tftPanel.pushBand(160, 80);
}

// Draws one frame of the scrolling news ticker: repaints only the text band
// of the already-allocated canvas and pushes just that band to the TFT. An
// earlier design pre-rasterized the strip into a side GFXcanvas16 sprite
// sized (320 + textWidth) x 80, but a bare 320x80 RGB565 sprite is 51.2 KB —
// more heap than this device ever has free — so the allocation always failed
// and the news slide flashed past in ~200 ms. Drawing only the visible
// characters per frame costs no extra heap. The text is laid out to the
// right of the window and advances one character cell at a time (see
// rotateCarousel's LED-marquee stepping), entering from the right and
// exiting left.
void drawNewsTickerFrame(int offset) {
  GFXcanvas16& canvas = tftPanel.canvas();
  uint32_t startedAt = micros();

  // Repaint + push ONLY the 240-px window and the text rows; the static
  // margins and hairlines (drawNewsTickerStatic) stay untouched. Fewer
  // pushed pixels = shorter sweep = proportionally smaller tear step.
  canvas.fillRect(NEWS_TICKER_WINDOW_X, NEWS_TICKER_BAND_TOP,
                  NEWS_TICKER_WINDOW_W, NEWS_TICKER_BAND_H, COLOR_CARD);

  const NewsStory& story = newsStories[newsStoryIndex];
  const char* text = story.description[0] != '\0' ? story.description
                                                  : story.headline;

  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(NEWS_TICKER_TEXT_SIZE);
  canvas.setTextWrap(false);
  // Chars whose left edge is left of the window are skipped; the push
  // clips partial chars at the window's right edge.
  int firstChar =
      (offset > NEWS_TICKER_WINDOW_R)
          ? (offset - NEWS_TICKER_WINDOW_R + NEWS_TICKER_CHAR_W - 1) /
                NEWS_TICKER_CHAR_W
          : 0;
  canvas.setCursor(NEWS_TICKER_WINDOW_R + firstChar * NEWS_TICKER_CHAR_W - offset,
                   NEWS_TICKER_TEXT_Y);
  for (const char* p = text + firstChar; *p != '\0'; ++p) {
    if (canvas.getCursorX() >= NEWS_TICKER_WINDOW_R + NEWS_TICKER_CHAR_W) break;
    canvas.print(*p);
  }
  canvas.setTextWrap(true);
  // Push the window row by row (TftPanel::pushRows): a single call for the
  // whole band would stride through the wrong memory and garble the text.
  tftPanel.pushRows(NEWS_TICKER_WINDOW_X, NEWS_TICKER_BAND_TOP,
                    NEWS_TICKER_WINDOW_W, NEWS_TICKER_BAND_H);

  // One timing line per story (not per step) so the per-step push cost is
  // visible on Serial when tuning MLB_NEWS_TICKER_STEP_MS.
  static size_t sTimedStory = SIZE_MAX;
  if (sTimedStory != newsStoryIndex) {
    sTimedStory = newsStoryIndex;
    DBG_PRINTF("[TICKER] step frame %lu us; %lu ms per %d-px cell\n",
               (unsigned long)(micros() - startedAt),
               (unsigned long)MLB_NEWS_TICKER_STEP_MS, NEWS_TICKER_CHAR_W);
  }
}

// Total pixel width of the current story's ticker text. Used by
// rotateCarousel() to detect when it has scrolled past.
int newsTickerTextPx() {
  const NewsStory& story = newsStories[newsStoryIndex];
  const char* text = story.description[0] != '\0' ? story.description
                                                  : story.headline;
  return (int)strlen(text) * NEWS_TICKER_CHAR_W;
}

// Renders one MLB news headline/description slide, shown between upcoming-game cards.
void renderNewsStory() {
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(1);
  drawCenteredText(canvas, "MLB NEWS", 160, 10);

  if (newsStoryCount == 0) {
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, "No news available", 160, 104);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "News refreshes every 30 min", 160, 124);
    tftPanel.pushFull();
    return;
  }

  const NewsStory& story = newsStories[newsStoryIndex];

  // Headline fills the top two-thirds in bold white block text.
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(2);
  drawWrappedText(canvas, story.headline, 16, 32, 288, 12, 22, 6);

  // Paint the static strip (background + hairlines) once, then the ticker's
  // first frame into the text band, then send the headline section above.
  drawNewsTickerStatic();
  newsTickerStartedAt = millis();
  newsTickerX = 0;
  drawNewsTickerFrame(newsTickerX);
  tftPanel.pushBand(0, 160);
}

// Dispatches to the current upcoming-games carousel sub-page (news story or game card).
void renderUpcomingCarouselPage() {
  if (upcomingGameCount == 0) {
    renderUpcomingGame();
    return;
  }
  // Skip the news slide entirely when no stories are available, rather than showing a blank one.
  UpcomingSubPage pageToRender = upcomingSubPage;
  if (pageToRender == UpcomingSubPage::NEWS_STORY && newsStoryCount == 0) {
    pageToRender = UpcomingSubPage::GAME_INFO;
  }
  switch (pageToRender) {
    case UpcomingSubPage::NEWS_STORY:
      renderNewsStory();
      break;
    case UpcomingSubPage::GAME_INFO:
    default:
      renderUpcomingGame();
      break;
  }
}
} // namespace

void renderWaiting(JsonObjectConst upcomingSchedule,
                   JsonObjectConst standings,
                   const int preferredTeamIds[3]) {
  hasCurrentLiveGame = false;
  invalidateMax7219Clock();
  otherGameCount = 0;
  tickerSlide = 0;
  upcomingGameCount = 0;
  upcomingGameIndex = 0;
  upcomingSubPage = UpcomingSubPage::NEWS_STORY;
  newsTickerStartedAt = millis();
  newsTickerX = 0;
  currentStandings = standings;
  int listedTeamIds[3] = {};

  for (size_t priority = 0; priority < 3; priority++) {
    int selectedTeamId = preferredTeamIds[priority];
    if (selectedTeamId == 0) continue;

    bool alreadyListed = false;
    for (size_t index = 0; index < upcomingGameCount; index++) {
      if (selectedTeamId == listedTeamIds[index]) {
        alreadyListed = true;
        break;
      }
    }
    if (alreadyListed) continue;

    JsonObjectConst game = findNextGame(upcomingSchedule, selectedTeamId);
    if (game.isNull()) continue;

    upcomingGames[upcomingGameCount].awayTeamId =
        game["teams"]["away"]["team"]["id"] | 0;
    upcomingGames[upcomingGameCount].homeTeamId =
        game["teams"]["home"]["team"]["id"] | 0;
    strlcpy(upcomingGames[upcomingGameCount].awayName,
            game["teams"]["away"]["team"]["name"] | "Away Team",
            sizeof(upcomingGames[upcomingGameCount].awayName));
    strlcpy(upcomingGames[upcomingGameCount].homeName,
            game["teams"]["home"]["team"]["name"] | "Home Team",
            sizeof(upcomingGames[upcomingGameCount].homeName));
    strlcpy(upcomingGames[upcomingGameCount].gameDate,
            game["gameDate"] | "",
            sizeof(upcomingGames[upcomingGameCount].gameDate));
    listedTeamIds[upcomingGameCount] = selectedTeamId;
    upcomingGameCount++;
  }

  lastCarouselTime = millis();
  DBG_PRINTF("[SCHED] renderer received %u upcoming card(s)\n",
             (unsigned)upcomingGameCount);
  renderUpcomingCarouselPage();
}

void logWaitingDisplayState(const int preferredTeamIds[3]) {
  DBG_PRINTF(
    "[DISPLAY] state=WAITING | TFT=waiting panel | matrices: home=0 away=0 "
    "| count LEDs: balls=0 strikes=0 outs=0 | priorities=[%d,%d,%d]\n",
    preferredTeamIds[0], preferredTeamIds[1], preferredTeamIds[2]);
}

void rotateCarousel() {
  if (!hasCurrentLiveGame || !currentLinescore.valid) {
    if (upcomingGameCount == 0) return;

    if (newsStoryCount == 0) {
      if (millis() - lastCarouselTime < MLB_UPCOMING_GAMES_ROTATE_MS) return;
      upcomingGameIndex = (upcomingGameIndex + 1) % upcomingGameCount;
      upcomingSubPage = UpcomingSubPage::GAME_INFO;
      lastCarouselTime = millis();
      renderUpcomingGame();
      return;
    }

    if (upcomingSubPage == UpcomingSubPage::NEWS_STORY && newsStoryCount > 0) {
      // LED-marquee stepping: the offset advances one whole character
      // cell per MLB_NEWS_TICKER_STEP_MS, repainting only when the cell
      // changes. Continuous scrolling can never be pushed cleanly over
      // the bit-banged bus (speed x push time always shows as tearing);
      // a stepped sign is perfectly static between ticks.
      int offset = (int)((millis() - newsTickerStartedAt) /
                         MLB_NEWS_TICKER_STEP_MS) * NEWS_TICKER_CHAR_W;
      if (offset == newsTickerX) return;  // holding between steps
      newsTickerX = offset;

      // Done when the trailing edge of the text has crossed the window's
      // LEFT edge (the push clips at the window, so pixels left of it are
      // never shown — no need to scroll the text all the way to x=0).
      if (newsTickerX > NEWS_TICKER_WINDOW_R - NEWS_TICKER_WINDOW_X +
                            newsTickerTextPx() + NEWS_TICKER_CHAR_W) {
        upcomingSubPage = UpcomingSubPage::GAME_INFO;
        lastCarouselTime = millis();
        // Actually paint the game card here: without this, the GAME_INFO
        // dwell showed the frozen final ticker frame for its whole 5 s and
        // then moved on — the upcoming-game cards never appeared.
        renderUpcomingGame();
        return;
      }
      drawNewsTickerFrame(newsTickerX);
      return;
    }

    if (millis() - lastCarouselTime < MLB_UPCOMING_GAMES_ROTATE_MS) return;
    lastCarouselTime = millis();

    uint8_t nextSubPage = static_cast<uint8_t>(upcomingSubPage) + 1;
    if (nextSubPage >= static_cast<uint8_t>(UpcomingSubPage::COUNT)) {
      nextSubPage = 0;
      upcomingGameIndex = (upcomingGameIndex + 1) % upcomingGameCount;
    }
    upcomingSubPage = static_cast<UpcomingSubPage>(nextSubPage);
    if (upcomingSubPage == UpcomingSubPage::NEWS_STORY && newsStoryCount > 0) {
      newsStoryIndex = (newsStoryIndex + 1) % newsStoryCount;
      newsTickerStartedAt = millis();
      newsTickerX = 0;
    }
    renderUpcomingCarouselPage();
    return;
  }

  // Live game: rotate the On Deck / Around-the-League ticker slide.
  if (millis() - lastCarouselTime >= MLB_CAROUSEL_ROTATE_MS) {
    tickerSlide = (tickerSlide + 1) % tickerSlideCount();
    renderLinescore(currentLinescore);  // re-uses dirty-rect path
    lastCarouselTime = millis();
  }
}

void updateNewsStories(JsonObjectConst newsRoot) {
  newsStoryCount = 0;
  for (JsonObjectConst article : newsRoot["articles"].as<JsonArrayConst>()) {
    if (newsStoryCount >= MAX_NEWS_STORIES) break;
    NewsStory& story = newsStories[newsStoryCount];
    strlcpy(story.headline, article["headline"] | "", sizeof(story.headline));
    strlcpy(story.description, article["description"] | "", sizeof(story.description));
    if (story.headline[0] == '\0') continue; // Skip malformed entries
    newsStoryCount++;
  }
  if (newsStoryIndex >= newsStoryCount) {
    newsStoryIndex = 0;
  }
}

// Publisher accessors for the core-0 data task. These wrap the newsStories[]
// storage inside the anonymous namespace so the data task can refresh the
// news cache without naming internals.
void publishNewsStory(const NewsSlotUpdate& slot) {
  if (slot.index >= MAX_NEWS_STORIES) return;
  strlcpy(newsStories[slot.index].headline, slot.headline,
          sizeof(newsStories[slot.index].headline));
  strlcpy(newsStories[slot.index].description, slot.description,
          sizeof(newsStories[slot.index].description));
}
void setNewsStoryCount(size_t count) {
  newsStoryCount = (count > MAX_NEWS_STORIES) ? MAX_NEWS_STORIES : count;
  newsStoryIndex = 0;
}
size_t getNewsStoryCount()  { return newsStoryCount; }
size_t getNewsStoryIndex()  { return newsStoryIndex; }
void   advanceNewsStoryIndex() { newsStoryIndex = (newsStoryIndex + 1) % newsStoryCount; }
const NewsStory& getNewsStory(size_t index) { return newsStories[index]; }
