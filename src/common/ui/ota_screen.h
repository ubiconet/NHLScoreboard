#pragma once

#include <Arduino.h>

// Firmware self-update UI (driven by the OTA updater on core 0, drawn by
// handleOtaUpdateScreen() on the render loop). Only POD + a small string
// cross the boundary, same pattern as the feed snapshots.

enum class OtaStage : uint8_t {
  NONE = 0,
  DOWNLOADING,   // binary is streaming to flash (progress 0..100)
  REBOOTING,     // image verified, device about to restart
  FAILED,        // download/flash failed; normal operation resumes
};

void publishOtaStage(OtaStage stage, int progressPct);  // core-0 updater
OtaStage getOtaStage();
int getOtaProgress();
void setOtaTargetVersion(const char* version);
// Draws the "updating — do not turn off" screen while an update is in
// progress. Returns true when the OTA UI owns this frame so the caller
// (loop()) should skip normal rendering.
bool handleOtaUpdateScreen();
