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

}  // namespace power
