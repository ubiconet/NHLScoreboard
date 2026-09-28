#include <Arduino.h>
#include <time.h>

#include "common/data/time_util.h"
#include "tm1637.h"

// TM1637 protocol (clock-low-write, DIO sampled on CLK rising edge):
//   write display data command 0x40 (auto-increment addresses)
//   address command 0xC0, then 4 segment bytes (left..right)
//   display control 0x88 | brightness (0x80 alone = display off)
// ACKs are ignored (a short delay after each byte instead) — every
// public-domain TM1637 driver does the same and it is reliable.

namespace {

int sClk = -1, sDio = -1;
uint8_t sBrightness = 3;
int sLastWallMinute = -1;

// 7-segment patterns 0-9 (bit7 = decimal point / colon half).
const uint8_t kDigits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                             0x6D, 0x7D, 0x07, 0x7F, 0x6F};
const int kColonDigit = 1;  // DP of the 2nd digit carries the colon

void delayHalf() { delayMicroseconds(5); }

void clkHigh() { digitalWrite(sClk, HIGH); delayHalf(); }
void clkLow()  { digitalWrite(sClk, LOW);  delayHalf(); }

void dioHigh() { digitalWrite(sDio, HIGH); delayHalf(); }
void dioLow()  { digitalWrite(sDio, LOW);  delayHalf(); }

void start() { dioLow(); clkLow(); }
void stop()  { clkHigh(); dioHigh(); }

bool writeByte(uint8_t b) {
  for (int i = 0; i < 8; ++i) {
    clkLow();
    digitalWrite(sDio, (b & 1) ? HIGH : LOW);
    delayHalf();
    clkHigh();
    b >>= 1;
  }
  // 9th clock = ACK slot; we release DIO and ignore the level.
  clkLow();
  pinMode(sDio, INPUT);
  delayHalf();
  clkHigh();
  delayHalf();
  pinMode(sDio, OUTPUT);
  clkLow();
  return true;
}

void writeFrame(const uint8_t digits[4]) {
  start(); writeByte(0x40); stop();
  start(); writeByte(0xC0);
  for (int i = 0; i < 4; ++i) writeByte(digits[i]);
  stop();
  start(); writeByte(0x88 | (sBrightness & 0x07)); stop();
}

}  // namespace

void initTm1637(int clkPin, int dioPin, uint8_t brightness) {
  sClk = clkPin;
  sDio = dioPin;
  sBrightness = brightness > 7 ? 7 : brightness;
  pinMode(sClk, OUTPUT);
  pinMode(sDio, OUTPUT);
  clkHigh();
  dioHigh();
  uint8_t blank[4] = {0, 0, 0, 0};
  writeFrame(blank);
}

void tm1637ShowPair(int hi, int lo, bool colonOn) {
  if (sClk < 0) return;
  uint8_t digits[4];
  digits[0] = kDigits[(hi / 10) % 10];
  digits[1] = kDigits[hi % 10];
  digits[2] = kDigits[(lo / 10) % 10];
  digits[3] = kDigits[lo % 10];
  if (colonOn) digits[kColonDigit] |= 0x80;
  writeFrame(digits);
}

void tm1637SetBrightness(uint8_t brightness) {
  sBrightness = brightness > 7 ? 7 : brightness;
  start(); writeByte(0x88 | (sBrightness & 0x07)); stop();
}

void invalidateTm1637WallClock() { sLastWallMinute = -1; }

void updateTm1637WallClock(bool enabled) {
  if (!enabled) {
    if (sLastWallMinute != -2) {  // -2 = currently blank
      uint8_t blank[4] = {0, 0, 0, 0};
      writeFrame(blank);
      sLastWallMinute = -2;
    }
    return;
  }
  time_t now = time(nullptr);
  if (!timeIsSynced()) return;  // no trustworthy wall time yet
  tm lt = {};
  localtime_r(&now, &lt);
  if (lt.tm_min == sLastWallMinute) return;
  sLastWallMinute = lt.tm_min;
  int hour = lt.tm_hour % 12;
  if (hour == 0) hour = 12;
  tm1637ShowPair(hour, lt.tm_min, true);
}
