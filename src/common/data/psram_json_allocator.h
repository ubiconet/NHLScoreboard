#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ArduinoJson allocator that keeps a document's pools and strings in PSRAM.
//
// ArduinoJson grows a document in 1 KB pools, and blocks that small always
// land in INTERNAL RAM (only allocations over 4 KB prefer PSRAM), so a
// long-lived multi-week schedule cache would fragment the internal heap
// that feed TLS handshakes need (two ~17 KB contiguous mbedTLS buffers).
// Use it for large or long-lived documents; small transient parse docs can
// keep the default allocator. Falls back to internal RAM without PSRAM.
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }

  void deallocate(void* ptr) override { heap_caps_free(ptr); }

  void* reallocate(void* ptr, size_t newSize) override {
    void* p = heap_caps_realloc(ptr, newSize,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_realloc(ptr, newSize, MALLOC_CAP_8BIT);
  }
};

// Shared instance. A function-local static, so documents defined at
// namespace scope in other translation units can take it during static
// initialization.
inline ArduinoJson::Allocator* psramJsonAllocator() {
  static PsramJsonAllocator instance;
  return &instance;
}
