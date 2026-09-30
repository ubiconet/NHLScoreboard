#pragma once

#include <Arduino.h>

// Triple cascaded MAX7219 8x8 LED matrix driver (bit-banged — no SPI
// peripheral). Chain order from the MCU's DIN: module 1 = Guest(away)
// score, module 2 = Period, module 3 = Home score. Pins are passed to
// initLedMatrix() by the sport/app layer (from its sport_config.h).

// Pass this instead of a real score to leave a matrix dark (no game active).
static const int MAX7219_SCORE_BLANK = -1;

void initLedMatrix(int dinPin, int clkPin, int csPin);
void setMax7219Scores(int awayScore, int homeScore);
// Full 3-module chain write: home / guest(away) / period in one 24-bit
// burst (MAX7219_SCORE_BLANK leaves that module dark).
void setMax7219Display(int homeScore, int awayScore, int periodValue);
// Scores on the outer modules with "SO" on the period module (shootout).
void setMax7219Shootout(int homeScore, int awayScore);
// Display-test register helpers: all LEDs on one chain position (1 = home,
// 2 = guest, 3 = period) or on all three at once, bypassing digit data.
void setMax7219PositionTest(int position, bool on);
void setMax7219DisplayTestAll(bool on);
// Shows or clears local time: hour on the home matrix, minutes on the away
// matrix. Refreshes once per minute; waits for NTP before first display.
void updateMax7219Clock(bool enabled);
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
