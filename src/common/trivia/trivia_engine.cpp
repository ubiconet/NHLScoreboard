#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "common/comms/network_service.h"
#include "common/data/snapshot_channel.h"
#include "trivia_engine.h"

// Trivia engine (see trivia_engine.h for the threading model). All game
// state below is touched ONLY from the network task (web handlers + tick);
// the render core reads the published TriviaView snapshots.

namespace {

TriviaBank sBank = {};
const char* sDeviceName = "";
bool sBegun = false;

volatile bool sModeActive = false;

// ---- round state (network task only) ----
uint8_t sPhase = TRIVIA_LOBBY;
uint8_t sQIndex = 0;
const TriviaQuestion* sQuestion = nullptr;
const TriviaQuestion* sUsed[TRIVIA_ROUND_QUESTIONS] = {};
uint8_t sLocked = TRIVIA_NO_LOCK;
bool sLockedValid = false;
uint32_t sLockedRemainingMs = 0;
uint8_t sCorrect = 0;
uint32_t sQEndMs = 0;
uint32_t sRevealEndMs = 0;
uint16_t sScore = 0;
uint8_t sStrikes = 0;
bool sLastCorrect = false;
uint16_t sLastPoints = 0;
bool sRoundComplete = false;
bool sNewHigh = false;
bool sFireworks = false;
// Cross-core one-shot: set on the network task at every wrong reveal,
// consumed on the render core for the buzzer (volatile — benign race, the
// worst case is one extra or missed poll window edge).
volatile bool sWrongAnswerCue = false;

// ---- multiplayer state (network task only) ----
struct MpPlayer {
  uint16_t token;                       // 0 never issued; identifies the phone
  char name[TRIVIA_NAME_MAX + 1];
  uint16_t score;
  uint8_t locked;                       // per-current-question lock
  uint32_t lockedRemainMs;              // time left when they locked
  bool lastCorrect;
  uint16_t lastPoints;
  uint8_t rank;                         // standing after the latest reveal
};
MpPlayer sPlayers[TRIVIA_MP_MAX_PLAYERS];
uint8_t sMpCount = 0;
uint16_t sMpNextToken = 1;              // tokens handed out this session
bool sMpActive = false;
uint8_t sMpQuestions = TRIVIA_MP_DEFAULT_QUESTIONS;
uint8_t sMpSeconds = TRIVIA_MP_DEFAULT_SECONDS;
uint32_t sMpWindowMs = TRIVIA_MP_DEFAULT_SECONDS * 1000;
uint32_t sRankEndMs = 0;                // RANKINGS phase deadline
uint8_t sMpCorrectCount = 0;            // last reveal: right-answer count
TriviaBoardRow sTop[5];
uint8_t sTopCount = 0;

// ---- persisted record (NVS "trivia") ----
uint16_t sHighScore = 0;
char sHighName[16] = "";
char sPortalUrl[40] = "";

SnapshotChannel<TriviaView> sViewChannel;

const TriviaQuestion* poolFor(uint8_t level, size_t& count) {
  switch (level) {
    case 1: count = sBank.easyCount; return sBank.easy;
    case 2: count = sBank.mediumCount; return sBank.medium;
    default: count = sBank.hardCount; return sBank.hard;
  }
}

// Random pick from the requested difficulty pool, avoiding the questions
// already used this round. Falls back to the neighboring pools when a pool
// is empty or exhausted (the bank is 250-per-level, so this only guards a
// sport shipping a tiny bank).
const TriviaQuestion* pickQuestion(uint8_t level) {
  for (int fallback = 0; fallback < 3; ++fallback) {
    size_t count = 0;
    const TriviaQuestion* pool = poolFor(level, count);
    if (pool != nullptr && count > 0) {
      for (int tries = 0; tries < 24; ++tries) {
        const TriviaQuestion* candidate = &pool[random(count)];
        bool used = false;
        for (uint8_t i = 0; i < sQIndex && i < TRIVIA_ROUND_QUESTIONS; ++i) {
          if (sUsed[i] == candidate) {
            used = true;
            break;
          }
        }
        if (!used) return candidate;
      }
    }
    level = (level >= 3) ? 1 : level + 1;
  }
  return nullptr;
}

// Name cleaner shared by the solo record field and multiplayer sign-up:
// short, printable, safe for the canvas + portal pages. Returns false when
// nothing printable is left.
bool cleanTriviaName(const char* in, char* out, size_t outSize) {
  if (in == nullptr) return false;
  size_t n = 0;
  bool lastSpace = true;  // also collapses leading spaces
  for (const char* p = in; *p != '\0' && n < outSize - 1; ++p) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ' ';
    if (!ok || (c == ' ' && lastSpace)) {
      lastSpace = (c == ' ');
      continue;
    }
    lastSpace = (c == ' ');
    out[n++] = c;
  }
  while (n > 0 && out[n - 1] == ' ') --n;
  out[n] = '\0';
  return n > 0;
}

// Ranks every player (1 + players strictly ahead; ties keep join order)
// and refreshes the top-5 board copy. Called once per reveal.
void computeStandings() {
  for (uint8_t i = 0; i < sMpCount; ++i) {
    uint8_t rank = 1;
    for (uint8_t j = 0; j < sMpCount; ++j) {
      if (sPlayers[j].score > sPlayers[i].score) ++rank;
    }
    sPlayers[i].rank = rank;
  }
  sTopCount = sMpCount < 5 ? sMpCount : 5;
  for (uint8_t slot = 0; slot < sTopCount; ++slot) {
    int best = -1;
    for (uint8_t i = 0; i < sMpCount; ++i) {
      if (sPlayers[i].rank < 1 || sPlayers[i].rank > sTopCount) continue;
      if (best >= 0) {
        // strictly better score, or equal score with earlier join order
        bool better = sPlayers[i].score > sPlayers[best].score ||
                      (sPlayers[i].score == sPlayers[best].score && i < best);
        if (!better) continue;
      }
      bool taken = false;  // a row already placed in an earlier slot
      for (uint8_t s2 = 0; s2 < slot && !taken; ++s2) {
        taken = strcmp(sTop[s2].name, sPlayers[i].name) == 0;
      }
      if (!taken) best = i;
    }
    if (best >= 0) {
      strlcpy(sTop[slot].name, sPlayers[best].name, sizeof(sTop[slot].name));
      sTop[slot].score = sPlayers[best].score;
    }
  }
}

void publishView() {
  TriviaView v = {};
  v.valid = true;
  v.phase = sPhase;
  v.questionIndex = sQIndex;
  v.totalQuestions = sMpActive ? sMpQuestions : TRIVIA_ROUND_QUESTIONS;
  v.lockedChoice = sLocked;
  v.correctIndex = (sPhase == TRIVIA_REVEAL || sPhase == TRIVIA_GAME_OVER ||
                    sPhase == TRIVIA_RANKINGS)
                       ? sCorrect
                       : 0;
  v.lastCorrect = sLastCorrect;
  v.lastPoints = sLastPoints;
  v.score = sScore;
  v.strikes = sStrikes;
  v.questionEndMs = sQEndMs;
  v.question = (sPhase == TRIVIA_QUESTION || sPhase == TRIVIA_REVEAL)
                   ? sQuestion
                   : nullptr;
  v.fireworks = (sPhase == TRIVIA_REVEAL) && sFireworks;
  v.roundComplete = sRoundComplete;
  v.newHigh = sNewHigh;
  strlcpy(v.highName, sHighName, sizeof(v.highName));
  v.highScore = sHighScore;
  if (sPhase == TRIVIA_LOBBY) {
    strlcpy(v.portalUrl, sPortalUrl, sizeof(v.portalUrl));
  }
  v.mp = sMpActive;
  v.playerCount = sMpCount;
  v.correctCount = sMpCorrectCount;
  v.topCount = sTopCount;
  for (uint8_t i = 0; i < sTopCount && i < 5; ++i) v.top[i] = sTop[i];
  if (sPhase == TRIVIA_GAME_OVER && sMpActive && sMpCount > 0) {
    strlcpy(v.winnerName, sTop[0].name, sizeof(v.winnerName));
    v.winnerScore = sTop[0].score;
  }
  sViewChannel.publish(v);
}

void startQuestion(uint32_t nowMs) {
  // Difficulty ramps easy -> hard across the round: fixed thirds solo,
  // proportional to the host's question count in multiplayer.
  uint8_t level;
  if (sMpActive) {
    level = 1 + (uint8_t)((uint16_t)sQIndex * 3 / sMpQuestions);
    if (level > 3) level = 3;
  } else {
    level = 1 + sQIndex / TRIVIA_QUESTIONS_PER_LEVEL;  // 1, 2, 3
  }
  const TriviaQuestion* q = pickQuestion(level);
  if (q == nullptr) {
    // Bank completely empty (sport shipped no trivia): bail out gracefully.
    sPhase = TRIVIA_GAME_OVER;
    sRoundComplete = false;
    publishView();
    return;
  }
  sQuestion = q;
  sUsed[sQIndex] = q;
  sLocked = TRIVIA_NO_LOCK;
  sLockedValid = false;
  sLockedRemainingMs = 0;
  for (uint8_t i = 0; i < sMpCount; ++i) sPlayers[i].locked = TRIVIA_NO_LOCK;
  sCorrect = q->correctIndex;
  sQEndMs = nowMs + (sMpActive ? sMpWindowMs : TRIVIA_QUESTION_MS);
  sPhase = TRIVIA_QUESTION;
  DBG_PRINTF("[TRIVIA] Q%u lvl %u: %.40s\n", sQIndex + 1, level, q->question);
  publishView();
}

void enterReveal(uint32_t nowMs) {
  if (sMpActive) {
    // Score every player against the just-closed window, then refresh the
    // standings board the TFT shows next.
    sCorrect = sQuestion != nullptr ? sQuestion->correctIndex : 0;
    sMpCorrectCount = 0;
    for (uint8_t i = 0; i < sMpCount; ++i) {
      MpPlayer& p = sPlayers[i];
      p.lastCorrect = p.locked != TRIVIA_NO_LOCK && p.locked == sCorrect;
      p.lastPoints = 0;
      if (p.lastCorrect) {
        ++sMpCorrectCount;
        uint32_t rem = p.lockedRemainMs;
        if (rem > sMpWindowMs) rem = sMpWindowMs;
        p.lastPoints = (uint16_t)((rem * TRIVIA_MAX_POINTS_PER_Q +
                                   sMpWindowMs / 2) / sMpWindowMs);
        p.score += p.lastPoints;
      }
    }
    computeStandings();
    sLastCorrect = false;
    sLastPoints = 0;
    sPhase = TRIVIA_REVEAL;
    sRevealEndMs = nowMs + TRIVIA_REVEAL_MS;
    DBG_PRINTF("[TRIVIA] MP reveal: %u/%u correct\n", sMpCorrectCount,
               sMpCount);
    publishView();
    return;
  }
  sLastCorrect = sLockedValid && sLocked == sCorrect;
  sLastPoints = 0;
  if (sLastCorrect) {
    uint32_t rem = sLockedRemainingMs;
    if (rem > TRIVIA_QUESTION_MS) rem = TRIVIA_QUESTION_MS;
    sLastPoints = (uint16_t)((rem * TRIVIA_MAX_POINTS_PER_Q +
                              TRIVIA_QUESTION_MS / 2) / TRIVIA_QUESTION_MS);
    sScore += sLastPoints;
    // Fireworks celebrate high-scoring answers: 80+ of the 100 possible
    // points (points scale with time remaining, so this replaces the old
    // "locked within three seconds" rule with the score it implied).
    sFireworks = sLastPoints >= TRIVIA_FIREWORKS_MIN_POINTS;
  } else {
    if (sStrikes < 255) ++sStrikes;
    sFireworks = false;
    sWrongAnswerCue = true;  // render core answers with the buzzer
  }
  sPhase = TRIVIA_REVEAL;
  sRevealEndMs = nowMs + TRIVIA_REVEAL_MS;
  DBG_PRINTF("[TRIVIA] reveal: %s +%u (score %u strikes %u%s)\n",
             sLastCorrect ? "correct" : "wrong", sLastPoints, sScore, sStrikes,
             sFireworks ? " FIREWORKS" : "");
  publishView();
}

void enterGameOver(uint32_t nowMs) {
  (void)nowMs;
  if (sMpActive) {
    // Party results: everyone played every question, so the round always
    // completes; the winner is the standings leader (rank 1, earliest
    // joiner on ties). The multiplayer score is a different game shape
    // from the solo run, so it never touches the stored solo record.
    sRoundComplete = true;
    sQuestion = nullptr;
    sNewHigh = false;
    sPhase = TRIVIA_GAME_OVER;
    DBG_PRINTF("[TRIVIA] MP game over: winner %s %u\n", sTop[0].name,
               sTop[0].score);
    publishView();
    return;
  }
  sRoundComplete = sStrikes < TRIVIA_MAX_STRIKES;
  sQuestion = nullptr;
  sNewHigh = sScore > 0 && sScore > sHighScore;
  if (sNewHigh) {
    // Commit the new record immediately so it can never be lost; the name
    // arrives when the player submits it (empty until then).
    sHighScore = sScore;
    sHighName[0] = '\0';
    Preferences prefs;
    prefs.begin("trivia", false);
    prefs.putInt("high", (int)sHighScore);
    prefs.putString("hname", "");
    prefs.end();
    DBG_PRINTF("[TRIVIA] new high score %u — awaiting name\n", sHighScore);
  }
  sPhase = TRIVIA_GAME_OVER;
  publishView();
}

void enterRankings(uint32_t nowMs) {
  sPhase = TRIVIA_RANKINGS;
  sRankEndMs = nowMs + TRIVIA_MP_RANKINGS_MS;
  publishView();
}

void advance(uint32_t nowMs) {
  uint8_t total = sMpActive ? sMpQuestions : TRIVIA_ROUND_QUESTIONS;
  if (!sMpActive && sStrikes >= TRIVIA_MAX_STRIKES) {
    enterGameOver(nowMs);
  } else if (sQIndex + 1 >= total) {
    enterGameOver(nowMs);
  } else {
    ++sQIndex;
    startQuestion(nowMs);
  }
}

// Minimal JSON string escaping for the authored question text (which never
// contains quotes/backslashes, but a stray one must not break the page).
String jsonEscape(const char* text) {
  String out;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    if ((unsigned char)*p >= ' ') out += *p;
  }
  return out;
}

const char* phaseName(uint8_t phase) {
  switch (phase) {
    case TRIVIA_QUESTION: return "question";
    case TRIVIA_REVEAL: return "reveal";
    case TRIVIA_RANKINGS: return "rankings";
    case TRIVIA_GAME_OVER: return "over";
    default: return "lobby";
  }
}

}  // namespace

namespace trivia {

void begin(const TriviaBank& bank, const char* deviceName) {
  sBank = bank;
  sDeviceName = deviceName ? deviceName : "";
  Preferences prefs;
  prefs.begin("trivia", true);
  sHighScore = (uint16_t)prefs.getInt("high", 0);
  strlcpy(sHighName, prefs.getString("hname", "").c_str(),
          sizeof(sHighName));
  prefs.end();
  sBegun = true;
  DBG_PRINTF("[TRIVIA] bank ready: %u/%u/%u questions, record %u (%s)\n",
             (unsigned)sBank.easyCount, (unsigned)sBank.mediumCount,
             (unsigned)sBank.hardCount, sHighScore, sHighName);
}

bool isModeActive() { return sModeActive; }

bool takeView(TriviaView& out, uint32_t& lastGen) {
  return sViewChannel.take(out, lastGen);
}

bool consumeWrongAnswerCue() {
  if (!sWrongAnswerCue) return false;
  sWrongAnswerCue = false;
  return true;
}

void tick() {
  if (!sBegun || !sModeActive) return;
  uint32_t now = millis();
  if (sPhase == TRIVIA_QUESTION && (int32_t)(now - sQEndMs) >= 0) {
    enterReveal(now);
  } else if (sPhase == TRIVIA_REVEAL && (int32_t)(now - sRevealEndMs) >= 0) {
    if (sMpActive) {
      enterRankings(now);  // standings card on the TFT between questions
    } else {
      advance(now);
    }
  } else if (sPhase == TRIVIA_RANKINGS &&
             (int32_t)(now - sRankEndMs) >= 0) {
    advance(now);
  }
}

void enterMode() {
  if (!sBegun) return;
  if (sModeActive) return;
  // Refresh the controller URL (IP can change between sessions). Runs on
  // the network task, so the Wi-Fi radio call is safe here.
  String ip = getDeviceIp();
  if (ip.length() > 0) {
    snprintf(sPortalUrl, sizeof(sPortalUrl), "http://%s/trivia", ip.c_str());
  } else {
    sPortalUrl[0] = '\0';
  }
  sPhase = TRIVIA_LOBBY;
  sQuestion = nullptr;
  sScore = 0;
  sStrikes = 0;
  sQIndex = 0;
  sNewHigh = false;
  sRoundComplete = false;
  publishView();
  sModeActive = true;  // published first: the render core never sees "active" without a view
  DBG_PRINTF("[TRIVIA] mode entered (controller %s)\n", sPortalUrl);
}

void exitMode() {
  if (!sModeActive) return;
  sModeActive = false;
  sPhase = TRIVIA_LOBBY;
  sQuestion = nullptr;
  // Players' tokens die with the session: their pages get ok:false and
  // fall back to the sign-up form for the next party.
  sMpActive = false;
  sMpCount = 0;
  sTopCount = 0;
  DBG_PRINTF("[TRIVIA] mode exited\n");
}

uint16_t getHighScore() { return sHighScore; }

const char* getHighScoreName() { return sHighName; }

// ---- web helpers (called from the network service route wrappers) ----

bool actionStart() {
  if (!sBegun) return false;
  if (sMpActive) return false;  // a party session owns the engine
  if (sPhase == TRIVIA_QUESTION || sPhase == TRIVIA_REVEAL) return true;
  sScore = 0;
  sStrikes = 0;
  sQIndex = 0;
  sNewHigh = false;
  sRoundComplete = false;
  sLastCorrect = false;
  sLastPoints = 0;
  for (uint8_t i = 0; i < TRIVIA_ROUND_QUESTIONS; ++i) sUsed[i] = nullptr;
  startQuestion(millis());
  return true;
}

bool actionAnswer(uint8_t index) {
  if (sMpActive) return false;  // players answer through mpAnswer()
  if (sPhase != TRIVIA_QUESTION || sLocked != TRIVIA_NO_LOCK) return false;
  if (index > 3) return false;
  uint32_t now = millis();
  uint32_t rem = (int32_t)(sQEndMs - now) > 0 ? (sQEndMs - now) : 0;
  sLocked = index;
  sLockedValid = true;
  sLockedRemainingMs = rem;
  DBG_PRINTF("[TRIVIA] locked %c with %lu ms left\n", 'A' + index,
             (unsigned long)rem);
  publishView();
  return true;
}

bool actionName(const char* name) {
  if (name == nullptr) return false;
  // Keep it short, printable, and safe for the canvas + portal pages.
  char cleaned[TRIVIA_NAME_MAX + 1] = {};
  if (!cleanTriviaName(name, cleaned, sizeof(cleaned))) return false;
  strlcpy(sHighName, cleaned, sizeof(sHighName));
  Preferences prefs;
  prefs.begin("trivia", false);
  prefs.putString("hname", sHighName);
  prefs.end();
  DBG_PRINTF("[TRIVIA] record named: %s\n", sHighName);
  publishView();
  return true;
}

String buildStateJson() {
  uint32_t now = millis();
  String j = "{\"active\":";
  j += sModeActive ? "true" : "false";
  j += ",\"mp\":";
  j += sMpActive ? "true" : "false";
  j += ",\"phase\":\"";
  j += phaseName(sPhase);
  j += "\",\"qi\":";
  j += (int)sQIndex;
  j += ",\"ql\":";
  j += (int)(sMpActive ? sMpQuestions : TRIVIA_ROUND_QUESTIONS);
  j += ",\"diff\":";
  j += sQuestion != nullptr ? (int)sQuestion->difficulty : 1;
  if (sPhase == TRIVIA_QUESTION || sPhase == TRIVIA_REVEAL) {
    j += ",\"q\":\"";
    j += jsonEscape(sQuestion != nullptr ? sQuestion->question : "");
    j += "\",\"ans\":[";
    for (int i = 0; i < 4; ++i) {
      if (i > 0) j += ",";
      j += "\"";
      j += jsonEscape(sQuestion != nullptr ? sQuestion->answers[i] : "");
      j += "\"";
    }
    j += "]";
  } else {
    j += ",\"q\":\"\",\"ans\":[\"\",\"\",\"\",\"\"]";
  }
  j += ",\"locked\":";
  j += (int)sLocked;
  j += ",\"correct\":";
  j += (sPhase == TRIVIA_REVEAL || sPhase == TRIVIA_GAME_OVER)
           ? (int)sCorrect
           : -1;
  j += ",\"res\":";
  j += sLastCorrect ? "true" : "false";
  j += ",\"pts\":";
  j += (int)sLastPoints;
  j += ",\"score\":";
  j += (int)sScore;
  j += ",\"strikes\":";
  j += (int)sStrikes;
  j += ",\"remMs\":";
  if (sPhase == TRIVIA_QUESTION) {
    int32_t rem = (int32_t)(sQEndMs - now);
    if (rem < 0) rem = 0;
    j += (long)rem;
  } else {
    j += "0";
  }
  j += ",\"roundDone\":";
  j += sRoundComplete ? "true" : "false";
  j += ",\"newHigh\":";
  j += sNewHigh ? "true" : "false";
  j += ",\"high\":";
  j += (int)sHighScore;
  j += ",\"highName\":\"";
  j += jsonEscape(sHighName);
  j += "\"}";
  return j;
}

String buildPageHtml() {
  String page = R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>@@NAME@@ Trivia</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:0 0 2px}.muted{color:#c5d3ee;font-size:13px}
.card{background:#0b2b62;border:1px solid #1c4587;border-radius:8px;padding:12px;margin:10px 0}
button{border:0;border-radius:6px;font-weight:700;cursor:pointer}
.ans{display:block;width:100%;text-align:left;padding:12px;margin:8px 0;font-size:17px;background:#0b2b62;color:#fff;border:1px solid #1c4587;box-sizing:border-box}
.ans:disabled{opacity:1}
.ans .k{display:inline-block;width:26px;height:26px;line-height:26px;text-align:center;background:#1c4587;border-radius:4px;margin-right:8px;font-size:15px}
.ans.lock{background:#f5c400;color:#000}.ans.lock .k{background:#000;color:#f5c400}
.ans.ok{background:#2e7d32;color:#fff}.ans.ok .k{background:#fff;color:#2e7d32}
.ans.bad{background:#c62828;color:#fff}.ans.bad .k{background:#fff;color:#c62828}
.go{width:100%;padding:14px;background:#f5c400;color:#000;font-size:18px}
.exit{width:100%;padding:12px;background:#777;color:#fff;margin-top:14px}
#tbar{height:10px;background:#12295c;border-radius:5px;overflow:hidden;margin:8px 0}
#tfill{height:100%;width:100%;background:#f5c400}
#tfill.low{background:#c62828}
#res{font-size:20px;font-weight:800;text-align:center;margin:6px 0;min-height:26px}
#sc{font-size:20px;font-weight:800;text-align:center}
#otitle{font-size:26px;font-weight:800;text-align:center}
#oscore{font-size:40px;font-weight:800;text-align:center;margin:8px 0}
input{padding:10px;font-size:16px;border:0;border-radius:6px;width:58%;box-sizing:border-box}
</style></head><body><main>
<h1>@@NAME@@ Trivia</h1>
<p class="muted" id="hi">High score: --</p>
<div id="lobby" class="card">
<p>15 questions, 10 seconds each. Answer fast for up to 100 points a question &mdash; it gets harder as you go. Three wrong answers and you're benched.</p>
<button class="go" onclick="start()">Solo Play</button>
</div>
<div id="game" style="display:none">
<div class="muted" id="qmeta">Q1 &middot; Easy</div>
<div id="qtext" class="card" style="font-size:18px;font-weight:600">Loading...</div>
<div id="tbar"><div id="tfill"></div></div>
<button class="ans" id="a0" onclick="ans(0)"><span class="k">A</span><span id="t0"></span></button>
<button class="ans" id="a1" onclick="ans(1)"><span class="k">B</span><span id="t1"></span></button>
<button class="ans" id="a2" onclick="ans(2)"><span class="k">C</span><span id="t2"></span></button>
<button class="ans" id="a3" onclick="ans(3)"><span class="k">D</span><span id="t3"></span></button>
<div id="res"></div>
<div id="sc">Score 0</div>
</div>
<div id="over" class="card" style="display:none">
<div id="otitle">GAME OVER</div>
<div id="oscore">0</div>
<div id="ohi" class="muted" style="text-align:center"></div>
<div id="nameRow" style="display:none;margin:10px 0">
<input id="namein" maxlength="12" placeholder="Your name">
<button style="padding:10px 12px;background:#f5c400;color:#000" onclick="saveName()">Save</button>
</div>
<button class="go" onclick="start()">Play Again</button>
</div>
<button class="exit" onclick="exitTrivia()">Exit Trivia Mode</button>
</main><script>
var s={phase:'lobby'},remMs=0;
function el(i){return document.getElementById(i)}
function apply(d){s=d;remMs=d.remMs;paint()}
function paint(){
el('hi').textContent=s.high>0?('High score: '+s.high+(s.highName?' \u2014 '+s.highName:'')):'High score: none yet';
el('lobby').style.display=s.phase=='lobby'?'block':'none';
el('game').style.display=(s.phase=='question'||s.phase=='reveal')?'block':'none';
el('over').style.display=s.phase=='over'?'block':'none';
if(s.phase=='question'||s.phase=='reveal'){
el('qmeta').textContent='Q'+(s.qi+1)+'/'+s.ql+' \u00b7 '+['','Easy','Medium','Hard'][s.diff];
el('qtext').textContent=s.q;
for(var i=0;i<4;i++){var b=el('a'+i);b.className='ans';b.disabled=(s.locked!=255||s.phase=='reveal');el('t'+i).textContent=s.ans[i];
if(s.phase=='question'&&s.locked==i)b.className='ans lock';
if(s.phase=='reveal'){if(i==s.correct)b.className='ans ok';else if(i==s.locked)b.className='ans bad'}}
if(s.phase=='reveal'){el('res').textContent=s.res?('Correct! +'+s.pts+' pts'):(s.locked==255?'Time up!':'Wrong \u2014 answer: '+String.fromCharCode(65+s.correct));
el('res').style.color=s.res?'#7ddc7d':'#ff8a80';el('tfill').style.width='0%';el('tfill').className='';}
else{el('res').textContent=''}
el('sc').textContent='Score '+s.score;
}
if(s.phase=='over'){
el('otitle').textContent=s.roundDone?'ROUND COMPLETE!':'GAME OVER';
el('oscore').textContent=s.score;
el('ohi').textContent=s.newHigh?'NEW HIGH SCORE!':('Best: '+s.high+(s.highName?' \u2014 '+s.highName:''));
el('nameRow').style.display=(s.newHigh&&!s.highName)?'block':'none';
}
}
function sync(){fetch('/trivia/state').then(function(r){return r.json()}).then(apply).catch(function(){})}
function start(){fetch('/trivia/start',{method:'POST'}).then(function(r){return r.json()}).then(apply).catch(function(){})}
function ans(i){if(s.phase!='question'||s.locked!=255)return;
fetch('/trivia/answer',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'a='+i}).then(function(r){return r.json()}).then(apply).catch(function(){})}
function saveName(){var n=el('namein').value.trim();if(!n)return;
fetch('/trivia/name',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(n)}).then(function(r){return r.json()}).then(apply).catch(function(){})}
function exitTrivia(){fetch('/trivia/exit',{method:'POST'}).then(function(){location.href='/'}).catch(function(){location.href='/'})}
setInterval(function(){
if(s.phase=='question'&&remMs>0){remMs-=100;if(remMs<0)remMs=0;
el('tfill').style.width=Math.round(remMs/100)+'%';el('tfill').className=remMs<3000?'low':'';}
},100);
setInterval(sync,500);
sync();
</script></body></html>)html";
  page.replace("@@NAME@@", sDeviceName);
  return page;
}

// ---- multiplayer party mode ----

String joinUrl() {
  String ip = getDeviceIp();
  if (ip.length() == 0) return String("");
  return String("http://") + ip + "/play";
}

bool mpHostLobby() {
  if (!sBegun) return false;
  // Fresh session: wipe any solo round or previous party, open the lobby.
  sScore = 0;
  sStrikes = 0;
  sQIndex = 0;
  sNewHigh = false;
  sRoundComplete = false;
  sLastCorrect = false;
  sLastPoints = 0;
  sFireworks = false;
  for (uint8_t i = 0; i < TRIVIA_ROUND_QUESTIONS; ++i) sUsed[i] = nullptr;
  sMpCount = 0;
  sTopCount = 0;
  sMpCorrectCount = 0;
  sMpActive = true;
  sPhase = TRIVIA_LOBBY;
  sQuestion = nullptr;
  // The lobby QR points players at the join page (IP can change between
  // sessions; this runs on the network task, so the radio call is safe).
  strlcpy(sPortalUrl, joinUrl().c_str(), sizeof(sPortalUrl));
  publishView();
  sModeActive = true;  // published first, like enterMode()
  DBG_PRINTF("[TRIVIA] MP lobby open (join %s)\n", sPortalUrl);
  return true;
}

bool mpSessionActive() { return sMpActive; }

uint16_t mpJoin(const char* name) {
  if (!sMpActive || sPhase != TRIVIA_LOBBY) return 0;  // joining closed
  if (sMpCount >= TRIVIA_MP_MAX_PLAYERS) return 0;
  char cleaned[TRIVIA_NAME_MAX + 1] = {};
  if (!cleanTriviaName(name, cleaned, sizeof(cleaned))) return 0;
  for (uint8_t i = 0; i < sMpCount; ++i) {
    if (strcasecmp(sPlayers[i].name, cleaned) == 0) return 0;  // taken
  }
  MpPlayer& p = sPlayers[sMpCount];
  p.token = sMpNextToken++;
  if (sMpNextToken == 0) sMpNextToken = 1;  // 0 is the "no token" marker
  strlcpy(p.name, cleaned, sizeof(p.name));
  p.score = 0;
  p.locked = TRIVIA_NO_LOCK;
  p.lockedRemainMs = 0;
  p.lastCorrect = false;
  p.lastPoints = 0;
  p.rank = 0;
  ++sMpCount;
  publishView();  // lobby card shows the new count
  DBG_PRINTF("[TRIVIA] MP join: %s (%u/%u)\n", p.name, sMpCount,
             (unsigned)TRIVIA_MP_MAX_PLAYERS);
  return p.token;
}

bool mpSetConfig(uint8_t questions, uint8_t seconds) {
  if (!sMpActive || sPhase != TRIVIA_LOBBY) return false;
  if (questions < TRIVIA_MP_MIN_QUESTIONS) questions = TRIVIA_MP_MIN_QUESTIONS;
  if (questions > TRIVIA_MP_MAX_QUESTIONS) questions = TRIVIA_MP_MAX_QUESTIONS;
  if (seconds < TRIVIA_MP_MIN_SECONDS) seconds = TRIVIA_MP_MIN_SECONDS;
  if (seconds > TRIVIA_MP_MAX_SECONDS) seconds = TRIVIA_MP_MAX_SECONDS;
  sMpQuestions = questions;
  sMpSeconds = seconds;
  sMpWindowMs = (uint32_t)seconds * 1000;
  DBG_PRINTF("[TRIVIA] MP config: %u questions, %u s each\n", questions,
             seconds);
  return true;
}

bool mpStart() {
  if (!sMpActive || sPhase != TRIVIA_LOBBY || sMpCount == 0) return false;
  startQuestion(millis());  // leaves LOBBY: mpJoin() closes the door
  return true;
}

bool mpAnswer(uint16_t token, uint8_t index) {
  if (!sMpActive || sPhase != TRIVIA_QUESTION || index > 3) return false;
  MpPlayer* p = nullptr;
  for (uint8_t i = 0; i < sMpCount; ++i) {
    if (sPlayers[i].token == token) {
      p = &sPlayers[i];
      break;
    }
  }
  if (p == nullptr || p->locked != TRIVIA_NO_LOCK) return false;
  uint32_t now = millis();
  uint32_t rem = (int32_t)(sQEndMs - now) > 0 ? (sQEndMs - now) : 0;
  p->locked = index;
  p->lockedRemainMs = rem;
  DBG_PRINTF("[TRIVIA] MP %s locked %c (%lu ms left)\n", p->name,
             'A' + index, (unsigned long)rem);
  return true;  // no publishView: the device view doesn't change per lock
}

String buildMpHostJson() {
  String j = "{\"active\":";
  j += sModeActive ? "true" : "false";
  j += ",\"mp\":";
  j += sMpActive ? "true" : "false";
  j += ",\"url\":\"";
  j += jsonEscape(joinUrl().c_str());
  j += "\"";
  if (sMpActive) {
    j += ",\"phase\":\"";
    j += phaseName(sPhase);
    j += "\",\"qi\":";
    j += (int)sQIndex;
    j += ",\"count\":";
    j += (int)sMpCount;
    j += ",\"max\":";
    j += (int)TRIVIA_MP_MAX_PLAYERS;
    j += ",\"questions\":";
    j += (int)sMpQuestions;
    j += ",\"seconds\":";
    j += (int)sMpSeconds;
    j += ",\"players\":[";
    for (uint8_t i = 0; i < sMpCount; ++i) {
      if (i > 0) j += ",";
      j += "{\"n\":\"";
      j += jsonEscape(sPlayers[i].name);
      j += "\",\"s\":";
      j += (int)sPlayers[i].score;
      j += "}";
    }
    j += "]";
    if (sPhase == TRIVIA_GAME_OVER && sTopCount > 0) {
      j += ",\"winner\":\"";
      j += jsonEscape(sTop[0].name);
      j += "\"";
    }
  }
  j += "}";
  return j;
}

String buildMpPlayerJson(uint16_t token) {
  MpPlayer* p = nullptr;
  for (uint8_t i = 0; i < sMpCount; ++i) {
    if (sPlayers[i].token == token) {
      p = &sPlayers[i];
      break;
    }
  }
  if (!sMpActive || p == nullptr) return String("{\"ok\":false}");
  uint32_t now = millis();
  String j = "{\"ok\":true,\"phase\":\"";
  j += phaseName(sPhase);
  j += "\",\"qi\":";
  j += (int)sQIndex;
  j += ",\"ql\":";
  j += (int)sMpQuestions;
  j += ",\"diff\":";
  j += sQuestion != nullptr ? (int)sQuestion->difficulty : 1;
  j += ",\"n\":";
  j += (int)sMpCount;
  j += ",\"name\":\"";
  j += jsonEscape(p->name);
  j += "\",\"q\":\"";
  if (sPhase == TRIVIA_QUESTION || sPhase == TRIVIA_REVEAL) {
    j += jsonEscape(sQuestion != nullptr ? sQuestion->question : "");
    j += "\",\"ans\":[";
    for (int i = 0; i < 4; ++i) {
      if (i > 0) j += ",";
      j += "\"";
      j += jsonEscape(sQuestion != nullptr ? sQuestion->answers[i] : "");
      j += "\"";
    }
    j += "]";
  } else {
    j += "\",\"ans\":[\"\",\"\",\"\",\"\"]";
  }
  j += ",\"locked\":";
  j += (int)p->locked;
  j += ",\"correct\":";
  bool revealed = sPhase == TRIVIA_REVEAL || sPhase == TRIVIA_RANKINGS ||
                  sPhase == TRIVIA_GAME_OVER;
  j += revealed ? (int)sCorrect : -1;
  j += ",\"res\":";
  j += p->lastCorrect ? "true" : "false";
  j += ",\"pts\":";
  j += (int)p->lastPoints;
  j += ",\"score\":";
  j += (int)p->score;
  j += ",\"rank\":";
  j += (int)p->rank;
  j += ",\"remMs\":";
  if (sPhase == TRIVIA_QUESTION) {
    int32_t rem = (int32_t)(sQEndMs - now);
    if (rem < 0) rem = 0;
    j += (long)rem;
  } else {
    j += "0";
  }
  if (sPhase == TRIVIA_RANKINGS || sPhase == TRIVIA_GAME_OVER) {
    j += ",\"board\":[";
    for (uint8_t i = 0; i < sTopCount; ++i) {
      if (i > 0) j += ",";
      j += "{\"n\":\"";
      j += jsonEscape(sTop[i].name);
      j += "\",\"s\":";
      j += (int)sTop[i].score;
      j += "}";
    }
    j += "]";
    if (sPhase == TRIVIA_GAME_OVER && sTopCount > 0) {
      j += ",\"winner\":\"";
      j += jsonEscape(sTop[0].name);
      j += "\"";
    }
  }
  j += "}";
  return j;
}

String buildMpJoinHtml() {
  String page = R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>@@NAME@@ Trivia</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:0 0 2px}.muted{color:#c5d3ee;font-size:13px}
.card{background:#0b2b62;border:1px solid #1c4587;border-radius:8px;padding:12px;margin:10px 0}
button{border:0;border-radius:6px;font-weight:700;cursor:pointer}
.ans{display:block;width:100%;text-align:left;padding:12px;margin:8px 0;font-size:17px;background:#0b2b62;color:#fff;border:1px solid #1c4587;box-sizing:border-box}
.ans:disabled{opacity:1}
.ans .k{display:inline-block;width:26px;height:26px;line-height:26px;text-align:center;background:#1c4587;border-radius:4px;margin-right:8px;font-size:15px}
.ans.lock{background:#f5c400;color:#000}.ans.lock .k{background:#000;color:#f5c400}
.ans.ok{background:#2e7d32;color:#fff}.ans.ok .k{background:#fff;color:#2e7d32}
.ans.bad{background:#c62828;color:#fff}.ans.bad .k{background:#fff;color:#c62828}
.go{width:100%;padding:14px;background:#f5c400;color:#000;font-size:18px}
#tbar{height:10px;background:#12295c;border-radius:5px;overflow:hidden;margin:8px 0}
#tfill{height:100%;width:100%;background:#f5c400}
#tfill.low{background:#c62828}
#res{font-size:20px;font-weight:800;text-align:center;margin:6px 0;min-height:26px}
#sc{font-size:18px;font-weight:800;text-align:center}
input{padding:10px;font-size:16px;border:0;border-radius:6px;width:60%;box-sizing:border-box}
.row{display:flex;justify-content:space-between;padding:4px 0;font-size:16px}
.row .rk{width:28px;color:#c5d3ee}.row.you{color:#f5c400;font-weight:800}
#owin{font-size:24px;font-weight:800;text-align:center;color:#f5c400;min-height:30px}
#ofinal{text-align:center;margin:8px 0}
</style></head><body><main>
<h1>@@NAME@@ Trivia</h1>
<div id="join" class="card">
<p>Enter your name to join the game on the scoreboard.</p>
<input id="namein" maxlength="12" placeholder="Your name" autocomplete="off">
<button class="go" style="width:auto;padding:10px 16px" onclick="doJoin()">Join Game</button>
<p class="muted" id="jmsg"></p>
</div>
<div id="wait" class="card" style="display:none">
<p style="font-size:18px">You're in as <b id="pname"></b>!</p>
<p class="muted" id="wcnt"></p>
<p class="muted" id="whint">Waiting for the host to start the game...</p>
</div>
<div id="game" style="display:none">
<div class="muted" id="qmeta"></div>
<div id="qtext" class="card" style="font-size:18px;font-weight:600"></div>
<div id="tbar"><div id="tfill"></div></div>
<button class="ans" id="a0" onclick="ans(0)"><span class="k">A</span><span id="t0"></span></button>
<button class="ans" id="a1" onclick="ans(1)"><span class="k">B</span><span id="t1"></span></button>
<button class="ans" id="a2" onclick="ans(2)"><span class="k">C</span><span id="t2"></span></button>
<button class="ans" id="a3" onclick="ans(3)"><span class="k">D</span><span id="t3"></span></button>
<div id="res"></div>
<div id="sc"></div>
</div>
<div id="board" class="card" style="display:none">
<div style="font-weight:800;margin-bottom:6px">Standings</div>
<div id="brows"></div>
</div>
<div id="over" class="card" style="display:none">
<div id="owin"></div>
<div id="ofinal" class="muted"></div>
<div id="obrows"></div>
</div>
</main><script>
var tok=localStorage.getItem('sbTriviaTok')||'',s=null,remMs=0;
function el(i){return document.getElementById(i)}
function rows(b){var h='';if(b)for(var i=0;i<b.length;i++){var r=b[i];
h+='<div class="row'+(s&&r.n==s.name?' you':'')+'"><span class="rk">'+(i+1)+'</span><span>'+r.n+'</span><span>'+r.s+'</span></div>'}return h}
function apply(d){
if(!d.ok){tok='';localStorage.removeItem('sbTriviaTok');s=null;paint();
el('jmsg').textContent='Sign-up closed \u2014 ask the host to open the game.';return}
s=d;remMs=d.remMs;paint()}
function paint(){
if(!s){el('join').style.display='block'}else{el('join').style.display='none'}
var ph=s?s.phase:'';
el('wait').style.display=(ph=='lobby')?'block':'none';
el('game').style.display=(ph=='question'||ph=='reveal')?'block':'none';
el('board').style.display=(ph=='rankings')?'block':'none';
el('over').style.display=(ph=='over')?'block':'none';
if(!s)return;
if(ph=='lobby'){el('pname').textContent=s.name;
el('wcnt').textContent=s.n+(s.n==1?' player':' players')+' joined';
el('whint').textContent='Waiting for the host to start the game...'}
if(ph=='question'||ph=='reveal'){
el('qmeta').textContent='Q'+(s.qi+1)+'/'+s.ql+' \u00b7 '+['','Easy','Medium','Hard'][s.diff];
el('qtext').textContent=s.q;
for(var i=0;i<4;i++){var b=el('a'+i);b.className='ans';b.disabled=(s.locked!=255||ph=='reveal');el('t'+i).textContent=s.ans[i];
if(ph=='question'&&s.locked==i)b.className='ans lock';
if(ph=='reveal'){if(i==s.correct)b.className='ans ok';else if(i==s.locked)b.className='ans bad'}}
if(ph=='reveal'){el('res').textContent=s.res?('Correct! +'+s.pts+' pts'):(s.locked==255?'Time up!':'Wrong \u2014 answer: '+String.fromCharCode(65+s.correct));
el('res').style.color=s.res?'#7ddc7d':'#ff8a80';el('tfill').style.width='0%';el('tfill').className='';}
else{el('res').textContent=''}
el('sc').textContent='Score '+s.score+(s.rank>0?' \u00b7 rank '+s.rank:'');}
if(ph=='rankings'){el('brows').innerHTML=rows(s.board)}
if(ph=='over'){el('owin').textContent=s.winner?('WINNER: '+s.winner):'GAME OVER';
el('ofinal').textContent='You finished rank '+s.rank+' with '+s.score+' pts';
el('obrows').innerHTML=rows(s.board)}
}
function sync(){if(!tok)return;
fetch('/play/state?p='+tok).then(function(r){return r.json()}).then(apply).catch(function(){})}
function doJoin(){var n=el('namein').value.trim();if(!n){el('jmsg').textContent='Enter a name first.';return}
el('jmsg').textContent='';
fetch('/play/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(n)})
.then(function(r){return r.json()}).then(function(d){
if(d.ok){tok=''+d.token;localStorage.setItem('sbTriviaTok',tok);apply(d.state)}
else{el('jmsg').textContent=d.err=='taken'?'That name is taken \u2014 pick another.':(d.err=='full'?'The game is full.':(d.err=='closed'?'Sign-up is closed \u2014 the game already started.':'Could not join \u2014 try again.'))}})
.catch(function(){el('jmsg').textContent='Could not reach the scoreboard.'})}
function ans(i){if(!s||s.phase!='question'||s.locked!=255)return;
fetch('/play/answer?p='+tok,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'a='+i})
.then(function(r){return r.json()}).then(apply).catch(function(){})}
setInterval(function(){
if(s&&s.phase=='question'&&remMs>0){remMs-=100;if(remMs<0)remMs=0;
el('tfill').style.width=Math.round(remMs/100)+'%';el('tfill').className=remMs<3000?'low':'';}
},100);
setInterval(sync,1000);
sync();
</script></body></html>)html";
  page.replace("@@NAME@@", sDeviceName);
  return page;
}

}  // namespace trivia
