#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// http_fetch — one shared keep-alive HTTP/1.1 session plus the buffered
// body reader every feed client on the data task builds on.
//
// All feed endpoints are fetched over plain HTTP on purpose: payloads are
// public sports data, validation-free TLS pinned ~45 KB of heap (more than
// the device has to spare alongside the JSON documents), and the handshake
// allocations (RSA/BIGNUM) fail on the fragmented heap. See the note at the
// top of the original client implementation (now sports/mlb/mlb_client.cpp)
// for the full history.
//
// The session is kept OPEN between calls: this install's network path
// refuses new TCP flows from the device after a burst of them, so reusing
// one connection across ticks is what keeps feeds alive. Use closeSession()
// before opening any other connection (e.g. the OTA updater's TLS).
namespace http_fetch {

// One GET on the shared session. Retries once on a fresh connection
// (covers a keep-alive socket the server silently dropped). Returns the
// HTTP status code, or -1 on transport failure.
int get(const String& url, uint32_t timeoutMs);

// Buffers the body of the response from a successful get() and parses it.
// Call only when get() returned HTTP_CODE_OK. Pass a filter to restrict
// what is kept, or nullptr to parse the whole body. The connection is left
// open on success so the shared session can serve the next request; close
// the session on any caller-side failure path.
DeserializationError parseBody(JsonDocument& doc, JsonDocument* filter = nullptr);

// Drop the shared keep-alive session (used by the OTA updater, which needs
// the heap + airtime to itself for its TLS download connection).
void closeSession();

// Return the retained response-body buffer to the heap (the OTA updater
// does this before mid-session TLS checks to maximize contiguous space).
void releaseBodyBuffer();

// Uniform debug line for every outbound call: endpoint name + HTTP code.
// Gated by SB_DEBUG so the live game loop doesn't stall ~25 ms per printf.
// maxAlloc is the largest contiguous block the heap can still satisfy —
// freeHeap alone hides fragmentation, and TLS handshake allocations fail
// against maxAlloc, not freeHeap.
void logCall(const char* endpointName, int httpCode);

}  // namespace http_fetch
