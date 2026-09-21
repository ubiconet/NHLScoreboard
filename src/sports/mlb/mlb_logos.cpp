#include "config.h"
#include "mlb_logos.h"

#include "team_logos.h"

namespace {
// 0x1909 marks transparent pixels in the generated logo bitmaps.
constexpr uint16_t TEAM_LOGO_TRANSPARENT = 0x1909;

// 64x64 RGB565 team-logo cache. Reading PROGMEM logos with pgm_read_word
// 4096 times per logo per repaint was the single biggest redraw cost on the
// live screen (each repaint drew 2 logos * 2 sized versions). After first
// decode we keep the pixels in RAM so subsequent draws are flat word loads.
// Two slots are enough for the live screen (home + away); upcoming-screen
// logos fall back to PROGMEM when their slot is occupied by a different team.
struct LogoCacheSlot {
  int  teamId;
  uint16_t pixels[64 * 64];
  bool valid;
};
LogoCacheSlot gLogoCache[2] = {};
}  // namespace

void ensureLogoCached(int teamId, const char* abbrev) {
  if (teamId == 0) return;
  for (int i = 0; i < 2; ++i) {
    if (gLogoCache[i].valid && gLogoCache[i].teamId == teamId) return;
  }
  // Pick the LRU slot (just whichever isn't currently being repainted).
  // For 2 slots, evict whichever is not the new id.
  int slot = (gLogoCache[0].valid && gLogoCache[0].teamId != teamId) ? 0 : 1;
  if (gLogoCache[slot].valid && gLogoCache[slot].teamId == teamId) return;
  // Decode from PROGMEM (or fall back to abbrev lookup if id not in switch).
  // getTeamLogo64 takes an abbreviation, so we only fill the cache when the
  // abbreviation is the canonical switch case. Otherwise leave the slot
  // invalid and drawTeamLogo64() will use its fallback badge path.
  if (abbrev == nullptr || abbrev[0] == '\0') return;
  const uint16_t* src = getTeamLogo64(abbrev);
  if (src == nullptr) return;
  gLogoCache[slot].valid  = true;
  gLogoCache[slot].teamId = teamId;
  for (size_t i = 0; i < 64 * 64; ++i) {
    gLogoCache[slot].pixels[i] = pgm_read_word(&src[i]);
  }
}

namespace {
const uint16_t* getCachedLogoPixels(int teamId) {
  for (int i = 0; i < 2; ++i) {
    if (gLogoCache[i].valid && gLogoCache[i].teamId == teamId) {
      return gLogoCache[i].pixels;
    }
  }
  return nullptr;
}
}  // namespace

void drawTeamLogo64(Adafruit_GFX& target, int x, int y, int teamId, const char* abbrev) {
  // RAM-cache hit avoids 4096 PROGMEM reads per logo per repaint.
  const uint16_t* cached = getCachedLogoPixels(teamId);
  if (cached != nullptr) {
    target.drawRGBBitmap(x, y, cached, 64, 64);
    return;
  }
  const uint16_t* logo = getTeamLogo64(abbrev);
  if (logo != nullptr) {
    // Fill cache for next time, then draw via the cached buffer.
    ensureLogoCached(teamId, abbrev);
    cached = getCachedLogoPixels(teamId);
    if (cached != nullptr) {
      target.drawRGBBitmap(x, y, cached, 64, 64);
      return;
    }
    for (int py = 0; py < 64; ++py) {
      for (int px = 0; px < 64; ++px) {
        uint16_t color = pgm_read_word(logo + (py * 64) + px);
        if (color == TEAM_LOGO_TRANSPARENT) continue;
        target.drawPixel(x + px, y + py, color);
      }
    }
  } else {
    // Fallback badge box if team logo array is missing
    target.fillRoundRect(x, y, 64, 64, 8, COLOR_CARD);
    target.drawRoundRect(x, y, 64, 64, 8, COLOR_GOLD);
    target.setTextColor(COLOR_LED_RED);
    target.setTextSize(2);
    int textLen = strlen(abbrev);
    int tx = x + 32 - (textLen * 6);
    target.setCursor(tx, y + 24);
    target.print(abbrev);
  }
}

void drawTeamLogoScaled(Adafruit_GFX& target, int x, int y, int teamId,
                        const char* abbrev, int size) {
  // Only the 64x64 unscaled form is cached; scaled draws (used on the
  // upcoming-game screen) still walk PROGMEM, but the upcoming screen is
  // much less frequent than the live one and only renders 2 logos total.
  const uint16_t* logo = getTeamLogo64(abbrev);
  if (logo == nullptr) {
    drawTeamLogo64(target, x, y, teamId, abbrev);
    return;
  }
  for (int outputY = 0; outputY < size; outputY++) {
    int sourceY = outputY * 64 / size;
    for (int outputX = 0; outputX < size; outputX++) {
      int sourceX = outputX * 64 / size;
      uint16_t color = pgm_read_word(logo + sourceY * 64 + sourceX);
      if (color == TEAM_LOGO_TRANSPARENT) continue;
      target.drawPixel(x + outputX, y + outputY, color);
    }
  }
}
