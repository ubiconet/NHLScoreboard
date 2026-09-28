#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "nhl_snapshot.h"

// Renderer-internal shared state — defined in nhl_renderer_linescore.cpp,
// used by both renderer TUs (live + waiting) and nowhere else. Do NOT
// include from outside the renderer pair.

namespace nhl_render {

// Latest game snapshot from core 0 + whether a live game owns the screen
// (set false by renderWaiting, true by renderLiveGame).
extern GameSnapshot currentGame;
extern bool hasCurrentLiveGame;

// Carousel slide index + the shared dwell clock (advanced by
// rotateCarousel()).
extern uint8_t tickerSlide;
extern uint32_t lastCarouselTime;

// Around-the-league ticker data, copied out of the day-slate snapshot so
// it stays valid across re-fetches.
struct OtherGameInfo {
  char awayAbbrev[4];
  char homeAbbrev[4];
  int  awayScore;
  int  homeScore;
  char gameState[8];
};
const size_t MAX_OTHER_GAMES = 9;
extern OtherGameInfo otherGames[MAX_OTHER_GAMES];
extern size_t otherGameCount;

}  // namespace nhl_render
