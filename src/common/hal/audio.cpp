#include <Arduino.h>
#include <driver/i2s.h>

#include "audio.h"

// Legacy (core 2.x) I2S driver: master TX, 16-bit mono, standard I2S
// framing. MAX98357 carries the left channel, so a mono stream on
// ONLY_LEFT lands on the speaker.
//
// Clips stream from PROGMEM inside a dedicated task on core 0 using
// BLOCKING i2s_write calls — the same property that made the original
// synthesized melody sound clean. Feeding from the render core instead
// starved the DMA ring during slow software-SPI canvas pushes (a full
// push outruns the ~93 ms of buffered audio), which audibly crackled.

namespace {

const i2s_port_t kPort = I2S_NUM_0;
bool sReady = false;

// Clip state, shared between startClip() (render core) and the stream
// task (core 0). Small and spinlock-guarded.
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
const int16_t* sClip = nullptr;
volatile size_t sPos = 0;
size_t sCount = 0;
bool sFlushed = true;

void audioStreamTask(void*) {
  static int16_t buf[512];
  while (true) {
    const int16_t* clip;
    size_t pos, count;
    portENTER_CRITICAL(&sMux);
    clip = sClip;
    pos = sPos;
    count = sCount;
    portEXIT_CRITICAL(&sMux);

    if (clip == nullptr || pos >= count) {
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }
    size_t n = count - pos;
    if (n > 512) n = 512;
    memcpy_P(buf, clip + pos, n * sizeof(int16_t));
    size_t written = 0;
    i2s_write(kPort, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
    portENTER_CRITICAL(&sMux);
    if (sClip == clip) {  // not replaced by a newer start
      sPos += written / sizeof(int16_t);
      if (sPos >= sCount && !sFlushed) {
        i2s_zero_dma_buffer(kPort);  // leave the line silent when done
        sFlushed = true;
        sClip = nullptr;
      }
    }
    portEXIT_CRITICAL(&sMux);
  }
}

}  // namespace

void initAudio(int bclkPin, int lrcPin, int dinPin) {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 22050;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = bclkPin;
  pins.ws_io_num = lrcPin;
  pins.data_out_num = dinPin;
  pins.mck_io_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK ||
      i2s_set_pin(kPort, &pins) != ESP_OK) {
    Serial.println("[HW] MAX98357 audio init FAILED");
    return;
  }
  sReady = true;
  xTaskCreatePinnedToCore(audioStreamTask, "audio", 4096, nullptr, 1,
                          nullptr, 0);
  Serial.println("[HW] MAX98357 I2S audio initialized (stream task)");
}

void startClip(const int16_t* data, size_t count, int sampleRate) {
  if (!sReady || data == nullptr || count == 0) return;
  i2s_set_sample_rates(kPort, sampleRate);
  i2s_zero_dma_buffer(kPort);
  portENTER_CRITICAL(&sMux);
  sClip = data;
  sCount = count;
  sPos = 0;
  sFlushed = false;
  portEXIT_CRITICAL(&sMux);
}

bool audioPlaying() {
  portENTER_CRITICAL(&sMux);
  bool playing = sClip != nullptr;
  portEXIT_CRITICAL(&sMux);
  return playing;
}
