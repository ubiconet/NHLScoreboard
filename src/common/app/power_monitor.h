#pragma once

#include <Arduino.h>

// Battery monitor — the runtime pack gauge for the LiPo + boost supply
// (constants in the sport's sport_config.h). Battery sensing is a
// board-version V1.1 HARDWARE feature ONLY: when the portal's
// board-version setting selects a board without the sense divider on
// BATTERY_SENSE_PIN, nothing here reads the pin — no gauge, no voltage
// anywhere. There is deliberately NO low-battery power gate anymore
// (removed v3.79): the board never shuts itself down or deep-sleeps on a
// low pack, on any board version — power control is the physical switch.
// serviceBatteryMonitor() samples the divider every
// BATTERY_MONITOR_INTERVAL_MS; call it from the core-0 data task loop.
namespace power {

// Sample the pack and update the runtime gauge. No-op on boards without
// the V1.1 sense divider.
void serviceBatteryMonitor();

// Last runtime sample. 0.0 when unknown (no divider / no sample yet).
float batteryVolts();

// Coarse charge level for the on-screen gauge: 0..4 quarter segments, or
// 255 when unknown — callers hide the indicator then, so boards without
// the divider never show a gauge.
uint8_t batteryQuarters();

// Estimated remaining charge, 0..100 percent, linear over the smoothed
// pack voltage between BATTERY_LOW_VOLTS (empty) and BATTERY_FULL_VOLTS
// (full — the charger's 4.18 V termination reads 100%); 255 = unknown.
// Voltage-only estimate: conservative under load (the boost converter
// sags the pack).
uint8_t batteryPercent();

}  // namespace power
