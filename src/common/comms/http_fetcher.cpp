#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "common/config.h"
#include "common/data/time_util.h"
#include "http_fetcher.h"

namespace http_fetch {
namespace {

// The Date response header is collected on every GET: any 200 response
// gives the device a coarse UTC clock before NTP lands (and as a full
// fallback when the AP blocks NTP) — see applyCoarseHttpDate.
// Transfer-Encoding tells the body reader how the response is framed.
const char* kCollectedHeaders[] = {"Date", "Transfer-Encoding"};
const size_t kCollectedHeaderCount =
    sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]);

// Shared session: one WiFiClient + one HTTPClient reused across the fetches
// of a single data-task tick via HTTP/1.1 keep-alive, and kept open between
// ticks. Feed clients that need their own connection (news, OTA) call
// closeSession() first so the two never overlap. A secure session
// (WiFiClientSecure, insecure handshake — public data) replaces the plain
// one while any https URL is being fetched; both never coexist.
WiFiClient* sClient = nullptr;
WiFiClientSecure* sSecureClient = nullptr;
HTTPClient sHttp;
bool sConnected = false;

void resetSession() {
  sHttp.end();
  if (sClient != nullptr) sClient->stop();
  if (sSecureClient != nullptr) sSecureClient->stop();
  sConnected = false;
}

void applyDateHeaderIfAny(int code) {
  if (code <= 0) return;
  String d = sHttp.header("Date");
  if (d.length() > 0) applyCoarseHttpDate(d.c_str());
}

// Response-body buffer. Parsing a large JSON straight off the live TLS
// stream overflowed (IncompleteInput / esp-sha allocation failures), so
// the body is read in full first and parsed from memory. It is a plain
// heap block, not a String: full NHL slates run past 64 KB (day scoreboard
// ~95 KB on a 12-game night, week schedule ~96 KB, a finished game's
// play-by-play ~120 KB), and without BOARD_HAS_PSRAM an Arduino String
// can't grow past 65,535 bytes — concat() just fails. The block prefers
// PSRAM (the N8R8 module's 8 MB is in the heap from boot) so big bodies
// stay off the internal RAM that TLS handshakes need, and falls back to
// internal RAM on a board without PSRAM.
const size_t kMaxBodyBytes = 384 * 1024;
char* sBody = nullptr;
size_t sBodyLen = 0;
size_t sBodyCap = 0;  // usable bytes, excluding the NUL terminator

bool reserveBody(size_t need) {
  if (need <= sBodyCap) return true;
  if (need > kMaxBodyBytes) return false;
  size_t cap = sBodyCap > 0 ? sBodyCap : 16 * 1024;
  while (cap < need) cap *= 2;
  if (cap > kMaxBodyBytes) cap = kMaxBodyBytes;
  void* grown = heap_caps_realloc(sBody, cap + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (grown == nullptr) {
    grown = heap_caps_realloc(sBody, cap + 1, MALLOC_CAP_8BIT);
  }
  if (grown == nullptr) return false;
  sBody = static_cast<char*>(grown);
  sBodyCap = cap;
  return true;
}

void clearBody() {
  sBodyLen = 0;
  if (sBody != nullptr) sBody[0] = '\0';
}

// Body read pacing. This install's Wi-Fi stalls for seconds at a time and
// the TCP receive window is only ~5.7 KB (a 95 KB body at 600 ms RTT takes
// ~10 s), so a read fails on INACTIVITY rather than a short total
// deadline; the overall cap keeps a trickling link from holding the data
// task indefinitely.
const uint32_t kBodyIdleTimeoutMs = 10000;
const uint32_t kBodyTotalTimeoutMs = 30000;
// The blank line after the last chunk normally rides the same segment.
const uint32_t kTrailerTimeoutMs = 1000;

struct ReadClock {
  uint32_t idleMs, totalMs, startedAt, lastByteAt;
  ReadClock(uint32_t idle, uint32_t total)
      : idleMs(idle), totalMs(total), startedAt(millis()),
        lastByteAt(startedAt) {}
  void progress() { lastByteAt = millis(); }
  bool expired() const {
    uint32_t now = millis();
    return now - lastByteAt > idleMs || now - startedAt > totalMs;
  }
};

// Presents the buffered body to ArduinoJson as a Stream so parsed strings
// are COPIED into the JsonDocument's own pool (in-memory inputs would be
// zero-copy linked into the body buffer, which the next fetch overwrites).
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

// Appends exactly `count` body bytes from the stream, waiting out Wi-Fi
// gaps (HTTPClient::getString() bails out with a silent partial body as
// soon as connected() blips false, which happens routinely on this
// install's lossy Wi-Fi while packets are still in flight). False on
// timeout or when the body would outgrow kMaxBodyBytes.
bool readBodyBytes(WiFiClient* stream, ReadClock& clock, size_t count) {
  if (!reserveBody(sBodyLen + count)) return false;
  while (count > 0) {
    if (clock.expired()) return false;
    int avail = stream->available();
    if (avail <= 0) {
      delay(2);
      continue;
    }
    size_t want = (size_t)avail < count ? (size_t)avail : count;
    int got = stream->read(reinterpret_cast<uint8_t*>(sBody + sBodyLen), want);
    if (got <= 0) {
      delay(2);
      continue;
    }
    sBodyLen += (size_t)got;
    sBody[sBodyLen] = '\0';
    count -= (size_t)got;
    clock.progress();
  }
  return true;
}

// One CRLF-terminated line of chunked framing: a chunk-size line, the CRLF
// closing a chunk's data, or a trailer. False on timeout, or for a line too
// long to be framing.
bool readFramingLine(WiFiClient* stream, ReadClock& clock, char* line,
                     size_t lineSize) {
  size_t n = 0;
  while (!clock.expired()) {
    uint8_t c;
    if (stream->available() <= 0 || stream->read(&c, 1) != 1) {
      delay(2);
      continue;
    }
    clock.progress();
    if (c == '\n') {
      if (n > 0 && line[n - 1] == '\r') --n;
      line[n] = '\0';
      return true;
    }
    if (n + 1 >= lineSize) return false;
    line[n++] = (char)c;
  }
  return false;
}

// Chunked transfer coding — HTTPClient does NOT de-chunk getStream() (left
// raw, a JSON body prefixed by e.g. "10128\r\n" parses as a bare number).
// Complete only once the zero-size last chunk arrives.
bool readChunkedBody(WiFiClient* stream, ReadClock& clock) {
  char line[64];
  while (true) {
    if (!readFramingLine(stream, clock, line, sizeof(line))) return false;
    char* end = nullptr;
    unsigned long size = strtoul(line, &end, 16);
    bool sizeLine = end != line &&
        (*end == '\0' || *end == ';' || *end == ' ' || *end == '\t');
    if (!sizeLine) return false;  // the framing is lost
    if (size == 0) break;
    if (!readBodyBytes(stream, clock, size)) return false;
    if (!readFramingLine(stream, clock, line, sizeof(line)) ||
        line[0] != '\0') {
      return false;  // chunk data must end in CRLF
    }
  }
  // Trailer fields + the blank line that ends the message: consuming them
  // leaves the keep-alive stream on the next response. Best effort — the
  // body is already complete, and HTTPClient drains leftovers on reuse.
  ReadClock trailerClock(kTrailerTimeoutMs, kTrailerTimeoutMs);
  while (readFramingLine(stream, trailerClock, line, sizeof(line))) {
    if (line[0] == '\0') break;
  }
  return true;
}

// Neither Content-Length nor chunked: the body runs until the server
// closes the connection.
bool readCloseDelimitedBody(WiFiClient* stream, ReadClock& clock) {
  while (!clock.expired()) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected()) return sBodyLen > 0;
      delay(2);
      continue;
    }
    if (!readBodyBytes(stream, clock, (size_t)avail)) return false;
  }
  return false;
}

// Reads the response body into the body buffer. True ONLY when the whole
// body arrived: a partial one (timeout, broken framing, over the size cap)
// is a failed fetch, never something to parse — a cut-off day slate
// silently loses the night's late games.
bool readResponseBody(HTTPClient& http) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) return false;
  clearBody();
  ReadClock clock(kBodyIdleTimeoutMs, kBodyTotalTimeoutMs);
  if (http.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
    return readChunkedBody(stream, clock);
  }
  int contentLength = http.getSize();
  if (contentLength >= 0) {
    return readBodyBytes(stream, clock, (size_t)contentLength);
  }
  return readCloseDelimitedBody(stream, clock);
}

}  // namespace

int get(const String& url, uint32_t timeoutMs) {
  if (sClient == nullptr) {
    sClient = new WiFiClient();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    sHttp.setReuse(true);
    sHttp.setTimeout(timeoutMs);
    sHttp.collectHeaders(kCollectedHeaders, kCollectedHeaderCount);
    if (!sHttp.begin(*sClient, url)) return -1;
    int code = sHttp.GET();
    applyDateHeaderIfAny(code);
    if (code >= 0 || attempt == 1) {
      sConnected = (code > 0);
      return code;
    }
    // code < 0: stale keep-alive or transient — rebuild the session once.
    resetSession();
  }
  return -1;
}

int getSecure(const String& url, uint32_t timeoutMs) {
  if (sSecureClient == nullptr) {
    sSecureClient = new WiFiClientSecure();
    sSecureClient->setInsecure();  // public sports data; also immune to the
                                   // pre-NTP clock making cert times fail
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    sHttp.setReuse(true);
    sHttp.setTimeout(timeoutMs);
    sHttp.collectHeaders(kCollectedHeaders, kCollectedHeaderCount);
    if (!sHttp.begin(*sSecureClient, url)) return -1;
    int code = sHttp.GET();
    applyDateHeaderIfAny(code);
    if (code >= 0 || attempt == 1) {
      sConnected = (code > 0);
      return code;
    }
    resetSession();
  }
  return -1;
}

DeserializationError parseBody(JsonDocument& doc, JsonDocument* filter) {
  bool readOk = readResponseBody(sHttp);
  DBG_PRINTF("[HTTP] body %u bytes%s\n", (unsigned)sBodyLen,
             readOk ? "" : ", read FAILED");
  if (!readOk) {
    // The keep-alive stream is mid-response: the next request needs a
    // clean connection.
    clearBody();
    resetSession();
    return DeserializationError(DeserializationError::IncompleteInput);
  }
  MemoryReadStream stream(sBody, sBodyLen);
  if (filter != nullptr) {
    return deserializeJson(doc, stream, DeserializationOption::Filter(*filter));
  }
  return deserializeJson(doc, stream);
}

void closeSession() {
  resetSession();
}

const char* debugBody() { return sBody != nullptr ? sBody : ""; }
size_t debugBodyLen() { return sBodyLen; }

void releaseBodyBuffer() {
  if (sBody != nullptr) {
    heap_caps_free(sBody);
    sBody = nullptr;
  }
  sBodyLen = 0;
  sBodyCap = 0;
}

void logCall(const char* endpointName, int httpCode) {
  DBG_PRINTF("[API CALL] %s -> HTTP %d | freeHeap=%u maxAlloc=%u\n",
             endpointName, httpCode, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

}  // namespace http_fetch
