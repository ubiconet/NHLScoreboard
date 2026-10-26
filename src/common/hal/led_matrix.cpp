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
bool REVERSE_HOME_GUEST = false;
bool FLIP_ALL_MODULES = false;

// MAX7219 Register Addresses
const uint8_t MAX7219_REG_NOOP = 0x00;
const uint8_t MAX7219_REG_DIGIT0 = 0x01;
const uint8_t MAX7219_REG_DECODEMODE = 0x09;
const uint8_t MAX7219_REG_INTENSITY = 0x0A;
const uint8_t MAX7219_REG_SCANLIMIT = 0x0B;
const uint8_t MAX7219_REG_SHUTDOWN = 0x0C;
const uint8_t MAX7219_REG_DISPLAYTEST = 0x0F;

// 3x8 font digits for 2-digit scores (0-9): full matrix height, the
// largest two-digit form that fits an 8x8 (the small 3x5 pair it replaced
// left three rows dark).
const uint8_t FONT_3X8[10][8] = {
  {0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b001, 0b001, 0b001, 0b001, 0b001, 0b001}, // 7
  {0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001, 0b111}  // 9
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

// 3x8 letters for the shootout indicator on the period module ("SO")
const uint8_t FONT3X8_S[8] = {
    0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b001, 0b111};  // S
const uint8_t FONT3X8_O[8] = {
    0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111};  // O

// 5x7 font letters used only for the boot-time matrix test (H = home, A = away)
const uint8_t LETTER_H_5X7[7] = {
  0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};
const uint8_t LETTER_A_5X7[7] = {
  0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};

int lastClockMinute = -1;
bool clockShown = false;
const uint8_t MATRIX_CLOCK_INTENSITY = 0x01;

// Per-module display intensity (send order: pos3, pos2, pos1 = home,
// period, guest) — all three identical. History: full 15 -> 11 -> 8 for
// comfortable viewing, then halved to 4.
const uint8_t kModuleIntensity[3] = {0x04, 0x04, 0x04};

void max7219ShiftByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(CLK_PIN, LOW);
    digitalWrite(DIN_PIN, (data & (1 << i)) ? HIGH : LOW);
    digitalWrite(CLK_PIN, HIGH);
  }
}

// Send three (reg, data) command pairs down the 3-device cascade in one
// CS burst. Data shifts through module 1 (guest, DIN from the MCU) into
// module 2 (period) and module 3 (home): the FIRST pair shifted travels
// furthest and lands in module 3 (home), so the argument order is home,
// period, guest.
void max7219Send3(uint8_t regH, uint8_t dataH, uint8_t regP, uint8_t dataP,
                  uint8_t regG, uint8_t dataG) {
  digitalWrite(CS_PIN, LOW);
  max7219ShiftByte(regH); max7219ShiftByte(dataH);
  max7219ShiftByte(regP); max7219ShiftByte(dataP);
  max7219ShiftByte(regG); max7219ShiftByte(dataG);
  digitalWrite(CS_PIN, HIGH);
}

void max7219SendAll(uint8_t reg, uint8_t data) {
  max7219Send3(reg, data, reg, data, reg, data);
}

// Sends one 16-bit (reg, data) command to a single chain position; the
// other two slots get NOOP. Position 1 = guest (nearest the MCU's DIN),
// 2 = period, 3 = home.
void sendPosCmd(int pos, uint16_t cmd) {
  uint16_t p3 = pos == 3 ? cmd : 0x0000;  // home slot (first shifted)
  uint16_t p2 = pos == 2 ? cmd : 0x0000;  // period slot
  uint16_t p1 = pos == 1 ? cmd : 0x0000;  // guest slot (last shifted)
  digitalWrite(CS_PIN, LOW);
  max7219ShiftByte(p3 >> 8); max7219ShiftByte(p3 & 0xFF);
  max7219ShiftByte(p2 >> 8); max7219ShiftByte(p2 & 0xFF);
  max7219ShiftByte(p1 >> 8); max7219ShiftByte(p1 & 0xFF);
  digitalWrite(CS_PIN, HIGH);
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

// Physical mounting correction: chain position 2 (the period module in
// this enclosure) is installed rotated 90° clockwise, so its content is
// rotated 90° counter-clockwise.
void rotateMatrix90Ccw(uint8_t rows[8]) {
  uint8_t result[8] = {0};
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      setMatrixBit(result, r, c, getMatrixBit(rows, c, 7 - r));
    }
  }
  memcpy(rows, result, 8);
}

void rotateMatrix180(uint8_t rows[8]) {
  uint8_t result[8] = {0};
  for (int r = 0; r < 8; r++) {
    uint8_t reversed = 0;
    for (int c = 0; c < 8; c++) {
      reversed |= ((rows[r] >> c) & 0x01) << (7 - c);
    }
    result[7 - r] = reversed;
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
    // Two 3x8 digits side by side: tens at cols 7..5, ones at cols 3..1,
    // spanning all 8 rows.
    int tens = score / 10;
    int ones = score % 10;
    for (int r = 0; r < 8; ++r) {
      rows[r] = static_cast<uint8_t>(((FONT_3X8[tens][r] & 0x07) << 4) |
                                     (FONT_3X8[ones][r] & 0x07));
    }
  }
}

// Chain order (MCU DIN -> out): 1 = guest, 2 = period, 3 = home. The
// period module (position 2) carries the mounting rotation above.
// Intensity is NOT set here — callers choose per-module (game) or uniform
// (idle clock) brightness.
void writeMatrixRows(uint8_t awayRows[8], uint8_t homeRows[8],
                     uint8_t periodRows[8]) {
  rotateMatrix90Ccw(periodRows);
  if (FLIP_ALL_MODULES) {
    rotateMatrix180(awayRows);
    rotateMatrix180(homeRows);
    rotateMatrix180(periodRows);
  }
  const uint8_t* homeDisplayRows =
      REVERSE_HOME_GUEST ? awayRows : homeRows;
  const uint8_t* awayDisplayRows =
      REVERSE_HOME_GUEST ? homeRows : awayRows;
  for (uint8_t row = 0; row < 8; row++) {
    uint8_t reg = MAX7219_REG_DIGIT0 + row;
    max7219Send3(reg, homeDisplayRows[row], reg, periodRows[row], reg,
                 awayDisplayRows[row]);
  }
}

void blankRows(uint8_t rows[8]) {
  for (int i = 0; i < 8; i++) rows[i] = 0;
}
} // namespace

void initLedMatrixBus(int dinPin, int clkPin, int csPin) {
  DIN_PIN = dinPin;
  CLK_PIN = clkPin;
  CS_PIN = csPin;
  // Idle levels end the floating window: CLK low (no clock edges), DIN
  // low (zeros even if something ever clocks), CS high (latch closed —
  // only a RISING CS commits shift-register contents into the chips).
  // No register writes here: the ESP32 can be executing before the 5V
  // boost rail reaches the MAX7219s' operating range, and writes in
  // that window land randomly (v3.81's too-early init caused exactly
  // that — random garble instead of the occasional power-on flash).
  pinMode(DIN_PIN, OUTPUT);
  pinMode(CLK_PIN, OUTPUT);
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(DIN_PIN, LOW);
  digitalWrite(CLK_PIN, LOW);
  digitalWrite(CS_PIN, HIGH);
}

void initLedMatrix(int dinPin, int clkPin, int csPin, bool reverseHomeGuest,
                   bool flipAllModules) {
  DIN_PIN = dinPin;
  CLK_PIN = clkPin;
  CS_PIN = csPin;
  REVERSE_HOME_GUEST = reverseHomeGuest;
  FLIP_ALL_MODULES = flipAllModules;

  // The bus pins were grabbed by initLedMatrixBus() at the very top of
  // boot, but noise from BEFORE that grab may have left garbage in the
  // chain's shift registers. Flush it out: with CS held low, clock three
  // NOOP words through, so the first CS rising edge (below) latches
  // harmless NOOPs into all three modules instead of that garbage.
  digitalWrite(DIN_PIN, LOW);
  digitalWrite(CLK_PIN, LOW);
  digitalWrite(CS_PIN, LOW);
  for (int i = 0; i < 6; ++i) max7219ShiftByte(0x00);  // 3x 0x0000 = NOOP
  digitalWrite(CS_PIN, HIGH);  // latches harmless NOOPs into all 3 modules

  // Dark FIRST. Noise latched before firmware ran may have left the
  // shutdown register at "normal" with garbage digits (or display-test
  // mode on, which lights every LED at full brightness and overrides
  // shutdown). The shutdown write parks the segments dark within one
  // command; everything else is then configured while dark, and normal
  // operation resumes only on a clean, fully blanked chain. (The old
  // order wrote shutdown=normal FIRST and blanked digits LAST, so any
  // garbage displayed until the end of init.)
  max7219SendAll(MAX7219_REG_SHUTDOWN, 0x00);    // dark NOW, stays dark
  max7219SendAll(MAX7219_REG_DISPLAYTEST, 0x00); // kill any noise-set test mode
  max7219SendAll(MAX7219_REG_DECODEMODE, 0x00);  // Raw matrix mode
  max7219SendAll(MAX7219_REG_SCANLIMIT, 0x07);   // Scan all 8 digits
  max7219Send3(MAX7219_REG_INTENSITY, kModuleIntensity[0],
               MAX7219_REG_INTENSITY, kModuleIntensity[2],
               MAX7219_REG_INTENSITY, kModuleIntensity[1]);
  for (uint8_t r = 0; r < 8; ++r)                // blank every digit register
    max7219SendAll(MAX7219_REG_DIGIT0 + r, 0x00);
  max7219SendAll(MAX7219_REG_SHUTDOWN, 0x01);    // resume — blank and quiet

  Serial.println("[HW] MAX7219 matrix driver initialized");
}

void setMax7219Power(bool on) {
  // Shutdown register: 0x00 parks every module (segments off, registers
  // retained), 0x01 resumes. Used by hibernate to make the chain dark.
  max7219SendAll(MAX7219_REG_SHUTDOWN, on ? 0x01 : 0x00);
}

void setMax7219Display(int homeScore, int awayScore, int periodValue) {
  uint8_t awayRows[8];
  uint8_t homeRows[8];
  uint8_t periodRows[8];

  scoreToMatrixRows(awayScore, awayRows);
  scoreToMatrixRows(homeScore, homeRows);
  scoreToMatrixRows(periodValue, periodRows);
  writeMatrixRows(awayRows, homeRows, periodRows);
  // Slots: home (pos 3, first shifted), period (pos 2), guest (pos 1).
  max7219Send3(MAX7219_REG_INTENSITY, kModuleIntensity[0],
               MAX7219_REG_INTENSITY, kModuleIntensity[2],
               MAX7219_REG_INTENSITY, kModuleIntensity[1]);
}

void setMax7219ScoreFlash(int homeScore, int awayScore, int periodValue,
                          bool invertHome, bool invertGuest) {
  uint8_t awayRows[8];
  uint8_t homeRows[8];
  uint8_t periodRows[8];

  scoreToMatrixRows(awayScore, awayRows);
  scoreToMatrixRows(homeScore, homeRows);
  scoreToMatrixRows(periodValue, periodRows);
  // Reverse-video: swap lit and dark LEDs on the flagged sides. XOR
  // commutes with rotateMatrix90Ccw/rotateMatrix180, so writeMatrixRows'
  // mounting corrections still land right-side up.
  if (invertHome)
    for (int r = 0; r < 8; ++r) homeRows[r] ^= 0xFF;
  if (invertGuest)
    for (int r = 0; r < 8; ++r) awayRows[r] ^= 0xFF;
  writeMatrixRows(awayRows, homeRows, periodRows);
  max7219Send3(MAX7219_REG_INTENSITY, kModuleIntensity[0],
               MAX7219_REG_INTENSITY, kModuleIntensity[2],
               MAX7219_REG_INTENSITY, kModuleIntensity[1]);
}

void setMax7219Scores(int awayScore, int homeScore) {
  // Period module stays dark in normal mode until the renderer drives it.
  setMax7219Display(homeScore, awayScore, MAX7219_SCORE_BLANK);
}

void setMax7219Shootout(int homeScore, int awayScore, bool invertHome,
                        bool invertGuest) {
  // Scores on the outer modules, "SO" on the period module (two 3x5
  // letters in the same slot layout as a two-digit period number).
  uint8_t awayRows[8], homeRows[8], soRows[8] = {0};
  scoreToMatrixRows(awayScore, awayRows);
  scoreToMatrixRows(homeScore, homeRows);
  for (int r = 0; r < 8; ++r) {
    soRows[r] = ((FONT3X8_S[r] & 0x07) << 4) | (FONT3X8_O[r] & 0x07);
  }
  if (invertHome)
    for (int r = 0; r < 8; ++r) homeRows[r] ^= 0xFF;
  if (invertGuest)
    for (int r = 0; r < 8; ++r) awayRows[r] ^= 0xFF;
  writeMatrixRows(awayRows, homeRows, soRows);
  max7219Send3(MAX7219_REG_INTENSITY, kModuleIntensity[0],
               MAX7219_REG_INTENSITY, kModuleIntensity[2],
               MAX7219_REG_INTENSITY, kModuleIntensity[1]);
}

void setMax7219Frames(const uint8_t awayRows[8], const uint8_t homeRows[8],
                      const uint8_t periodRows[8]) {
  // writeMatrixRows applies the mounting corrections in place, so work on
  // copies — callers may pass literals/shared buffers.
  uint8_t away[8], home[8], period[8];
  memcpy(away, awayRows, 8);
  memcpy(home, homeRows, 8);
  memcpy(period, periodRows, 8);
  writeMatrixRows(away, home, period);
  max7219Send3(MAX7219_REG_INTENSITY, kModuleIntensity[0],
               MAX7219_REG_INTENSITY, kModuleIntensity[2],
               MAX7219_REG_INTENSITY, kModuleIntensity[1]);
}

void setMax7219PositionTest(int position, bool on) {
  uint16_t cmd = (uint16_t(MAX7219_REG_DISPLAYTEST) << 8) | (on ? 0x01 : 0x00);
  sendPosCmd(position, cmd);
}

void setMax7219DisplayTestAll(bool on) {
  max7219SendAll(MAX7219_REG_DISPLAYTEST, on ? 0x01 : 0x00);
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
  uint8_t periodRows[8];
  int hour = localTime.tm_hour % 12;
  if (hour == 0) hour = 12;
  // Both matrices use the same two 3x5 digit style (minutes with a leading
  // zero, e.g. "07", hours likewise) so the clock reads as one consistent
  // font. An earlier revision rendered single-digit hours in the large 5x7
  // glyph, which looked mismatched next to the minutes.
  scoreToMatrixRows(localTime.tm_min, awayRows, true, true);
  scoreToMatrixRows(hour, homeRows, true, true);
  blankRows(periodRows);
  writeMatrixRows(awayRows, homeRows, periodRows);
  max7219SendAll(MAX7219_REG_INTENSITY, MATRIX_CLOCK_INTENSITY);
  lastClockMinute = localTime.tm_min;
  clockShown = true;
}

void invalidateMax7219Clock() {
  lastClockMinute = -1;
  // Treat active-game content as clock-owned so a disabled idle clock clears it.
  clockShown = true;
}

void runMax7219ChainDiagnostic() {
  const uint16_t NOOP = 0x0000;
  const uint16_t TEST_ON = (uint16_t(MAX7219_REG_DISPLAYTEST) << 8) | 0x01;
  const uint16_t TEST_OFF = (uint16_t(MAX7219_REG_DISPLAYTEST) << 8) | 0x00;
  const char* role[3] = {"1 = GUEST (first after MCU DIN)", "2 = PERIOD",
                         "3 = HOME"};
  // send3 argument order is (pos3, pos2, pos1): first pair shifted lands in
  // the LAST module of the chain.
  auto sendPos = [&](int pos, uint16_t cmd) {
    uint16_t p3 = pos == 3 ? cmd : NOOP;
    uint16_t p2 = pos == 2 ? cmd : NOOP;
    uint16_t p1 = pos == 1 ? cmd : NOOP;
    digitalWrite(CS_PIN, LOW);
    max7219ShiftByte(p3 >> 8); max7219ShiftByte(p3 & 0xFF);
    max7219ShiftByte(p2 >> 8); max7219ShiftByte(p2 & 0xFF);
    max7219ShiftByte(p1 >> 8); max7219ShiftByte(p1 & 0xFF);
    digitalWrite(CS_PIN, HIGH);
  };

  Serial.println("[MTX] === chain diagnostic start ===");
  Serial.println("[MTX] clearing: shutdown->normal, display-test off, blank");
  max7219SendAll(MAX7219_REG_SHUTDOWN, 0x01);
  max7219SendAll(MAX7219_REG_DECODEMODE, 0x00);
  max7219SendAll(MAX7219_REG_SCANLIMIT, 0x07);
  max7219SendAll(MAX7219_REG_DISPLAYTEST, 0x00);
  max7219SendAll(MAX7219_REG_INTENSITY, 0x05);
  for (uint8_t r = 0; r < 8; ++r)
    max7219SendAll(MAX7219_REG_DIGIT0 + r, 0x00);
  delay(2500);

  Serial.println("[MTX] STEP 1: all-on test, ONE chain position at a time.");
  Serial.println("[MTX] Each step: exactly ONE module fully lit for 3 s.");
  for (int pos = 1; pos <= 3; ++pos) {
    Serial.printf("[MTX] ALL-ON -> chain position %s ... watch now\n",
                  role[pos - 1]);
    sendPos(pos, TEST_ON);
    delay(3000);
    sendPos(pos, TEST_OFF);
    delay(500);
  }

  Serial.println("[MTX] STEP 2: position digits — '1' on pos1, '2' on pos2,"
                 " '3' on pos3, one at a time.");
  for (int pos = 1; pos <= 3; ++pos) {
    Serial.printf("[MTX] digit '%d' -> chain position %s ... watch now\n",
                  pos, role[pos - 1]);
    uint8_t rows[3][8];
    for (int m = 0; m < 3; ++m) {
      blankRows(rows[m]);
      if (m == pos - 1) scoreToMatrixRows(pos, rows[m]);
    }
    rotateMatrix90Ccw(rows[1]);  // period module mounting correction
    if (FLIP_ALL_MODULES) {
      for (int m = 0; m < 3; ++m) rotateMatrix180(rows[m]);
    }
    for (uint8_t r = 0; r < 8; ++r) {
      max7219Send3(MAX7219_REG_DIGIT0 + r, rows[2][r],
                   MAX7219_REG_DIGIT0 + r, rows[1][r],
                   MAX7219_REG_DIGIT0 + r, rows[0][r]);
    }
    delay(3000);
  }

  Serial.println("[MTX] STEP 3: steady pattern HOME=1 GUEST=3 PERIOD=2.");
  Serial.println("[MTX] Interpretation:");
  Serial.println("[MTX]  - a module lit during NO step => its DIN or CS is"
                 " not on the bus (check jumpers).");
  Serial.println("[MTX]  - a module lit during SEVERAL steps => its DOUT"
                 " feeds nothing / its CS floats (reseat CS + DOUT->DIN).");
  Serial.println("[MTX]  - positions answered in the wrong physical order =>"
                 " swap the DIN feed or remap roles in software.");
  setMax7219Display(1, 3, 2);
  Serial.println("[MTX] === chain diagnostic done ===");
}

void runMax7219BootTest() {
  uint8_t homeRows[8] = {0};
  uint8_t awayRows[8] = {0};
  uint8_t periodRows[8] = {0};
  for (int row = 0; row < 7; row++) {
    homeRows[row] = (LETTER_H_5X7[row] & 0x1F) << 1; // Center 5 bits in 8 cols
    awayRows[row] = (LETTER_A_5X7[row] & 0x1F) << 1;
  }

  writeMatrixRows(awayRows, homeRows, periodRows);

  Serial.println("[HW TEST] MAX7219 boot test: H=home, A=away");
  delay(2000);
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
}
