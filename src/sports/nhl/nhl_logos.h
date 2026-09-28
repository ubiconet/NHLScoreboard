#pragma once

#include <Adafruit_GFX.h>

// Team-logo drawing with a small RAM cache. Logos are 64x64 RGB565 PROGMEM
// bitmaps (team_logos.h) with 0x1909 as the transparent sentinel.

// Decodes the team's logo into the RAM cache (no-op when already cached or
// the abbreviation has no logo). Called by the live renderer to pre-warm
// the cache before the first team-card draw.
void ensureLogoCached(int teamId, const char* abbrev);

// Draws the 64x64 logo at (x, y) — RAM-cache path when warm, PROGMEM walk
// otherwise, fallback badge when the team has no logo.
void drawTeamLogo64(Adafruit_GFX& target, int x, int y, int teamId, const char* abbrev);

// Draws the logo scaled to size x size (nearest-neighbor); used by the
// upcoming-game card. Only the unscaled 64x64 form is cached.
void drawTeamLogoScaled(Adafruit_GFX& target, int x, int y, int teamId,
                        const char* abbrev, int size);
