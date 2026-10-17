#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/gfx.h"
#include "common/ui/qr.h"
#include "trivia_engine.h"

// Trivia on-device screen — the "game show stage" rendered on the render
// core (sport::tick delegates here while trivia mode is active; see
// trivia_engine.h). Layout on the 320x240 panel:
//
//   header strip   TRIVIA | Q3/15 | score
//   timer bar      10 s answer window, gold -> red under 3 s
//   question       up to 4 wrapped lines, size-2 text
//   answers        four cards A) B) C) D) — gold when locked, green/red at
//                  the reveal
//   footer         penalty dots | +points | record
//
// Value-cached like the sport screens: a full repaint only on phase
// changes, card-row pushes on lock/reveal, and a 10 Hz timer-band push
// while a question runs. The shared canvas + band/row pushes keep the
// bit-banged bus busy only when something actually changed.

namespace {

constexpr int kHeaderH = 18;
constexpr int kBarY = 20;
constexpr int kBarH = 8;
constexpr int kBarX = 8;
constexpr int kBarW = 304;
constexpr int kQY = 34;
constexpr int kCardX = 8;
constexpr int kCardW = 304;
constexpr int kCardH = 24;
constexpr int kCardY[4] = {104, 130, 156, 182};
constexpr int kFooterY = 208;

const uint16_t kGreen = 0x07E0;
const uint16_t kRed = 0xF800;
const uint16_t kBarBg = 0x1082;  // dark slate for the unelapsed track

struct Drawn {
  uint8_t phase = 255;
  uint8_t qIndex = 255;
  uint8_t locked = 255;
  uint8_t correct = 255;
  uint8_t strikes = 255;
  uint16_t score = 9999;
  uint16_t lastPoints = 9999;
  uint16_t high = 9999;
  char highName[16] = {0};
  int barTenth = -1;
  int tmSec = -1;
  int mHome = -2, mAway = -2, mPer = -2;
  int leds = -1;
} d;

uint16_t cardFill(const TriviaView& v, int i) {
  if (v.phase == TRIVIA_REVEAL) {
    if (i == v.correctIndex) return kGreen;   // the actual answer
    if (i == v.lockedChoice) return kRed;     // the player's miss
    return COLOR_CARD;
  }
  if (v.phase == TRIVIA_QUESTION && v.lockedChoice == i) {
    return COLOR_GOLD;  // locked in — "final answer" highlight
  }
  return COLOR_CARD;
}

void drawCard(const TriviaView& v, int i, bool push) {
  GFXcanvas16& c = tftPanel.canvas();
  uint16_t fill = cardFill(v, i);
  bool invert = fill != COLOR_CARD;
  c.fillRoundRect(kCardX, kCardY[i], kCardW, kCardH, 5, fill);
  c.setTextSize(2);
  char letter[2] = {(char)('A' + i), '\0'};
  c.setTextColor(invert ? 0x0000 : COLOR_GOLD);
  c.setCursor(kCardX + 10, kCardY[i] + 4);
  c.print(letter);
  c.setTextColor(invert ? 0x0000 : ST77XX_WHITE);
  c.setCursor(kCardX + 34, kCardY[i] + 4);
  printClipped(c, v.question->answers[i], 22);
  if (push) tftPanel.pushRows(kCardX, kCardY[i], kCardW, kCardH);
}

void drawHeader(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillRect(0, 0, 320, kHeaderH, COLOR_CARD);
  c.setTextSize(2);
  c.setTextColor(COLOR_GOLD);
  c.setCursor(8, 1);
  c.print("TRIVIA");
  bool inRound = v.phase == TRIVIA_QUESTION || v.phase == TRIVIA_REVEAL;
  if (inRound) {
    char q[10];
    snprintf(q, sizeof(q), "Q%u/%u", v.questionIndex + 1,
             (unsigned)TRIVIA_ROUND_QUESTIONS);
    c.setTextColor(ST77XX_WHITE);
    drawCenteredText(c, q, 160, 1);
    char sc[8];
    snprintf(sc, sizeof(sc), "%u", v.score);
    c.setTextColor(COLOR_GOLD);
    c.setCursor(312 - (int)strlen(sc) * 12, 1);
    c.print(sc);
  } else if (v.highScore > 0) {
    char best[16];
    snprintf(best, sizeof(best), "BEST %u", v.highScore);
    c.setTextColor(COLOR_MUTED);
    c.setCursor(312 - (int)strlen(best) * 12, 1);
    c.print(best);
  }
}

void drawBar(int tenth) {  // tenth: 0..100 (tenths of a second left)
  GFXcanvas16& c = tftPanel.canvas();
  c.fillRect(kBarX, kBarY, kBarW, kBarH, kBarBg);
  int w = kBarW * tenth / 100;
  if (w > 0) {
    c.fillRect(kBarX, kBarY, w, kBarH, tenth <= 30 ? kRed : COLOR_GOLD);
  }
  tftPanel.pushBand(kBarY, kBarH);
}

void drawFooter(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillRect(0, kFooterY, 320, 240 - kFooterY, COLOR_BG);
  // Penalty dots: one lit per wrong answer, three ends the run (mirrors
  // the physical penalty LEDs).
  c.setTextSize(1);
  c.setTextColor(COLOR_MUTED);
  c.setCursor(8, kFooterY + 2);
  c.print("PENALTIES");
  for (int i = 0; i < 3; ++i) {
    int cx = 16 + i * 24, cy = kFooterY + 19;
    if (i < v.strikes) {
      c.fillCircle(cx, cy, 6, kRed);
    } else {
      c.drawCircle(cx, cy, 6, COLOR_MUTED);
    }
  }
  // Center: the reveal verdict (+points or the miss).
  if (v.phase == TRIVIA_REVEAL) {
    char line[16];
    if (v.lastCorrect) {
      snprintf(line, sizeof(line), "+%u PTS", v.lastPoints);
    } else {
      strlcpy(line, v.lockedChoice == TRIVIA_NO_LOCK ? "TIME UP" : "WRONG",
              sizeof(line));
    }
    c.setTextSize(2);
    c.setTextColor(v.lastCorrect ? kGreen : kRed);
    drawCenteredText(c, line, 160, kFooterY + 10);
  }
  // Right: the record to beat.
  if (v.highScore > 0) {
    char best[24];
    snprintf(best, sizeof(best), "BEST %u %s", v.highScore, v.highName);
    c.setTextSize(1);
    c.setTextColor(COLOR_MUTED);
    c.setCursor(312 - (int)strlen(best) * 6, kFooterY + 2);
    c.print(best);
  }
  tftPanel.pushBand(kFooterY, 240 - kFooterY);
}

void drawQuestionFrame(const TriviaView& v, int barTenth) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  c.setTextSize(2);
  c.setTextColor(ST77XX_WHITE);
  drawWrappedText(c, v.question->question, 8, kQY, 304, 12, 17, 4);
  for (int i = 0; i < 4; ++i) drawCard(v, i, false);
  drawFooter(v);
  tftPanel.pushFull();
  drawBar(barTenth);  // static full frame first, then the fresh band on top
}

void drawLobby(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(3);
  drawCenteredText(c, "TRIVIA MODE", 160, 40);
  c.setTextSize(2);
  c.setTextColor(COLOR_MUTED);
  drawWrappedText(c, "Open the controller on your phone and press Solo "
                     "Play. 15 questions, 10 seconds each.",
                  8, 84, 190, 12, 17, 6);
  if (v.portalUrl[0] != '\0') {
    int modules = drawQr(c, v.portalUrl, 214, 96, 3, 6);
    c.setTextSize(1);
    c.setTextColor(COLOR_MUTED);
    drawCenteredText(c, "scan for controller",
                     214 + (modules * 3) / 2, 96 + modules * 3 + 10);
  }
  if (v.highScore > 0) {
    char best[24];
    snprintf(best, sizeof(best), "BEST %u %s", v.highScore, v.highName);
    c.setTextColor(COLOR_GOLD);
    drawCenteredText(c, best, 160, 206);
  }
  tftPanel.pushFull();
}

void drawGameOver(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  if (v.roundComplete) {
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(3);
    drawCenteredText(c, "ROUND COMPLETE!", 160, 44);
  } else {
    c.setTextColor(kRed);
    c.setTextSize(4);
    drawCenteredText(c, "GAME OVER", 160, 44);
  }
  c.setTextColor(COLOR_MUTED);
  c.setTextSize(2);
  drawCenteredText(c, "SCORE", 160, 92);
  char sc[8];
  snprintf(sc, sizeof(sc), "%u", v.score);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(5);
  drawCenteredText(c, sc, 160, 112);
  if (v.newHigh) {
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(2);
    drawCenteredText(c, "NEW HIGH SCORE!", 160, 162);
    if (v.highName[0] != '\0') {
      c.setTextColor(ST77XX_WHITE);
      drawCenteredText(c, v.highName, 160, 184);
    } else {
      c.setTextColor(COLOR_MUTED);
      c.setTextSize(1);
      drawCenteredText(c, "enter your name on the phone", 160, 186);
    }
  } else if (v.highScore > 0) {
    char best[24];
    snprintf(best, sizeof(best), "BEST %u %s", v.highScore, v.highName);
    c.setTextColor(COLOR_MUTED);
    c.setTextSize(2);
    drawCenteredText(c, best, 160, 176);
  }
  tftPanel.pushFull();
}

void applyPenaltyLeds(const TriviaView& v) {
  int strikes = (v.phase == TRIVIA_LOBBY) ? 0 : v.strikes;
  if (strikes > 3) strikes = 3;
  if (strikes == d.leds) return;
  d.leds = strikes;
  // Count-LED grouping on this board: group A lamps 1-2 + group B lamp 1
  // give three visible penalty lights in a row.
  int lampsA = strikes >= 2 ? 2 : strikes;  // 1 or 2 lamps on group A
  int lampsB = strikes >= 3 ? 1 : 0;        // third strike adds group B
  setCountLeds(lampsA, lampsB, 0);
}

void applyMatrices(const TriviaView& v) {
  int home, away, per;
  if (v.phase == TRIVIA_QUESTION || v.phase == TRIVIA_REVEAL) {
    home = v.score > 99 ? 99 : (int)v.score;
    away = v.questionIndex + 1;
    per = v.question != nullptr ? v.question->difficulty : -1;
  } else if (v.phase == TRIVIA_GAME_OVER) {
    home = v.score > 99 ? 99 : (int)v.score;
    away = MAX7219_SCORE_BLANK;
    per = MAX7219_SCORE_BLANK;
  } else {
    home = away = per = MAX7219_SCORE_BLANK;
  }
  if (home != d.mHome || away != d.mAway || per != d.mPer) {
    d.mHome = home;
    d.mAway = away;
    d.mPer = per;
    setMax7219Display(home, away, per);
  }
}

void applyClock(const TriviaView& v, uint32_t nowMs) {
  int sec = 0;
  if (v.phase == TRIVIA_QUESTION) {
    int32_t rem = (int32_t)(v.questionEndMs - nowMs);
    if (rem < 0) rem = 0;
    sec = (rem + 999) / 1000;
  }
  if (sec != d.tmSec) {
    d.tmSec = sec;
    tm1637ShowPair(0, sec, true);
  }
}

}  // namespace

namespace trivia {

void renderScreen(uint32_t nowMs) {
  static TriviaView v = {};
  static uint32_t lastGen = 0;
  takeView(v, lastGen);  // no fresh publish leaves the previous copy in place
  if (!v.valid) return;

  // Full repaints only on phase changes (a full push costs ~1 s on the
  // bit-banged bus); same-phase publishes animate incrementally below.
  if (v.phase != d.phase) {
    d = Drawn{};  // every cached value back to "never drawn"
    d.phase = v.phase;
    if (v.phase == TRIVIA_LOBBY) {
      drawLobby(v);
      d.high = v.highScore;
      strlcpy(d.highName, v.highName, sizeof(d.highName));
    } else if (v.phase == TRIVIA_GAME_OVER) {
      drawGameOver(v);
      d.score = v.score;
      d.high = v.highScore;
      strlcpy(d.highName, v.highName, sizeof(d.highName));
    } else {
      drawQuestionFrame(v, v.phase == TRIVIA_REVEAL ? 0 : 100);
      d.barTenth = v.phase == TRIVIA_REVEAL ? 0 : 100;
      d.qIndex = v.questionIndex;
      d.locked = v.lockedChoice;
      d.correct = v.phase == TRIVIA_REVEAL ? v.correctIndex : 255;
      d.score = v.score;
      d.lastPoints = v.lastPoints;
      d.strikes = v.strikes;
      d.high = v.highScore;
      strlcpy(d.highName, v.highName, sizeof(d.highName));
    }
  }

  if (v.phase == TRIVIA_QUESTION || v.phase == TRIVIA_REVEAL) {
    // Lock-in highlight: only the picked card changes.
    if (v.lockedChoice != d.locked) {
      d.locked = v.lockedChoice;
      if (v.lockedChoice != TRIVIA_NO_LOCK) drawCard(v, v.lockedChoice, true);
    }
    if (v.score != d.score) {
      d.score = v.score;
      drawHeader(v);
    }
    if (v.strikes != d.strikes || v.lastPoints != d.lastPoints ||
        v.highScore != d.high || strcmp(v.highName, d.highName) != 0) {
      d.strikes = v.strikes;
      d.lastPoints = v.lastPoints;
      d.high = v.highScore;
      strlcpy(d.highName, v.highName, sizeof(d.highName));
      drawFooter(v);
    }
    if (v.phase == TRIVIA_QUESTION) {
      int32_t rem = (int32_t)(v.questionEndMs - nowMs);
      if (rem < 0) rem = 0;
      int tenth = (rem + 99) / 100;
      if (tenth > 100) tenth = 100;
      if (tenth != d.barTenth) {
        d.barTenth = tenth;
        drawBar(tenth);
      }
    }
  } else if (v.phase == TRIVIA_GAME_OVER &&
             (v.highScore != d.high || strcmp(v.highName, d.highName) != 0)) {
    // Name arrived for the new record — repaint the result screen.
    d.high = v.highScore;
    strlcpy(d.highName, v.highName, sizeof(d.highName));
    drawGameOver(v);
  }

  applyClock(v, nowMs);
  applyMatrices(v);
  applyPenaltyLeds(v);
}

}  // namespace trivia
