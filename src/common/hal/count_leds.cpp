#include <Arduino.h>
#include <esp_timer.h>

#include "count_leds.h"

namespace {
int COUNT_LED_PINS[7] = {-1, -1, -1, -1, -1, -1, -1};

// The blink timer runs in the esp_timer task while setCountLeds() runs on
// the render core: the lock keeps the remembered counts and the pins in
// step, so ending blink mode can never leave a stale blink phase lit.
portMUX_TYPE sLedLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t sCountA = 0, sCountB = 0, sCountC = 0;  // last setCountLeds() request
bool sBlinking = false;
bool sBlinkLit = false;
uint8_t sBlinkA = 0, sBlinkB = 0, sBlinkC = 0;
esp_timer_handle_t sBlinkTimer = nullptr;

// Caller holds sLedLock.
void writeCounts(uint8_t countA, uint8_t countB, uint8_t countC) {
  const bool lit[7] = {countA >= 1, countA >= 2, countA >= 3,
                       countB >= 1, countB >= 2,
                       countC >= 1, countC >= 2};
  for (int i = 0; i < 7; ++i) {
    if (COUNT_LED_PINS[i] >= 0) {
      digitalWrite(COUNT_LED_PINS[i], lit[i] ? HIGH : LOW);
    }
  }
}

void onBlinkTimer(void*) {
  portENTER_CRITICAL(&sLedLock);
  if (sBlinking) {
    sBlinkLit = !sBlinkLit;
    if (sBlinkLit) {
      writeCounts(sBlinkA, sBlinkB, sBlinkC);
    } else {
      writeCounts(0, 0, 0);
    }
  }
  portEXIT_CRITICAL(&sLedLock);
}
}  // namespace

void initCountLeds(const int pins[7]) {
  for (int i = 0; i < 7; ++i) {
    COUNT_LED_PINS[i] = pins[i];
    pinMode(pins[i], OUTPUT);
  }
  setCountLeds(0, 0, 0);
  Serial.println("[HW] Count LED driver initialized");
}

void setCountLeds(uint8_t countA, uint8_t countB, uint8_t countC) {
  portENTER_CRITICAL(&sLedLock);
  sCountA = countA;
  sCountB = countB;
  sCountC = countC;
  if (!sBlinking) writeCounts(countA, countB, countC);
  portEXIT_CRITICAL(&sLedLock);
}

void setCountLedsBlink(bool on, uint8_t blinkA, uint8_t blinkB,
                       uint8_t blinkC, uint32_t periodMs) {
  if (on == sBlinking) return;
  if (on && sBlinkTimer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = onBlinkTimer;
    args.name = "count_led_blink";
    if (esp_timer_create(&args, &sBlinkTimer) != ESP_OK) {
      sBlinkTimer = nullptr;
      return;
    }
  }
  portENTER_CRITICAL(&sLedLock);
  sBlinking = on;
  if (on) {
    sBlinkA = blinkA;
    sBlinkB = blinkB;
    sBlinkC = blinkC;
    sBlinkLit = true;  // start lit so the indicator shows at once
    writeCounts(blinkA, blinkB, blinkC);
  } else {
    writeCounts(sCountA, sCountB, sCountC);
  }
  portEXIT_CRITICAL(&sLedLock);
  if (on) {
    esp_timer_start_periodic(sBlinkTimer, (uint64_t)periodMs * 1000ULL);
  } else {
    esp_timer_stop(sBlinkTimer);
  }
}

void runCountLedTestLoop() {
  for (int i = 0; i < 7; ++i) {
    for (int j = 0; j < 7; ++j) {
      digitalWrite(COUNT_LED_PINS[j], j == i ? HIGH : LOW);
    }

    Serial.printf("[HW TEST] LED pin %d ON\n", COUNT_LED_PINS[i]);
    delay(500);
  }

  setCountLeds(0, 0, 0);
}
