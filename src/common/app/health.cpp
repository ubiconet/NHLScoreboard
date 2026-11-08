#include "health.h"

#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "common/config.h"

namespace health {

const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "reset-pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

String lastCrashJson() {
  static bool sRead = false;
  static String sJson = "null";
  if (sRead) return sJson;
  sRead = true;

  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return sJson;
  esp_core_dump_summary_t* s = static_cast<esp_core_dump_summary_t*>(
      malloc(sizeof(esp_core_dump_summary_t)));
  if (s == nullptr) return sJson;
  if (esp_core_dump_get_summary(s) == ESP_OK) {
    char running[17] = "";
    esp_ota_get_app_elf_sha256(running, sizeof(running));
    char elf[17];
    strlcpy(elf, reinterpret_cast<const char*>(s->app_elf_sha256),
            sizeof(elf));
    char task[sizeof(s->exc_task) + 1];
    strlcpy(task, s->exc_task, sizeof(task));
    char bt[16 * 11 + 1] = "";
    size_t used = 0;
    uint32_t depth = s->exc_bt_info.depth > 16 ? 16 : s->exc_bt_info.depth;
    for (uint32_t i = 0; i < depth && used < sizeof(bt); ++i) {
      used += snprintf(bt + used, sizeof(bt) - used, "%s0x%08x",
                       i ? " " : "", (unsigned)s->exc_bt_info.bt[i]);
    }
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"task\":\"%s\",\"pc\":\"0x%08x\",\"cause\":%u,"
             "\"vaddr\":\"0x%08x\",\"elf\":\"%s\",\"thisFirmware\":%s,"
             "\"bt\":\"%s\"}",
             task, (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause,
             (unsigned)s->ex_info.exc_vaddr, elf,
             strncmp(elf, running, 16) == 0 ? "true" : "false", bt);
    sJson = buf;
  }
  free(s);
  return sJson;
}

void serviceHealthHeartbeat() {
#if SB_DEBUG
  static uint32_t sLastAt = 0;
  uint32_t now = millis();
  if (sLastAt != 0 && now - sLastAt < HEALTH_HEARTBEAT_MS) return;
  sLastAt = now;

  // Stack high-water marks are the fewest bytes each task has ever had
  // left; a value trending toward zero is the warning before a canary trip.
  const char* tasks[] = {"loopTask", "nhl_data", "NetworkTask", "audio"};
  char stacks[96] = "";
  size_t used = 0;
  for (const char* name : tasks) {
    TaskHandle_t h = xTaskGetHandle(name);
    if (h == nullptr) continue;
    used += snprintf(stacks + used, sizeof(stacks) - used, " %s=%u", name,
                     (unsigned)uxTaskGetStackHighWaterMark(h));
    if (used >= sizeof(stacks)) break;
  }
  Serial.printf(
      "[HEALTH] up=%lus int free=%u min=%u big=%u | psram free=%u big=%u |"
      " stack-left%s\n",
      (unsigned long)(now / 1000),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), stacks);
#endif
}

}  // namespace health
