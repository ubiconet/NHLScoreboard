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
  uint8_t players = 255;  // MP lobby: repaint when sign-ups change
  uint16_t score = 9999;
  uint16_t lastPoints = 9999;
  uint16_t high = 9999;
  char highName[16] = {0};
  int barTenth = -1;
  int tmSec = -1;
  bool matricesBlanked = false;
  int leds = -1;
} d;

// ---- Fireworks (80+ point answers; looping MP winner celebration) ----
// ~1 s particle burst across the three matrices for an answer scoring 80+
// of the 100 possible points: expanding shells with random spark pixels
// that scatter and fade, cascading module to module. Runs one frame per
// render pass — never blocks the loop. At the multiplayer game-over
// screen the burst re-arms in a loop for a sustained celebration.
constexpr int FW_FRAMES = 13;
constexpr uint32_t FW_FRAME_MS = 80;
constexpr uint32_t FW_LOOP_GAP_MS = 700;  // MP winner: pause between bursts
struct Fireworks {
  bool active = false;
  uint32_t startMs = 0;     // 0 until the first frame actually draws (the
                            // reveal's full repaint eats ~1 s of bus time)
  uint32_t lastFrameMs = 0;
  uint32_t endedAtMs = 0;   // when the last burst finished (re-arm timing)
  int frame = -1;
} fw;

void drawFireworksFrame(int t) {
  uint8_t rows[3][8] = {};  // 0 = away, 1 = period, 2 = home
  for (int m = 0; m < 3; ++m) {
    float radius = t * 0.55f - m * 0.45f;  // cascade across the chain
    if (radius <= 0) continue;
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        float dx = x * 2 - 7, dy = y * 2 - 7;        // centered coords
        float dist = sqrtf(dx * dx + dy * dy) / 2;   // 0..~4.9
        bool lit = fabsf(dist - radius) < 0.8f;      // the expanding shell
        if (!lit && dist < radius && dist > radius - 2.0f) {
          lit = random(100) < 28;                    // scattering sparks
        }
        if (lit) rows[m][y] |= (uint8_t)(1 << (7 - x));
      }
    }
  }
  setMax7219Frames(rows[0], rows[2], rows[1]);
}

void tickFireworks(uint32_t nowMs) {
  if (!fw.active) return;
  if (fw.startMs == 0) fw.startMs = nowMs;  // clock starts at first draw
  if (nowMs - fw.startMs >= FW_FRAMES * FW_FRAME_MS) {
    fw.active = false;
    fw.endedAtMs = nowMs;
    uint8_t blank[8] = {0};
    setMax7219Frames(blank, blank, blank);
    d.matricesBlanked = true;
    return;
  }
  if (fw.lastFrameMs != 0 && nowMs - fw.lastFrameMs < FW_FRAME_MS) return;
  fw.lastFrameMs = nowMs;
  if (++fw.frame < FW_FRAMES) drawFireworksFrame(fw.frame);
}

uint16_t cardFill(const TriviaView& v, int i) {
  if (v.phase == TRIVIA_REVEAL) {
    if (i == v.correctIndex) return kGreen;   // the actual answer
    if (!v.mp && i == v.lockedChoice) return kRed;  // solo: the player's miss
    return COLOR_CARD;                        // MP: no device player
  }
  if (!v.mp && v.phase == TRIVIA_QUESTION && v.lockedChoice == i) {
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
  bool inRound = v.phase == TRIVIA_QUESTION || v.phase == TRIVIA_REVEAL ||
                 v.phase == TRIVIA_RANKINGS;
  if (inRound) {
    char q[10];
    snprintf(q, sizeof(q), "Q%u/%u", v.questionIndex + 1,
             (unsigned)v.totalQuestions);
    c.setTextColor(ST77XX_WHITE);
    drawCenteredText(c, q, 160, 1);
    char sc[14];
    if (v.mp) {
      // Party mode: the number to watch is the leader's score (player
      // count until the first reveal puts points on the board).
      if (v.topCount > 0 && v.top[0].score > 0) {
        snprintf(sc, sizeof(sc), "%u", v.top[0].score);
      } else {
        snprintf(sc, sizeof(sc), "P%u", v.playerCount);
      }
    } else {
      snprintf(sc, sizeof(sc), "%u", v.score);
    }
    c.setTextColor(COLOR_GOLD);
    c.setCursor(312 - (int)strlen(sc) * 12, 1);
    c.print(sc);
  } else if (!v.mp && v.highScore > 0) {
    char best[16];
    snprintf(best, sizeof(best), "BEST %u", v.highScore);
    c.setTextColor(COLOR_MUTED);
    c.setCursor(312 - (int)strlen(best) * 12, 1);
    c.print(best);
  }
}

void drawBar(int pct, bool low) {  // pct: 0..100 of the answer window
  GFXcanvas16& c = tftPanel.canvas();
  c.fillRect(kBarX, kBarY, kBarW, kBarH, kBarBg);
  int w = kBarW * pct / 100;
  if (w > 0) {
    c.fillRect(kBarX, kBarY, w, kBarH, low ? kRed : COLOR_GOLD);
  }
  tftPanel.pushBand(kBarY, kBarH);
}

void drawFooter(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillRect(0, kFooterY, 320, 240 - kFooterY, COLOR_BG);
  if (v.mp) {
    // Party reveal: the room's verdict, not one player's.
    if (v.phase == TRIVIA_REVEAL) {
      char line[24];
      snprintf(line, sizeof(line), "%u OF %u CORRECT", v.correctCount,
               v.playerCount);
      c.setTextSize(2);
      c.setTextColor(v.correctCount > 0 ? kGreen : kRed);
      drawCenteredText(c, line, 160, kFooterY + 1);
      if (v.topCount > 0 && v.top[0].score > 0) {
        char lead[24];
        snprintf(lead, sizeof(lead), "TOP %s %u", v.top[0].name,
                 v.top[0].score);
        c.setTextColor(COLOR_GOLD);
        drawCenteredText(c, lead, 160, kFooterY + 17);
      }
    }
    tftPanel.pushBand(kFooterY, 240 - kFooterY);
    return;
  }
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
  // Center: the reveal verdict, then the running score — the TFT owns the
  // score display after every question (the matrices stay dark in trivia).
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
    drawCenteredText(c, line, 160, kFooterY);
    char score[20];
    snprintf(score, sizeof(score), "SCORE %u", v.score);
    c.setTextSize(2);
    c.setTextColor(COLOR_GOLD);
    drawCenteredText(c, score, 160, kFooterY + 16);
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

void drawQuestionFrame(const TriviaView& v, int barPct) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  c.setTextSize(2);
  c.setTextColor(ST77XX_WHITE);
  drawWrappedText(c, v.question->question, 8, kQY, 304, 12, 17, 4);
  for (int i = 0; i < 4; ++i) drawCard(v, i, false);
  drawFooter(v);
  tftPanel.pushFull();
  drawBar(barPct, false);  // static full frame first, then the fresh band on top
}

void drawLobby(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  if (v.mp) {
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(3);
    drawCenteredText(c, "TRIVIA NIGHT", 160, 36);
    c.setTextSize(2);
    c.setTextColor(COLOR_MUTED);
    drawWrappedText(c, "Players: join with the link your host "
                       "shared, or scan the code.",
                    8, 76, 190, 12, 17, 4);
    // Live sign-up count + the names so far.
    char count[20];
    snprintf(count, sizeof(count), "%u PLAYER%s IN", v.playerCount,
             v.playerCount == 1 ? "" : "S");
    c.setTextColor(v.playerCount > 0 ? COLOR_GOLD : COLOR_MUTED);
    c.setCursor(8, 148);
    c.print(count);
    if (v.playerCount > 0) {
      char names[96] = {};
      size_t used = 0;
      for (uint8_t i = 0;
           i < v.playerCount && i < 5 && used + 14 < sizeof(names); ++i) {
        // Player names ride in the view only as the top-5 board; in the
        // lobby the board mirrors the first five joiners in order.
        if (i >= v.topCount) break;
        used += snprintf(names + used, sizeof(names) - used, "%s%s",
                         i > 0 ? ", " : "", v.top[i].name);
      }
      c.setTextSize(1);
      c.setTextColor(COLOR_MUTED);
      drawWrappedText(c, names, 8, 170, 190, 6, 12, 3);
    }
    c.setTextSize(2);
    c.setTextColor(COLOR_MUTED);
    drawCenteredText(c, "waiting for the host...", 160, 206);
  } else {
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(3);
    drawCenteredText(c, "TRIVIA MODE", 160, 40);
    c.setTextSize(2);
    c.setTextColor(COLOR_MUTED);
    drawWrappedText(c, "Open the controller on your phone and press Solo "
                       "Play. 15 questions, 10 seconds each.",
                    8, 84, 190, 12, 17, 6);
  }
  if (v.portalUrl[0] != '\0') {
    int modules = drawQr(c, v.portalUrl, 214, 96, 3, 6);
    c.setTextSize(1);
    c.setTextColor(COLOR_MUTED);
    drawCenteredText(c, v.mp ? "scan to join" : "scan for controller",
                     214 + (modules * 3) / 2, 96 + modules * 3 + 10);
  }
  if (!v.mp && v.highScore > 0) {
    char best[24];
    snprintf(best, sizeof(best), "BEST %u %s", v.highScore, v.highName);
    c.setTextColor(COLOR_GOLD);
    drawCenteredText(c, best, 160, 206);
  }
  tftPanel.pushFull();
}

// Standings card — the between-questions leaderboard (3 s) and the list
// under the multiplayer winner screen.
void drawStandingsList(const TriviaView& v, int y0, int rowH) {
  GFXcanvas16& c = tftPanel.canvas();
  for (uint8_t i = 0; i < v.topCount && i < 5; ++i) {
    int y = y0 + i * rowH;
    if (i == 0) c.fillRoundRect(6, y - 2, 308, rowH, 4, COLOR_CARD);
    char rank[4];
    snprintf(rank, sizeof(rank), "%u", i + 1);
    c.setTextSize(2);
    c.setTextColor(i == 0 ? COLOR_GOLD : COLOR_MUTED);
    c.setCursor(16, y);
    c.print(rank);
    c.setTextColor(i == 0 ? ST77XX_WHITE : COLOR_MUTED);
    c.setCursor(48, y);
    printClipped(c, v.top[i].name, 12);
    char score[8];
    snprintf(score, sizeof(score), "%u", v.top[i].score);
    c.setTextColor(i == 0 ? COLOR_GOLD : ST77XX_WHITE);
    c.setCursor(304 - (int)strlen(score) * 12, y);
    c.print(score);
  }
}

void drawRankings(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(3);
  drawCenteredText(c, "STANDINGS", 160, 32);
  drawStandingsList(v, 72, 28);
  c.setTextSize(1);
  c.setTextColor(COLOR_MUTED);
  drawCenteredText(c, "next question coming up...", 160, 226);
  tftPanel.pushFull();
}

void drawGameOver(const TriviaView& v) {
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  drawHeader(v);
  if (v.mp) {
    // Winner celebration: name center stage, score, then the final
    // standings. The matrix fireworks loop while this screen is up
    // (armed in renderScreen).
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(2);
    drawCenteredText(c, "WINNER", 160, 34);
    c.setTextColor(ST77XX_WHITE);
    c.setTextSize(4);
    drawCenteredText(c, v.winnerName, 160, 56);
    char sc[16];
    snprintf(sc, sizeof(sc), "%u PTS", v.winnerScore);
    c.setTextColor(COLOR_GOLD);
    c.setTextSize(2);
    drawCenteredText(c, sc, 160, 90);
    drawStandingsList(v, 122, 22);
    tftPanel.pushFull();
    return;
  }
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
  (void)v;
  // Trivia keeps the score matrices dark — the score lives on the TFT
  // (header + reveal footer) and only the fireworks may light them.
  if (fw.active) return;
  if (!d.matricesBlanked) {
    d.matricesBlanked = true;
    setMax7219Display(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK,
                      MAX7219_SCORE_BLANK);
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
    fw.active = false;  // a phase change ends any running burst
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
    } else if (v.phase == TRIVIA_RANKINGS) {
      drawRankings(v);  // MP standings card between questions (3 s)
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
      // Lightning answer: arm the matrix fireworks for the reveal.
      if (v.phase == TRIVIA_REVEAL && v.fireworks) fw.active = true;
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
      int32_t window = (int32_t)v.questionWindowMs;
      if (window <= 0) window = TRIVIA_QUESTION_MS;
      int pct = rem * 100 / window;
      if (pct > 100) pct = 100;
      if (pct != d.barTenth) {
        d.barTenth = pct;
        drawBar(pct, rem < 3000);  // red under 3 s, like solo
      }
    }
  } else if (v.phase == TRIVIA_GAME_OVER &&
             (v.highScore != d.high || strcmp(v.highName, d.highName) != 0)) {
    // Name arrived for the new record — repaint the result screen.
    d.high = v.highScore;
    strlcpy(d.highName, v.highName, sizeof(d.highName));
    drawGameOver(v);
  } else if (v.phase == TRIVIA_LOBBY && v.mp &&
             v.playerCount != d.players) {
    // Party lobby: someone signed up — refresh the count + names + QR.
    d.players = v.playerCount;
    drawLobby(v);
  }

  if (v.phase == TRIVIA_REVEAL) {
    tickFireworks(nowMs);
  } else if (v.phase == TRIVIA_GAME_OVER && v.mp) {
    // Winner celebration: keep launching matrix bursts while the final
    // standings are up.
    if (!fw.active && nowMs - fw.endedAtMs >= FW_LOOP_GAP_MS) {
      fw.active = true;
      fw.frame = -1;
      fw.startMs = 0;
      fw.lastFrameMs = 0;
    }
    tickFireworks(nowMs);
  }
  applyClock(v, nowMs);
  applyMatrices(v);
  applyPenaltyLeds(v);
}

}  // namespace trivia
