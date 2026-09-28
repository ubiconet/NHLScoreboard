#pragma once

#include <Arduino.h>

// TM1637 4-digit 7-segment driver (bit-banged 2-wire: CLK + DIO).
// Drives the period/game clock during live games and the wall clock in
// waiting mode. Brightness 0..7. The colon bit lives on the DP of digit 1
// (second from left) on the common 4-digit clock modules — one constant
// here if a module variant differs.

void initTm1637(int clkPin, int dioPin, uint8_t brightness);

// Shows two 2-digit numbers as "hi lo" (each 0..99). The colon is lit by
// default and in every caller — the board convention is that the clock
// colon never goes out, whatever is being displayed.
// Game clock: (mm, ss). Wall clock: (hh, mm).
void tm1637ShowPair(int hi, int lo, bool colonOn = true);

// Brightness 0..7, or 0 to blank the display.
void tm1637SetBrightness(uint8_t brightness);

// Mirror of updateMax7219Clock(): 12-hour wall clock, redrawn once per
// minute, gated on NTP; clears the display when disabled.
void updateTm1637WallClock(bool enabled);

// Forces the next updateTm1637WallClock(true) to repaint (mode changes).
void invalidateTm1637WallClock();
