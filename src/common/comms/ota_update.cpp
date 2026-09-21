#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "common/comms/http_fetcher.h"
#include "common/ui/ota_screen.h"
#include "ota_update.h"

namespace {

// Download staging buffer. Static (not stack): the data task's 12 KB stack
// can't absorb a multi-KB read buffer alongside the HTTP/TLS frames.
uint8_t sOtaBuf[4096];

bool sCheckedOnce = false;
bool sLastCheckOk = false;
uint32_t sLastCheckAt = 0;
volatile bool sCheckRequested = false;
uint8_t sBootAttempts = 0;
// Two shots in the pristine-heap boot window, no more: each failed TLS
// attempt costs ~15 s (timeout + gap), and holding the feed fetches behind
// six of them starved game data past the boot screens. The 10-minute
// periodic retry picks the update check back up regardless.
const uint8_t OTA_BOOT_MAX_ATTEMPTS = 2;

// True when the manifest version is strictly NEWER than FIRMWARE_VERSION.
// Both are "vMAJOR.MINOR"; anything unparsable is treated as not newer so
// a malformed manifest can never trigger a downgrade loop.
bool manifestIsNewer(const char* manifestVersion) {
  int fwMaj = 0, fwMin = 0, mfMaj = 0, mfMin = 0;
  if (sscanf(FIRMWARE_VERSION, "v%d.%d", &fwMaj, &fwMin) != 2) return false;
  if (sscanf(manifestVersion, "v%d.%d", &mfMaj, &mfMin) != 2) return false;
  return mfMaj > fwMaj || (mfMaj == fwMaj && mfMin > fwMin);
}

// Reads exactly Content-Length bytes of an already-header-parsed response
// into buf (leaving the keep-alive connection clean for the next request).
// Returns the number of bytes read, or 0 on timeout/overflow.
size_t readExactBody(HTTPClient& http, uint8_t* buf, size_t bufLen) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) return 0;
  int remaining = http.getSize();  // -1 when absent
  if (remaining < 0 || (size_t)remaining > bufLen) return 0;
  size_t got = 0;
  uint32_t deadline = millis() + 8000;
  while (got < (size_t)remaining && millis() < deadline) {
    size_t avail = stream->available();
    if (avail == 0) {
      delay(2);
      continue;
    }
    size_t n = stream->readBytes(buf + got, (size_t)remaining - got);
    if (n == 0) continue;
    got += n;
  }
  return got == (size_t)remaining ? got : 0;
}

// Streams the body of an already-successful GET into the flash updater.
// total is the Content-Length (-1 when unknown). Returns true when the
// image was fully written and verified.
bool streamBodyToFlash(HTTPClient& http, int total) {
  publishOtaStage(OtaStage::DOWNLOADING, 0);
  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint32_t lastProgressAt = millis();
  uint32_t lastByteAt = millis();
  for (;;) {
    size_t avail = stream->available();
    if (avail == 0) {
      if ((total > 0 && written >= (size_t)total) || !stream->connected()) {
        break;
      }
      if (millis() - lastByteAt > OTA_DOWNLOAD_STALL_MS) {
        DBG_PRINTF("[OTA] download stalled at %u bytes, aborting\n",
                   (unsigned)written);
        return false;
      }
      delay(5);
      continue;
    }
    size_t chunk = sizeof(sOtaBuf);
    if (chunk > avail) chunk = avail;
    size_t got = stream->readBytes(sOtaBuf, chunk);
    if (got == 0) continue;
    lastByteAt = millis();
    if (Update.write(sOtaBuf, got) != got) {
      DBG_PRINTF("[OTA] flash write failed: %s\n", Update.errorString());
      return false;
    }
    written += got;
    if (total > 0 && millis() - lastProgressAt >= 250) {
      lastProgressAt = millis();
      publishOtaStage(OtaStage::DOWNLOADING,
                      (int)(written * 100 / (size_t)total));
    }
  }
  http.end();

  if (total > 0 && written != (size_t)total) {
    DBG_PRINTF("[OTA] short download: %u of %d bytes\n", (unsigned)written,
               total);
    return false;
  }
  if (!Update.end(true)) {
    DBG_PRINTF("[OTA] Update.end failed: %s\n", Update.errorString());
    return false;
  }
  DBG_PRINTF("[OTA] flashed %u bytes OK\n", (unsigned)written);
  return true;
}

}  // namespace

void serviceOtaUpdates(uint32_t onlineForMs) {
  // Check once, early: the TLS handshake needs the still-pristine boot heap
  // (two ~17 KB contiguous mbedtls buffers; the fragmented post-feed heap's
  // largest block is too small), which is also why the feed fetches wait
  // for otaBootGateReached(). The periodic recheck below is best-effort.
  // A portal-requested check bypasses the pacing (the user is watching),
  // but still runs on the data task with the heap released.
  bool requested = sCheckRequested;
  sCheckRequested = false;
  if (!requested) {
    if (onlineForMs < OTA_FIRST_CHECK_AFTER_ONLINE_MS) return;
    uint32_t now = millis();
    if (sCheckedOnce) {
      uint32_t interval =
          sLastCheckOk ? OTA_CHECK_INTERVAL_MS : OTA_CHECK_RETRY_MS;
      if ((now - sLastCheckAt) < interval) return;
    } else if (sBootAttempts > 0 && (now - sLastCheckAt) < 5000) {
      return;
    }
  }
  sLastCheckAt = millis();

  // Run alone: drop the feed keep-alive session and let the data-task
  // loop hold the feed fetches until this returns.
  http_fetch::closeSession();

  // Claim the update context BEFORE any TLS connection: it needs a 4 KB
  // contiguous staging buffer that the fragmented in-session heap can no
  // longer satisfy (the 2.0.x Updater reports that failed malloc as
  // "No Error"). No flash is touched until the first write.
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    DBG_PRINTF("[OTA] Update.begin failed: %s (freeHeap=%u maxAlloc=%u)\n",
               Update.errorString(), ESP.getFreeHeap(),
               ESP.getMaxAllocHeap());
    return;
  }

  // ONE TLS session serves both the manifest and, when an update is
  // needed, the binary: this network path refuses a second fresh TLS
  // connection opened moments after the first, so a second handshake for
  // the download reliably failed.
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);  // default 120 s would stall the data task
  HTTPClient http;
  http.setReuse(true);

  http.begin(client, OTA_MANIFEST_URL);
  http.setTimeout(10000);
  int httpCode = http.GET();
  DBG_PRINTF("[OTA] manifest -> HTTP %d\n", httpCode);
  uint8_t manifestBuf[512];
  if (httpCode != HTTP_CODE_OK ||
      readExactBody(http, manifestBuf, sizeof(manifestBuf)) == 0) {
    http.end();
    Update.abort();
    sLastCheckOk = false;
    sBootAttempts++;
    // After the boot attempts are exhausted, let the feeds start; the
    // periodic retry takes over from there.
    sCheckedOnce = (sBootAttempts >= OTA_BOOT_MAX_ATTEMPTS);
    return;
  }
  // Manifest fetched: from here on, every outcome counts as "checked" so
  // the feeds can start.
  sCheckedOnce = true;
  // Null-terminate and parse; values are copied out before the buffer is
  // reused (in-memory parses link strings into the buffer).
  manifestBuf[sizeof(manifestBuf) - 1] = '\0';
  JsonDocument doc;
  DeserializationError error =
      deserializeJson(doc, (const char*)manifestBuf);
  if (error) {
    DBG_PRINTF("[OTA] manifest parse error: %s\n", error.c_str());
    http.end();
    Update.abort();
    sLastCheckOk = false;
    sCheckedOnce = true;  // fetched-but-bad: don't spin on it
    return;
  }
  char manifestVersion[16] = {};
  strlcpy(manifestVersion, doc["version"] | "", sizeof(manifestVersion));
  String url;
  const char* urlField = doc["url"] | "";
  if (urlField[0] != '\0') {
    url = urlField;
  } else {
    const char* fileField = doc["file"] | "";
    String base(OTA_MANIFEST_URL);
    int slash = base.lastIndexOf('/');
    url = base.substring(0, slash + 1) + fileField;
  }
  sLastCheckOk = manifestVersion[0] != '\0' && url.length() > 0;
  if (!sLastCheckOk) {
    http.end();
    Update.abort();
    sCheckedOnce = true;  // fetched-but-incomplete: don't spin on it
    return;
  }

  if (!manifestIsNewer(manifestVersion)) {
    DBG_PRINTF("[OTA] up to date (%s, manifest %s)\n", FIRMWARE_VERSION,
               manifestVersion);
    http.end();
    Update.abort();
    return;
  }
  DBG_PRINTF("[OTA] update available: %s -> %s\n", FIRMWARE_VERSION,
             manifestVersion);
  setOtaTargetVersion(manifestVersion);

  // Same host? Reuse the warm TLS session. Otherwise close it and open a
  // fresh one (HTTPClient cannot re-target a keep-alive socket itself).
  String manifestBase(OTA_MANIFEST_URL);
  int slash = manifestBase.lastIndexOf('/');
  bool sameHost = url.startsWith(manifestBase.substring(0, slash + 1));
  if (!sameHost) {
    http.end();
  }

  http.setTimeout(15000);  // large binary over lossy Wi-Fi
  http.begin(client, url);
  httpCode = http.GET();
  int total = http.getSize();  // -1 when chunked
  DBG_PRINTF("[OTA] firmware -> HTTP %d (%d bytes)\n", httpCode, total);
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    Update.abort();
    return;
  }

  if (streamBodyToFlash(http, total)) {
    publishOtaStage(OtaStage::REBOOTING, 100);
    // Give the renderer a few seconds to show the reboot notice.
    uint32_t noticeUntil = millis() + 4000;
    while (millis() < noticeUntil) delay(100);
    ESP.restart();
  }

  // Failed update: tell the user, then fall back to normal operation. The
  // current firmware partition is untouched — Update only swaps on a fully
  // written + verified image.
  Update.abort();
  publishOtaStage(OtaStage::FAILED, 0);
  uint32_t failUntil = millis() + 5000;
  while (millis() < failUntil) delay(100);
  publishOtaStage(OtaStage::NONE, 0);
}

bool otaUpdateInProgress() {
  OtaStage stage = getOtaStage();
  return stage == OtaStage::DOWNLOADING || stage == OtaStage::REBOOTING;
}

void requestOtaCheckNow() { sCheckRequested = true; }
bool otaCheckRequested() { return sCheckRequested; }
bool otaEverChecked() { return sCheckedOnce; }
bool otaLastCheckOk() { return sLastCheckOk; }

bool otaBootGateReached(uint32_t onlineForMs) {
  // The TLS handshake needs a pristine heap: two ~17 KB contiguous mbedtls
  // buffers that the fragmented post-feed heap (largest block ~33 KB) can't
  // satisfy. So the feed fetches must not start until the first OTA check
  // has either run or been given up on for this session.
  return sCheckedOnce || onlineForMs > OTA_BOOT_GATE_TIMEOUT_MS;
}
