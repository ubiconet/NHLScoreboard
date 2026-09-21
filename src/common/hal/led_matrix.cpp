#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "common/data/time_util.h"
#include "led_matrix.h"

namespace {
// Pins, captured from initLedMatrix() — the driver never reads config itself.
int DIN_PIN = -1;
int CLK_PIN = -1;
int CS_PIN = -1;

// MAX7219 Register Addresses
const uint8_t MAX7219_REG_NOOP = 0x00;
const uint8_t MAX7219_REG_DIGIT0 = 0x01;
const uint8_t MAX7219_REG_DECODEMODE = 0x09;
const uint8_t MAX7219_REG_INTENSITY = 0x0A;
const uint8_t MAX7219_REG_SCANLIMIT = 0x0B;
const uint8_t MAX7219_REG_SHUTDOWN = 0x0C;
const uint8_t MAX7219_REG_DISPLAYTEST = 0x0F;

// 3x5 font digits for 2-digit scores (0-9)
const uint8_t FONT_3X5[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
  {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b111, 0b001, 0b111}  // 9
};

// 5x7 font digits for 1-digit centered scores (0-9)
const uint8_t FONT_5X7[10][7] = {
  {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 0
  {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
  {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
  {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
  {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
  {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
  {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
  {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
  {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
  {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}  // 9
};

// 5x7 font letters used only for the boot-time matrix test (H = home, A = away)
const uint8_t LETTER_H_5X7[7] = {
  0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};
const uint8_t LETTER_A_5X7[7] = {
  0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};

int lastClockMinute = -1;
bool clockShown = false;
const uint8_t MATRIX_GAME_INTENSITY = 0x05;
const uint8_t MATRIX_CLOCK_INTENSITY = 0x01;
uint8_t matrixIntensity = MATRIX_GAME_INTENSITY;

void max7219ShiftByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(CLK_PIN, LOW);
    digitalWrite(DIN_PIN, (data & (1 << i)) ? HIGH : LOW);
    digitalWrite(CLK_PIN, HIGH);
  }
}

// Send (reg, data) command pair to 2 cascaded MAX7219 devices: dev1 (Home) then dev0 (Away)
void max7219Send(uint8_t reg1, uint8_t data1, uint8_t reg0, uint8_t data0) {
  digitalWrite(CS_PIN, LOW);
  max7219ShiftByte(reg1);
  max7219ShiftByte(data1);
  max7219ShiftByte(reg0);
  max7219ShiftByte(data0);
  digitalWrite(CS_PIN, HIGH);
}

void max7219SendAll(uint8_t reg, uint8_t data) {
  max7219Send(reg, data, reg, data);
}

void setMatrixIntensity(uint8_t intensity) {
  if (matrixIntensity == intensity) return;
  max7219SendAll(MAX7219_REG_INTENSITY, intensity);
  matrixIntensity = intensity;
}

// Bit c of row r ("column c") lives at bit position (7-c) of rows[r].
uint8_t getMatrixBit(const uint8_t rows[8], int r, int c) {
  return (rows[r] >> (7 - c)) & 0x01;
}

void setMatrixBit(uint8_t rows[8], int r, int c, uint8_t value) {
  if (value) {
    rows[r] |= static_cast<uint8_t>(1 << (7 - c));
  } else {
    rows[r] &= static_cast<uint8_t>(~(1 << (7 - c)));
  }
}

// Physical mounting correction: home matrix is installed upside-down.
void rotateMatrix180(uint8_t rows[8]) {
  uint8_t result[8] = {0};
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      setMatrixBit(result, r, c, getMatrixBit(rows, 7 - r, 7 - c));
    }
  }
  memcpy(rows, result, 8);
}

// Physical mounting correction: away matrix is installed rotated 90 degrees clockwise.
void rotateMatrix90Ccw(uint8_t rows[8]) {
  uint8_t result[8] = {0};
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      setMatrixBit(result, r, c, getMatrixBit(rows, c, 7 - r));
    }
  }
  memcpy(rows, result, 8);
}

// Convert integer score (0-99) into 8 row bytes for an 8x8 matrix. A negative
// score (MAX7219_SCORE_BLANK) leaves the matrix dark for "no active game".
void scoreToMatrixRows(int score, uint8_t rows[8], bool compactSingleDigit = false,
                       bool leadingZero = true) {
  for (int i = 0; i < 8; i++) rows[i] = 0;
  if (score < 0) return;
  if (score > 99) score = 99;

  // Render a single digit with the larger, centered 5x7 font whenever we're
  // allowed to collapse it. Game mode (compactSingleDigit=false) always uses
  // this; clock mode uses it whenever leadingZero is off so e.g. hour "1".."9"
  // is centered rather than jammed into the right half of the matrix.
  bool useSingleDigit = score < 10 && (!compactSingleDigit || !leadingZero);
  if (useSingleDigit) {
    for (int r = 0; r < 7; r++) {
      rows[r] = (FONT_5X7[score][r] & 0x1F) << 1; // Center 5 bits in 8 cols
    }
  } else {
    // Two 3x5 digits side by side: tens at cols 7..5, ones at cols 3..1.
    int tens = score / 10;
    int ones = score % 10;
    for (int r = 0; r < 5; r++) {
      uint8_t tensBits = FONT_3X5[tens][r] & 0x07;
      uint8_t onesBits = FONT_3X5[ones][r] & 0x07;
      rows[r + 1] = (tensBits << 4) | onesBits;
    }
  }
}

void writeMatrixRows(uint8_t awayRows[8], uint8_t homeRows[8]) {
  rotateMatrix180(homeRows);
  rotateMatrix90Ccw(awayRows);
  for (uint8_t row = 0; row < 8; row++) {
    uint8_t reg = MAX7219_REG_DIGIT0 + row;
    max7219Send(reg, homeRows[row], reg, awayRows[row]);
  }
}
} // namespace

void initLedMatrix(int dinPin, int clkPin, int csPin) {
  DIN_PIN = dinPin;
  CLK_PIN = clkPin;
  CS_PIN = csPin;

  pinMode(DIN_PIN, OUTPUT);
  pinMode(CLK_PIN, OUTPUT);
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);

  // Initialize MAX7219 registers
  max7219SendAll(MAX7219_REG_SHUTDOWN, 0x01);    // Normal operation
  max7219SendAll(MAX7219_REG_DECODEMODE, 0x00);  // Raw matrix mode
  max7219SendAll(MAX7219_REG_SCANLIMIT, 0x07);   // Scan all 8 digits
  max7219SendAll(MAX7219_REG_INTENSITY, MATRIX_GAME_INTENSITY);
  max7219SendAll(MAX7219_REG_DISPLAYTEST, 0x00); // Test off

  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  Serial.println("[HW] MAX7219 matrix driver initialized");
}

void setMax7219Scores(int awayScore, int homeScore) {
  setMatrixIntensity(MATRIX_GAME_INTENSITY);
  uint8_t awayRows[8];
  uint8_t homeRows[8];

  scoreToMatrixRows(awayScore, awayRows);
  scoreToMatrixRows(homeScore, homeRows);
  writeMatrixRows(awayRows, homeRows);
}

void updateMax7219Clock(bool enabled) {
  if (!enabled) {
    if (clockShown) {
      setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
      clockShown = false;
    }
    return;
  }

  time_t now = time(nullptr);
  if (!timeIsSynced()) return; // Wait until NTP has established the local clock.

  tm localTime = {};
  localtime_r(&now, &localTime);
  if (localTime.tm_min == lastClockMinute) return;

  uint8_t awayRows[8];
  uint8_t homeRows[8];
  int hour = localTime.tm_hour % 12;
  if (hour == 0) hour = 12;
  // Both matrices use the same two 3x5 digit style (minutes with a leading
  // zero, e.g. "07", hours likewise) so the clock reads as one consistent
  // font. An earlier revision rendered single-digit hours in the large 5x7
  // glyph, which looked mismatched next to the minutes.
  scoreToMatrixRows(localTime.tm_min, awayRows, true, true);
  scoreToMatrixRows(hour, homeRows, true, true);
  setMatrixIntensity(MATRIX_CLOCK_INTENSITY);
  writeMatrixRows(awayRows, homeRows);
  lastClockMinute = localTime.tm_min;
  clockShown = true;
}

void invalidateMax7219Clock() {
  lastClockMinute = -1;
  // Treat active-game content as clock-owned so a disabled idle clock clears it.
  clockShown = true;
}

void runMax7219BootTest() {
  uint8_t homeRows[8] = {0};
  uint8_t awayRows[8] = {0};
  for (int row = 0; row < 7; row++) {
    homeRows[row] = (LETTER_H_5X7[row] & 0x1F) << 1; // Center 5 bits in 8 cols
    awayRows[row] = (LETTER_A_5X7[row] & 0x1F) << 1;
  }
  rotateMatrix180(homeRows);
  rotateMatrix90Ccw(awayRows);

  for (uint8_t row = 0; row < 8; row++) {
    uint8_t reg = MAX7219_REG_DIGIT0 + row;
    max7219Send(reg, homeRows[row], reg, awayRows[row]);
  }

  Serial.println("[HW TEST] MAX7219 boot test: H=home, A=away");
  delay(2000);
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
}
