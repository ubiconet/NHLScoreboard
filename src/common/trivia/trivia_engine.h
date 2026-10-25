#pragma once

#include <Arduino.h>

// ============================================================================
// Trivia mode — game-show trivia on the scoreboard, controlled from the
// setup portal (phone-friendly web page).
// ============================================================================
// Fully sport-agnostic: the sport layer supplies a question bank
// (TriviaBank) through the sport:: contract; round flow, scoring, high-score
// storage, the controller web page, and the on-device screen all live here
// in common/trivia.
//
// Threading model (mirrors manual mode):
//   * The ENGINE runs on the network task (core 0): the portal route
//     wrappers call the action/build functions, tick() advances the round
//     clock, and every state change is published to the render core as a
//     TriviaView snapshot.
//   * The SCREEN (trivia_screen.cpp) runs on the render core inside
//     sport::tick(): it draws the current view on the TFT, the score
//     matrices, the TM1637 countdown, and the penalty LEDs. It never
//     mutates engine state.
//
// Round rules: 15 questions — 5 easy, 5 medium, 5 hard. Each question has
// a 10 s answer window; the player locks A/B/C/D from the web page any
// time inside the window (lock-in is highlighted immediately). When the
// clock runs out the correct answer is revealed and the question is
// scored: up to 100 points, faster answers worth more. A wrong answer (or
// a timeout) lights one penalty LED; the third wrong answer ends the run.

// One bank question. Text budgets match the on-device screen layout:
// question <= ~90 chars, each answer <= 22 chars.
struct TriviaQuestion {
  uint8_t difficulty;      // 1 = easy, 2 = medium, 3 = hard
  uint8_t correctIndex;    // 0..3 — index into answers[]
  const char* question;
  const char* answers[4];
};

// The sport-supplied bank: three pools by difficulty (the round ramps
// easy -> medium -> hard).
struct TriviaBank {
  const TriviaQuestion* easy;
  size_t easyCount;
  const TriviaQuestion* medium;
  size_t mediumCount;
  const TriviaQuestion* hard;
  size_t hardCount;
};

// ---- Round rules (also described on the web page) ----
// Defined ahead of the multiplayer block: the MP capacity limits and the
// board-row struct build on these.
static const uint8_t TRIVIA_NO_LOCK = 255;
static const uint8_t TRIVIA_ROUND_QUESTIONS = 15;
static const uint8_t TRIVIA_QUESTIONS_PER_LEVEL = 5;  // 5 easy, 5 medium, 5 hard
static const uint32_t TRIVIA_QUESTION_MS = 10000;     // answer window per question
static const uint32_t TRIVIA_REVEAL_MS = 4000;        // answer/result display
static const uint8_t TRIVIA_MAX_STRIKES = 3;          // 3rd wrong answer ends the run
static const uint16_t TRIVIA_MAX_POINTS_PER_Q = 100;  // top score per question
static const size_t TRIVIA_NAME_MAX = 12;             // stored name length cap
static const uint16_t TRIVIA_FIREWORKS_MIN_POINTS = 80;  // score this high = fireworks

// ---- Multiplayer party mode ----
// One host (the /trivia controller page) opens a lobby, players sign up
// from their own phones at /play, and everyone plays the same questions
// in lockstep. The host tunes the party in the lobby: question count,
// seconds per question, which difficulty levels may appear (multi-
// select), and the standings pause between questions. The device becomes
// the room display: standings between questions, winner + matrix
// fireworks at the end. No strikes and no early exit — everyone plays
// every question, ranking is by score.
// Player capacity is bounded on purpose: every phone polls once a second
// through the single-threaded web server (see the capacity notes in
// AGENTS.md) — 15 keeps request rate and per-connection heap comfortable.
static const uint8_t TRIVIA_MP_MAX_PLAYERS = 15;
static const uint8_t TRIVIA_MP_DEFAULT_QUESTIONS = 15;
static const uint8_t TRIVIA_MP_MIN_QUESTIONS = 3;
static const uint8_t TRIVIA_MP_MAX_QUESTIONS = TRIVIA_ROUND_QUESTIONS;
static const uint8_t TRIVIA_MP_DEFAULT_SECONDS = 10;
static const uint8_t TRIVIA_MP_MIN_SECONDS = 5;
static const uint8_t TRIVIA_MP_MAX_SECONDS = 60;
// Host multi-select: which difficulty levels may appear, as a bitmask.
// The round still ramps low -> high across whatever subset is allowed.
static const uint8_t TRIVIA_MP_DIFF_EASY = 0x1;
static const uint8_t TRIVIA_MP_DIFF_MEDIUM = 0x2;
static const uint8_t TRIVIA_MP_DIFF_HARD = 0x4;
static const uint8_t TRIVIA_MP_DEFAULT_DIFFS =
    TRIVIA_MP_DIFF_EASY | TRIVIA_MP_DIFF_MEDIUM | TRIVIA_MP_DIFF_HARD;
// Standings pause between questions, in seconds (0 = skip the standings
// card: the next question starts right after the answer reveal).
static const uint8_t TRIVIA_MP_DEFAULT_GAP_S = 3;
static const uint8_t TRIVIA_MP_MIN_GAP_S = 0;
static const uint8_t TRIVIA_MP_MAX_GAP_S = 30;

// One leaderboard row shipped in the view (name + score, top 5).
struct TriviaBoardRow {
  char name[TRIVIA_NAME_MAX + 1];
  uint16_t score;
};

// What the render core draws. POD, published through a SnapshotChannel on
// every state change; the countdown itself is computed render-side from
// questionEndMs so it needs no per-second publishes.
struct TriviaView {
  bool valid;
  uint8_t phase;            // TriviaPhaseValue
  uint8_t questionIndex;    // 0-based, valid while a round runs
  uint8_t totalQuestions;   // Q-counter denominator (configurable in MP)
  uint8_t lockedChoice;     // 0..3, 255 = none locked yet
  uint8_t correctIndex;     // meaningful at REVEAL / GAME_OVER only
  bool lastCorrect;         // reveal/gameover: outcome of the last question
  uint16_t lastPoints;      // reveal/gameover: points scored on it
  uint16_t score;
  uint8_t strikes;          // wrong answers so far (3 = game over)
  uint32_t questionEndMs;   // QUESTION phase: millis() deadline
  uint32_t questionWindowMs;  // answer window length (timer bar scale)
  const TriviaQuestion* question;  // null in lobby/game over
  bool fireworks;           // reveal: 80+ point answer — celebrate on matrices
  bool roundComplete;       // game over: survived all 15 questions
  bool newHigh;             // game over: beat the stored record
  char highName[16];        // stored record holder ("" until named)
  uint16_t highScore;       // stored record
  char portalUrl[40];       // lobby: controller/join URL for the QR code
  // Multiplayer fields (mp == false: ignore everything below).
  bool mp;                  // a multiplayer session owns the mode
  uint8_t playerCount;      // players signed up
  uint8_t correctCount;     // reveal: how many answered right
  uint8_t topCount;         // rankings/gameover: rows in top[]
  TriviaBoardRow top[5];    // standings, best first (ties: earlier joiner)
  char winnerName[TRIVIA_NAME_MAX + 1];  // game over: rank 1
  uint16_t winnerScore;
};

enum TriviaPhaseValue : uint8_t {
  TRIVIA_LOBBY = 0,
  TRIVIA_QUESTION = 1,
  TRIVIA_REVEAL = 2,
  TRIVIA_GAME_OVER = 3,
  TRIVIA_RANKINGS = 4,      // MP only: standings card between questions
};

namespace trivia {

// Shell boot hook (before network services start): stores the bank and
// loads the saved high score from NVS.
void begin(const TriviaBank& bank, const char* deviceName);

// ---- render core ----
bool isModeActive();                     // trivia owns the display
void renderScreen(uint32_t nowMs);       // trivia_screen.cpp
bool takeView(TriviaView& out, uint32_t& lastGen);
// One-shot wrong-answer cue, raised at every reveal the player missed
// (timeout included). The sport layer consumes it on the render core and
// answers with its end-of-period buzzer clip (mirrors manual mode's
// consumeManualPeriodEndBuzzer — the engine can't name sport audio).
bool consumeWrongAnswerCue();

// ---- network task ----
void tick();                             // countdown + phase transitions
void enterMode();                        // /trivia page opened: engage mode
void exitMode();                         // leave trivia (exit button / other mode)
uint16_t getHighScore();                 // for the settings menu
const char* getHighScoreName();

// HTTP-facing helpers. The network service owns the WebServer: its route
// wrappers parse arguments and send these bodies (transport stays out of
// the engine).
String buildPageHtml();                  // GET /trivia
String buildStateJson();                 // GET /trivia/state (polled by the page)
bool actionStart();                      // POST /trivia/start — false: no bank
bool actionAnswer(uint8_t index);        // POST /trivia/answer (a=0..3)
bool actionName(const char* name);       // POST /trivia/name (name=...)

// ---- Multiplayer party mode (same engine, shared phases) ----
// Opens the join lobby (false: no bank). Resets any solo round.
bool mpHostLobby();
// True while a multiplayer lobby or game owns the mode.
bool mpSessionActive();
// Registers a player by (cleaned, unique) name while the lobby is open.
// Returns the player's token, or 0 when rejected (lobby closed / full /
// name taken / empty).
uint16_t mpJoin(const char* name);
// Why the last mpJoin() failed: "closed", "full", "taken", or "name" —
// for the join page's error message.
const char* mpLastJoinError();
// Host settings, clamped to the ranges above. Only settable in the lobby.
// diffs is a TRIVIA_MP_DIFF_* bitmask (an empty mask falls back to all
// three levels); gapSeconds is the standings pause between questions
// (0 = next question right after the reveal).
bool mpSetConfig(uint8_t questions, uint8_t seconds, uint8_t gapSeconds,
                 uint8_t diffs);
// Closes joining and starts question 1 (false: no players / not lobby).
bool mpStart();
// Signed-up player count (for the route wrappers' error messages).
uint8_t mpPlayerCount();
// Locks a player's answer during the question window (first lock is final,
// like solo). Unknown token / wrong phase: rejected silently.
bool mpAnswer(uint16_t token, uint8_t index);
// Host page poll: session state, settings, and the live standings. The
// ok/err pair is used by the action routes' responses (polls use the
// defaults).
String buildMpHostJson(bool ok = true, const char* err = nullptr);
// Player page poll: personal state (own lock/result/score/rank + standings).
// Invalid token: {"ok":false} and the page drops back to the join form.
String buildMpPlayerJson(uint16_t token);
// The /play join page served to players' phones.
String buildMpJoinHtml();

}  // namespace trivia
