# Trivia Mode

A hockey trivia game show on the scoreboard, played from the phone (v3.60+).
Open the device's setup portal and press **Open Trivia Mode** (or go
straight to `http://<device-ip>/trivia`).

## How a round plays

- **15 questions per round**, difficulty ramps: 5 easy → 5 medium → 5 hard,
  drawn randomly from a 750-question bank (`src/sports/nhl/trivia_questions_*.h`,
  250 per difficulty) with no repeats within a round.
- Each question has a **10-second window**. The device screen shows the
  question and the four options Millionaire-style; the phone is the
  controller — tap **A / B / C / D** any time inside the window. The pick
  locks in immediately and lights up gold on both screens.
- When the clock runs out, the correct answer is revealed (green; a wrong
  lock shows red) and the question is scored: **up to 100 points, the
  faster the lock the higher the score** (timeout = 0 points).
- Each wrong answer (or timeout) lights one **penalty LED** — the same
  home/guest penalty lights the live game uses. The third wrong answer is
  **game over**. Survive all 15 and the round is completed.
- Beat the stored record and the screens show **NEW HIGH SCORE!**; the web
  page asks for a name (kept in NVS: score + 12-char name) and the settings
  menu shows the current record.
- **Exit Trivia Mode** is pinned to the bottom of the controller page at
  all times; exiting returns the scoreboard to its normal display
  (waiting screen or live game, whichever was showing).

## Where things live

| Piece | File |
|---|---|
| Game engine (state machine, scoring, NVS record, web page/JSON) | `src/common/trivia/trivia_engine.{h,cpp}` |
| On-device screen (TFT + matrices + TM1637 + penalty LEDs) | `src/common/trivia/trivia_screen.cpp` |
| Question bank (content is the sport's; engine is generic) | `src/sports/nhl/trivia_questions_{easy,medium,hard}.h` |
| HTTP routes + settings-menu section | `src/common/comms/network_service.cpp` (`/trivia*`) |

## Architecture notes

The engine follows the manual-mode pattern: all game state is owned by the
**network task** (core 0) — the portal route wrappers and `trivia::tick()`
(run from the network task loop) are the only writers, so no locking is
needed. Every state change publishes a `TriviaView` POD snapshot through a
`SnapshotChannel`; the **render core** (inside `sport::tick()`, which
delegates to `trivia::renderScreen()`) only reads snapshots, like it does
for game feeds. The countdown is computed render-side from the published
deadline, so ticking the clock costs no publishes.

Like manual mode and the game demo, trivia is exclusive with them:
arriving at `/manual` or starting the demo exits trivia, and vice versa.
The penalty LEDs double as the three-strike display (group A lamps 1-2 +
group B lamp 1); the score matrices show `score / question # / difficulty`,
and the TM1637 mirrors the countdown.

Adding trivia content for a new sport: keep the engine as-is and replace
the three `trivia_questions_*.h` files (see `TriviaQuestion` in
`common/trivia/trivia_engine.h`; questions ≤ ~90 chars, answers ≤ 22 chars,
`difficulty` must match the pool file, `correctIndex` 0-3).
`tools/audit_trivia.py` validates counts, budgets, and answer uniqueness.
