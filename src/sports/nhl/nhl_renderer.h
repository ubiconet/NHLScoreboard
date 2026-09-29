#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "nhl_snapshot.h"

// NHL renderer API (split across nhl_renderer_linescore.cpp and
// nhl_renderer_waiting.cpp). Cross-core data plumbing (snapshot channels,
// followed-game id, schedule cache, fetch diagnostics) lives in nhl_state.h;
// boot/OTA screens live in common/ui.

// ---- News slots (storage owned by the renderer; the core-0 data task
// publishes fresh stories through these accessors) ----
struct NewsStory {
  char headline[120];
  char description[220];
};

// Constant shared with the renderer so the data task's cache allocation
// doesn't exceed the renderer slot count.
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

// ---- Renderers ----

// Renders the live-game screen (TFT + score matrices + penalty LEDs) from
// a fresh landing snapshot. Value-cached: only changed regions repaint.
void renderLiveGame(const GameSnapshot& game);

// Forces the next renderLiveGame() to repaint EVERYTHING (used after
// something else owned the display, e.g. the portal display test).
void forceLiveRepaint(const GameSnapshot& game);

// Advances the live game clock one second at a time between the 5 s
// landing polls while the real game clock is running (frozen when it is
// stopped); each poll re-syncs. Updates the TM1637 + TFT header clock
// only when the displayed second changes. No-op without a live game.
void tickLiveClock();

// Manual mode (portal "Manual Mode" page owns the values): TFT shows
// shots per team, matrices show scores + period, TM1637 the manual clock,
// penalty LEDs follow the manual mask. Value-cached like the live screen.
void renderManual();
// Drops the manual screen's cache so the next renderManual() repaints all.
void invalidateManualScreen();

// Renders the waiting state: upcoming-game cards — one per preferred team,
// away @ home with logos, abbrevs, date/time and a countdown — rotating
// with the league ticker and NHL news slides (rotated by
// rotateCarousel()); clears matrices/LEDs on entry.
void renderWaiting(JsonObjectConst dayScoreJson,
                   const int preferredTeamIds[3]);

// Rotates the waiting-mode carousel (and the live ticker) when its dwell
// time has elapsed.
void rotateCarousel();

// Apply the latest day-slate snapshot to the around-the-league ticker
// (excludes the followed game).
void updateOtherGames(const ScheduleSnapshot& schedule, long excludeGameId);
