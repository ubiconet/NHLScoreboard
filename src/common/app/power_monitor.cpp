#include <Arduino.h>

#include "config.h"
#include "common/app/power_monitor.h"
#include "common/comms/network_service.h"

namespace power {
namespace {

float readBatteryVolts() {
  // Only boards whose hardware carries the divider (V1.1+, per the
  // portal's board-version setting) may read the pin — on anything else
  // it floats and reads like noise.
  if (!batterySenseEnabled(getBoardVersion())) return 0.0f;
  // 11 dB attenuation maps the divider's 0-2.1 V output (half of the pack's
  // 3.0-4.2 V) into the ADC's ~0-3.1 V window. Median-of-5 rejects the boost
  // converter's ripple and single-sample ADC glitches.
  analogSetPinAttenuation(BATTERY_SENSE_PIN, ADC_11db);
  uint32_t samples[5];
  for (int i = 0; i < 5; ++i) {
    samples[i] = analogReadMilliVolts(BATTERY_SENSE_PIN);
    delay(2);
  }
  for (int i = 1; i < 5; ++i) {  // insertion sort, n = 5
    uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }
    samples[j + 1] = key;
  }
  return samples[2] * BATTERY_SENSE_DIVIDER / 1000.0f;
}

// ---- Runtime gauge state (core-0 writer, render-core reader) ----
// Float/uint8 writes are single aligned stores, so the render core can
// sample them without a lock — same discipline as the fetch diagnostics.
float sRuntimeVolts = 0.0f;    // last runtime sample; 0 = unknown
float sSmoothedVolts = 0.0f;   // EMA of samples; 0 = none yet
uint8_t sRuntimeQuarters = 0xFF;
uint8_t sRuntimePercent = 0xFF;
uint32_t sLastMonitorAt = 0;

// State of charge is a straight voltage mapping over the pack's usable
// window (user-specified levels): BATTERY_LOW_VOLTS (3.4 V) = empty,
// BATTERY_FULL_VOLTS (4.18 V and above — the charger's termination
// voltage) = full. Voltage-only fuel gauging is inherently coarse — the
// boost converter's load sags the pack ~0.1-0.2 V (under-reading while
// running) — but the linear scale keeps the gauge predictable and
// matches the quarter steps exactly; a true gauge needs a fuel-gauge IC.
uint8_t voltsToPercent(float v) {
  float span = BATTERY_FULL_VOLTS - BATTERY_LOW_VOLTS;
  float pct = (v - BATTERY_LOW_VOLTS) / span * 100.0f + 0.5f;
  if (pct < 0.0f) return 0;
  if (pct > 100.0f) return 100;
  return (uint8_t)pct;
}

// Quarter segments bucket the SAME percent scale: 4/4 spans 75-100% (a
// completed charge holds "full" until the discharge crosses 75%,
// ~3.99 V), then 3/4 / 2/4 at the 50% / 25% lines (~3.79 / ~3.60 V),
// 1/4 down to the empty floor. An explicit threshold ladder (not a
// divided float) keeps the exact boundaries on the lit side; the icon
// and the percentage switch buckets at the same voltage.
uint8_t voltsToQuarters(float v) {
  const float seg = (BATTERY_FULL_VOLTS - BATTERY_LOW_VOLTS) / 4.0f;
  if (v >= BATTERY_FULL_VOLTS - seg) return 4;        // 75-100%: full
  if (v >= BATTERY_FULL_VOLTS - 2.0f * seg) return 3; // 50-75%
  if (v >= BATTERY_FULL_VOLTS - 3.0f * seg) return 2; // 25-50%
  if (v > BATTERY_LOW_VOLTS) return 1;                // above empty
  return 0;                                           // at/below 3.4 V
}

}  // namespace

void serviceBatteryMonitor() {
  if (!batterySenseEnabled(getBoardVersion())) return;  // no divider
  uint32_t now = millis();
  if (sLastMonitorAt != 0 && now - sLastMonitorAt < BATTERY_MONITOR_INTERVAL_MS) {
    return;
  }
  sLastMonitorAt = now;
  sRuntimeVolts = readBatteryVolts();
  // Plausibility guard: a 1S LiPo never sits above ~4.3 V. A higher
  // reading means the divider taps the boost output (~5 V rail) or the
  // pin floats — hide the gauge rather than show a lying "full".
  // batteryVolts() stays raw so the diagnostics page can display the
  // real number.
  if (sRuntimeVolts > 0.5f && sRuntimeVolts <= 4.30f) {
    // EMA across the 30 s samples steadies the charge estimate; a fast
    // alpha (0.25) still tracks a real discharge over minutes.
    sSmoothedVolts = (sSmoothedVolts <= 0.0f)
                         ? sRuntimeVolts
                         : sSmoothedVolts + 0.25f * (sRuntimeVolts - sSmoothedVolts);
    sRuntimePercent = voltsToPercent(sSmoothedVolts);
    sRuntimeQuarters = voltsToQuarters(sSmoothedVolts);
  } else {
    sSmoothedVolts = 0.0f;  // resync when a trustworthy reading returns
    sRuntimePercent = 0xFF;
    sRuntimeQuarters = 0xFF;
  }
  static uint8_t sLastLoggedQuarters = 0xFE;
  if (sRuntimeQuarters != sLastLoggedQuarters) {
    sLastLoggedQuarters = sRuntimeQuarters;
    if (sRuntimeQuarters <= 4) {
      DBG_PRINTF("[POWER] battery %.2f V -> %u%% (%u/4)\n", sRuntimeVolts,
                 (unsigned)sRuntimePercent, (unsigned)sRuntimeQuarters);
    } else {
      DBG_PRINTF("[POWER] battery %.2f V -> implausible, gauge hidden\n",
                 sRuntimeVolts);
    }
  }
}

float batteryVolts() { return sRuntimeVolts; }
uint8_t batteryQuarters() { return sRuntimeQuarters; }
uint8_t batteryPercent() { return sRuntimePercent; }

}  // namespace power
