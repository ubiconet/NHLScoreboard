#pragma once

#include <Arduino.h>

// SnapshotChannel — the lock-free cross-core mailbox every feed uses.
//
// One writer (the core-0 data task) calls publish(); any number of readers
// on the render core call take() with their own "last seen" generation
// counter. The payload is a POD struct copied in one assignment; a full
// memory barrier between payload write and generation bump guarantees the
// reader never sees a new generation with a half-written struct. No locks,
// no allocation.
//
// Requirement on T: must be a POD/struct copyable by assignment and must
// expose a `valid` field — take() returns out.valid so callers can ignore
// not-yet-populated payloads (a fresh-but-empty snapshot still advances the
// caller's generation counter and won't be re-delivered).
template <typename T>
class SnapshotChannel {
 public:
  void publish(const T& value) {
    value_ = value;
    __sync_synchronize();
    gen_++;
  }

  uint32_t generation() const { return gen_; }

  // Copies the current snapshot into `out` only if the generation counter
  // has advanced since the caller's last sample. Returns true when a fresh
  // (and valid) payload was delivered.
  bool take(T& out, uint32_t& lastGen) {
    if (gen_ == lastGen) return false;
    out = value_;
    lastGen = gen_;
    return out.valid;
  }

 private:
  T value_{};
  volatile uint32_t gen_ = 0;
};
