#pragma once

#include <Arduino.h>

// Power gate — the low-battery defense for the LiPo + boost supply (see the
// battery section of the sport's sport_config.h). A board-version V1.1
// feature ONLY: when the portal's board-version setting selects a board
// without the sense divider, the whole gate is disabled — no voltage check,
// no boot journal — and every boot proceeds normally. On V1.1 boards two
// detection paths share one outcome (flashed warning screen + deep sleep):
//   voltage:  the divider on BATTERY_SENSE_PIN is read at power-on, before
//             the displays light and before Wi-Fi starts. At/below
//             BATTERY_LOW_VOLTS the board flashes the warning and sleeps
//             immediately, before any cycling can start. While asleep it
//             silently re-checks every BATTERY_RECHECK_MS and resumes on
//             its own once the charger lifts the pack past
//             BATTERY_RESUME_VOLTS.
//   backstop: an NVS boot journal counts rapid bad boots (brownout resets
//             / sessions that never reached BATTERY_HEALTHY_BOOT_MS of
//             uptime); a streak of BATTERY_BAD_BOOT_STREAK trips the same
//             path. Recovery then needs a power cycle after charging.
namespace power {

enum class BootDecision {
  NORMAL,    // proceed with the boot
  RESLEEP,   // timer-wake recheck found the pack still low: stay asleep
  SHUTDOWN,  // low battery: flash the warning and shut down
};

// Runs at the very top of setup(), before the display or Wi-Fi come up.
// Cheap and deliberately first: during a low-battery boot storm some cycles
// die within seconds, and the journal/battery reading must happen even then.
BootDecision earlyBootCheck();

// Enter (or re-enter) the low-battery deep sleep. Never returns.
void sleepNow();

// Flash the low-battery warning on the TFT, blank the matrices, the TM1637
// clock, and the penalty LEDs, then deep-sleep. Never returns. The display
// must already be initialized (call after sport::setup()).
void showLowBatteryAndSleep();

// Call every loop() pass: once the session passes BATTERY_HEALTHY_BOOT_MS
// of uptime it marks the boot journal healthy, so a later normal power-off
// doesn't count toward the bad-boot streak.
void noteHealthyUptime();

// ---- Runtime battery gauge (V1.1 boards only) ----
// The sense pin sits on ADC1 (Wi-Fi never claims it), so the pack voltage
// stays readable while the board runs. serviceBatteryMonitor() paces
// itself to BATTERY_MONITOR_INTERVAL_MS — call it from the core-0 data
// task loop; it reads nothing on boards without the divider.
void serviceBatteryMonitor();

// Last runtime sample. 0.0 when unknown (no divider / no sample yet).
float batteryVolts();

// Coarse charge level for the on-screen gauge: 0..4 quarter segments, or
// 255 when unknown — callers hide the indicator then, so boards without
// the divider never show a gauge.
uint8_t batteryQuarters();

// Estimated remaining charge, 0..100 percent, linear over the smoothed
// pack voltage between BATTERY_LOW_VOLTS (empty) and BATTERY_FULL_VOLTS
// (full, 4.2 V and above); 255 = unknown. Voltage-only estimate:
// conservative under load (the boost converter sags the pack).
uint8_t batteryPercent();

}  // namespace power
