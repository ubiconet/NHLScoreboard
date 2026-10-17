#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "common/app/power_monitor.h"
#include "common/comms/network_service.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/hal/tm1637.h"
#include "common/ui/low_battery_screen.h"

namespace power {
namespace {

// Survives deep sleep and soft/brownout resets while power holds; zeroed by
// a true power-on. Flags "we chose to sleep for low battery", so a timer
// wake knows to re-check silently instead of doing storm bookkeeping.
RTC_DATA_ATTR bool sBatterySleep = false;

Preferences prefs;

// --- NVS boot journal (namespace "power") ---
//   died:     1 = the previous boot started but never reached healthy
//             uptime (brownout/collapse/user power-off), 2 = deliberate
//             low-battery sleep, 0 = clean previous session.
//   badboots: consecutive bad boots (brownout reset reason, or died == 1).
constexpr const char* kPrefsNamespace = "power";
constexpr const char* kKeyDied = "died";
constexpr const char* kKeyBadBoots = "badboots";

float readBatteryVolts() {
  // Only boards whose hardware carries the divider (V1.1+, per the
  // portal's board-version setting) may read the pin — on anything else
  // it floats and reads like noise.
  if (!batterySenseEnabled(getBoardVersion())) return 0.0f;
  // 11 dB attenuation maps the divider's 0-2.1 V output (half of the pack's
  // 3.0-4.2 V) into the ADC's ~0-3.1 V window. Median-of-5 rejects the boost
  // converter's ripple and single-sample ADC glitches.
  analogSetPinAttenuation(BATTERY_SENSE_PIN, ADC_11db);
  uint32_t samples[5];
  for (int i = 0; i < 5; ++i) {
    samples[i] = analogReadMilliVolts(BATTERY_SENSE_PIN);
    delay(2);
  }
  for (int i = 1; i < 5; ++i) {  // insertion sort, n = 5
    uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }
    samples[j + 1] = key;
  }
  return samples[2] * BATTERY_SENSE_DIVIDER / 1000.0f;
}

void journalDied(uint8_t value) {
  prefs.begin(kPrefsNamespace, false);
  prefs.putUChar(kKeyDied, value);
  prefs.end();
}

}  // namespace

BootDecision earlyBootCheck() {
  // The power gate is a V1.1 hardware feature (battery-sense divider).
  // On earlier board revisions low-battery detection is disabled
  // entirely: no voltage check and no boot-storm journal — those boards
  // boot and run unconditionally, even through brownout cycles.
  if (!batterySenseEnabled(getBoardVersion())) {
    return BootDecision::NORMAL;
  }

  // Timer wake from our own low-battery sleep: re-check the pack and stay
  // down silently (no display work) until the charger resumes it.
  bool timerWake = sBatterySleep &&
                   esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (timerWake) {
    if (batterySenseEnabled(getBoardVersion()) &&
        readBatteryVolts() >= BATTERY_RESUME_VOLTS) {
      sBatterySleep = false;  // charger present — resume a normal boot
      journalDied(0);
      return BootDecision::NORMAL;
    }
    return BootDecision::RESLEEP;
  }

  // Storm bookkeeping — journal FIRST, sense second: a cycle that dies
  // seconds later still counted toward the streak.
  prefs.begin(kPrefsNamespace, false);
  uint8_t died = prefs.getUChar(kKeyDied, 0);
  uint8_t badBoots = prefs.getUChar(kKeyBadBoots, 0);
  bool badBoot = (esp_reset_reason() == ESP_RST_BROWNOUT) || (died == 1);
  if (badBoot) {
    badBoots = (badBoots < 255) ? badBoots + 1 : badBoots;
  } else {
    badBoots = 0;
  }
  prefs.putUChar(kKeyDied, 1);  // assume short-lived until proven healthy
  prefs.putUChar(kKeyBadBoots, badBoots);
  prefs.end();

  bool low = badBoots >= BATTERY_BAD_BOOT_STREAK;
  if (batterySenseEnabled(getBoardVersion())) {
    float volts = readBatteryVolts();
    if (volts > 0.5f && volts <= BATTERY_LOW_VOLTS) low = true;
  }
  return low ? BootDecision::SHUTDOWN : BootDecision::NORMAL;
}

void sleepNow() {
  // Journal the deliberate sleep BEFORE anything slow: if the pack collapses
  // during the warning hold, the next boot must not count this session
  // toward the bad-boot streak.
  sBatterySleep = true;
  journalDied(2);
  esp_sleep_enable_timer_wakeup((uint64_t)BATTERY_RECHECK_MS * 1000ULL);
  esp_deep_sleep_start();  // no return
}

void showLowBatteryAndSleep() {
  float volts = readBatteryVolts();  // 0.0 when no divider is wired
  DBG_PRINTF("[POWER] low battery (%.2f V) — warning, then deep sleep\n", volts);
  renderLowBatteryWarning(volts, 2500);

  // Dark outputs: the matrices, the TM1637 clock, and the penalty LEDs
  // would otherwise keep drawing (and showing stale state) through the
  // sleep.
  setMax7219Display(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK,
                    MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);
  tm1637SetBrightness(0);

  // Blank the panel and put it into its low-power state — the ST7789 keeps
  // scanning GRAM (mA-range) unless told to sleep. The backlight LEDs have
  // no firmware control on this wiring, so they stay lit until the switch
  // is flipped; power off after seeing the warning when possible.
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);
  tftPanel.pushFull();
  tftPanel.raw().sendCommand(0x10);  // SLPIN

  sleepNow();
}

void noteHealthyUptime() {
  if (!batterySenseEnabled(getBoardVersion())) return;  // gate disabled
  if (millis() < BATTERY_HEALTHY_BOOT_MS) return;
  static bool marked = false;
  if (marked) return;
  marked = true;
  prefs.begin(kPrefsNamespace, false);
  if (prefs.getUChar(kKeyDied, 0) == 1) prefs.putUChar(kKeyDied, 0);
  prefs.end();
}

}  // namespace power
