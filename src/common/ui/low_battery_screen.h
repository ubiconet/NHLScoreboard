#pragma once

#include <Arduino.h>

// Low-battery warning screen (the power gate's UI): a battery icon whose
// charge segment flashes red, the pack voltage when known, and a
// charge-to-resume hint. Blocks for holdMs (icon flash included), then
// returns — the power module puts the device into deep sleep right after.
// volts <= 0 hides the voltage line (no divider wired).
void renderLowBatteryWarning(float volts, uint32_t holdMs);
