#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "mlb_snapshot.h"

// MLB renderer API (split across mlb_renderer_linescore.cpp and
// mlb_renderer_waiting.cpp). Cross-core data plumbing (snapshot channels, active
// gamePk, schedule cache, fetch diagnostics) lives in mlb_state.h; boot/OTA
// screens live in common/ui.

// ---- News slots (storage owned by the renderer; the core-0 data task
// publishes fresh stories through these accessors) ----
struct NewsStory {
  char headline[120];
  char description[220];
};

// Constant shared with the renderer so the data task's cache allocation
// (gCachedNews) doesn't exceed the renderer slot count.
static const size_t MAX_NEWS_STORIES = 10;

struct NewsSlotUpdate {
  size_t index;
  char   headline[120];
  char   description[220];
};
void publishNewsStory(const NewsSlotUpdate& slot);
void setNewsStoryCount(size_t count);   // also resets the index to 0
size_t getNewsStoryCount();
size_t getNewsStoryIndex();
void   advanceNewsStoryIndex();
const NewsStory& getNewsStory(size_t index);

// Apply the latest news payload to the carousel's slide list (legacy JSON
// path; the live path uses the slot publishers above).
void updateNewsStories(JsonObjectConst newsRoot);

// ---- Renderers ----

// One-time splash screen shown at boot, before network/game state is known.
void renderBootSplash();

// Renders the full MLB linescore UI on the ST7789 TFT and updates hardware LEDs / MAX7219 matrices
void renderLinescore(const LinescoreSnapshot& linescore);

// Shows a completed plate-appearance result for a short time, then restores the live panel.
void showAtBatResult(const char* batterName, const char* event,
                     const char* description);
void updateAtBatResultDisplay();
void resetAtBatResultDisplay();
// True while the full-screen at-bat result card owns the display.
bool isAtBatResultVisible();

// Renders the waiting state and clears scores/count LEDs until a preferred team is live.
void renderWaiting(JsonObjectConst upcomingSchedule,
                   JsonObjectConst standings,
                   const int preferredTeamIds[3]);

// Writes the expected TFT, LED matrix, and discrete LED state to Serial.
void logLiveDisplayState(const LinescoreSnapshot& linescore, int gamePk);
void logWaitingDisplayState(const int preferredTeamIds[3]);

// Rotates the scrolling ticker in the lower TFT section (On Deck / league scores)
void rotateCarousel();

// Apply the latest snapshot of "other live games" to the ticker data used by
// drawBottomPanel. Excluded gamePk keeps the tracked game off the list.
void updateOtherGames(const ScheduleSnapshot& schedule, int excludeGamePk);
