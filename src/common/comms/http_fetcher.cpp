#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include "common/config.h"
#include "http_fetcher.h"

namespace http_fetch {
namespace {

// Shared session: one WiFiClient + one HTTPClient reused across the fetches
// of a single data-task tick via HTTP/1.1 keep-alive, and kept open between
// ticks. Feed clients that need their own connection (news, OTA) call
// closeSession() first so the two never overlap.
WiFiClient* sClient = nullptr;
HTTPClient sHttp;
bool sConnected = false;

void resetSession() {
  sHttp.end();
  if (sClient != nullptr) sClient->stop();
  sConnected = false;
}

// Reusable response-body buffer. An open TLS session pins ~45 KB of heap
// (mbedTLS in/out record buffers), and the device only has ~50 KB free once
// Wi-Fi + the display canvas are up, so parsing a large JSON straight off
// the live stream overflowed (IncompleteInput / esp-sha allocation
// failures). Instead the body is read into this buffer first; payloads are
// kept small (teamId-filtered schedule, linescore) so the subsequent parse
// fits alongside the open session. Capacity is kept between fetches so the
// same heap block is reused instead of churned.
String sResponseBody;

// Presents the buffered body to ArduinoJson as a Stream so parsed strings
// are COPIED into the JsonDocument's own pool (in-memory inputs would be
// zero-copy linked into sResponseBody, which the next fetch overwrites).
class MemoryReadStream : public Stream {
 public:
  MemoryReadStream(const char* data, size_t len)
      : data_(data), len_(len), pos_(0) {}
  int available() override { return (int)(len_ - pos_); }
  int read() override { return pos_ < len_ ? (unsigned char)data_[pos_++] : -1; }
  int peek() override { return pos_ < len_ ? (unsigned char)data_[pos_] : -1; }
  size_t write(uint8_t) override { return 0; }
 private:
  const char* data_;
  size_t len_;
  size_t pos_;
};

// Reads the response body into sResponseBody with an explicit deadline.
// HTTPClient::getString() uses an available()-polling loop that bails out
// with a silent empty/partial String as soon as connected() blips false —
// which happens routinely on this install's lossy Wi-Fi while body packets
// are still in flight. This reader waits out the gaps instead, and reads
// EXACTLY Content-Length bytes so the keep-alive connection stays clean for
// the next request on the shared session.
bool readResponseBody(HTTPClient& http, uint32_t timeoutMs) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) return false;
  int remaining = http.getSize();  // Content-Length, or -1 when absent
  uint32_t deadline = millis() + timeoutMs;
  uint8_t buf[512];
  sResponseBody = "";
  while ((int)sResponseBody.length() < 0x10000 && millis() < deadline) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (remaining == 0) break;  // known size fully read
      if (remaining < 0 && !stream->connected()) break;  // close-delimited end
      delay(2);
      continue;
    }
    size_t toRead = sizeof(buf);
    if (toRead > avail) toRead = avail;
    if (remaining > 0 && toRead > (size_t)remaining) toRead = (size_t)remaining;
    size_t got = stream->readBytes(buf, toRead);
    if (got == 0) continue;
    sResponseBody.concat((const char*)buf, (unsigned int)got);
    if (remaining > 0) remaining -= (int)got;
    if (remaining == 0) break;
  }
  return (remaining == 0) ||
         (remaining < 0 && sResponseBody.length() > 0);
}

}  // namespace

int get(const String& url, uint32_t timeoutMs) {
  if (sClient == nullptr) {
    sClient = new WiFiClient();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    sHttp.setReuse(true);
    sHttp.setTimeout(timeoutMs);
    if (!sHttp.begin(*sClient, url)) return -1;
    int code = sHttp.GET();
    if (code >= 0 || attempt == 1) {
      sConnected = (code > 0);
      return code;
    }
    // code < 0: stale keep-alive or transient — rebuild the session once.
    resetSession();
  }
  return -1;
}

DeserializationError parseBody(JsonDocument& doc, JsonDocument* filter) {
  int contentLength = sHttp.getSize();
  bool readOk = readResponseBody(sHttp, 8000);
  DBG_PRINTF("[HTTP] body %u bytes (content-length %d%s)\n",
             (unsigned)sResponseBody.length(), contentLength,
             readOk ? "" : ", read incomplete");
  if (!readOk) {
    return DeserializationError(DeserializationError::IncompleteInput);
  }
  MemoryReadStream stream(sResponseBody.c_str(), sResponseBody.length());
  if (filter != nullptr) {
    return deserializeJson(doc, stream, DeserializationOption::Filter(*filter));
  }
  return deserializeJson(doc, stream);
}

void closeSession() {
  resetSession();
}

void releaseBodyBuffer() {
  sResponseBody = String();
}

void logCall(const char* endpointName, int httpCode) {
  DBG_PRINTF("[API CALL] %s -> HTTP %d | freeHeap=%u maxAlloc=%u\n",
             endpointName, httpCode, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

}  // namespace http_fetch
