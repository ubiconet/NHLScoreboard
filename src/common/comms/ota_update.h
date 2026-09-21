#pragma once

#include <Arduino.h>

// Firmware self-update against the releases/manifest.json in the GitHub
// repo (written by `pio run -t deploy`). Runs entirely on the core-0 data
// task: fetch manifest, compare version with FIRMWARE_VERSION, and when
// they differ download + flash the new binary while the renderer shows the
// "updating, do not turn off" screen (see handleOtaUpdateScreen()).
// onlineForMs = milliseconds since the network last came online (0 while
// offline); the first check fires shortly after association.
void serviceOtaUpdates(uint32_t onlineForMs);

// True while an update check is between the downloading and reboot stages —
// the data task skips its normal feed fetches in that window so the TLS
// download never competes for heap or airtime.
bool otaUpdateInProgress();

// False during the first moments online until the boot OTA check has run:
// the TLS handshake needs the still-pristine heap, so the feed fetches
// (which fragment it with JSON pools) must wait. Turns true after the
// check, or after OTA_BOOT_GATE_TIMEOUT_MS as a fallback.
bool otaBootGateReached(uint32_t onlineForMs);

// Ask the data task to run an update check on its next loop pass,
// bypassing the retry throttle (the setup portal's "Check for update
// now" button). Mid-session checks run with the feed buffers released to
// give the TLS handshake the largest contiguous heap available.
void requestOtaCheckNow();
bool otaCheckRequested();      // true while a request is pending
bool otaEverChecked();         // at least one check completed
bool otaLastCheckOk();         // last check reached and parsed the manifest
