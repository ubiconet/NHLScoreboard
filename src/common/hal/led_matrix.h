#pragma once

#include <Arduino.h>

// Triple cascaded MAX7219 8x8 LED matrix driver (bit-banged — no SPI
// peripheral). Chain order from the MCU's DIN: module 1 = Guest(away)
// score, module 2 = Period, module 3 = Home score. Pins are passed to
// initLedMatrix() by the sport/app layer (from its sport_config.h).

// Pass this instead of a real score to leave a matrix dark (no game active).
static const int MAX7219_SCORE_BLANK = -1;

// Early bus pin grab, called as the first hardware action of boot:
// drives DIN/CLK low and CS high, ending the window where floating
// lines let power-ramp noise clock garbage into the chain. Does NOT
// touch the MAX7219 registers — the 5V boost rail may still be ramping
// (the ESP32 executes before the MAX7219s are in their operating
// range), so register writes wait for initLedMatrix() at its usual
// later spot, after the display bring-up has given the rail time to
// settle.
void initLedMatrixBus(int dinPin, int clkPin, int csPin);

// Full driver bring-up: flushes the shift registers, parks the chain in
// shutdown, configures and blanks it while dark, then resumes normal
// operation. Call AFTER the power rail has settled (see
// initLedMatrixBus).
void initLedMatrix(int dinPin, int clkPin, int csPin,
                   bool reverseHomeGuest = false, bool flipAllModules = false);
void setMax7219Scores(int awayScore, int homeScore);
// Full 3-module chain write: home / guest(away) / period in one 24-bit
// burst (MAX7219_SCORE_BLANK leaves that module dark).
void setMax7219Display(int homeScore, int awayScore, int periodValue);
// Celebration variant of setMax7219Display: either score frame may be
// shown INVERTED (reverse video — every LED lit where the normal glyph
// is dark, and vice versa). Complementing the rows commutes with the
// mounting corrections, so orientation is unaffected.
void setMax7219ScoreFlash(int homeScore, int awayScore, int periodValue,
                          bool invertHome, bool invertGuest);
// Scores on the outer modules with "SO" on the period module (shootout).
// Either score frame may be shown INVERTED, like setMax7219ScoreFlash.
void setMax7219Shootout(int homeScore, int awayScore,
                        bool invertHome = false, bool invertGuest = false);
// Raw 8x8 frames for all three modules at once (one byte per row, bit 7 =
// leftmost column), with the usual chain-order + mounting corrections
// applied, at game intensity. Used by non-score effects (trivia fireworks).
void setMax7219Frames(const uint8_t awayRows[8], const uint8_t homeRows[8],
                      const uint8_t periodRows[8]);
// Display-test register helpers: all LEDs on one chain position (1 = home,
// 2 = guest, 3 = period) or on all three at once, bypassing digit data.
void setMax7219PositionTest(int position, bool on);
void setMax7219DisplayTestAll(bool on);
// Shows or clears local time: hour on the home matrix, minutes on the away
// matrix. Refreshes once per minute; waits for NTP before first display.
void updateMax7219Clock(bool enabled);
// Puts the whole chain into the MAX7219 shutdown state (all segments dark,
// registers retained) or back to normal operation — used by hibernate.
void setMax7219Power(bool on);
// Forces the next updateMax7219Clock(true) call to repaint even when the
// minute hasn't changed (e.g. after leaving a live game).
void invalidateMax7219Clock();

// TEST ONLY: shows "H" on the home matrix and "A" on the away matrix at boot.
void runMax7219BootTest();

// TEST ONLY: narrated chain diagnostic — clears all modules, drives the
// all-on DISPLAYTEST register on ONE chain position at a time, then a
// per-position digit, reporting each step over Serial. Blocking (~25 s);
// used by the bench hardware test mode to isolate chain wiring faults.
void runMax7219ChainDiagnostic();
