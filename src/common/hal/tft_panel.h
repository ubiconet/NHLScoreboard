#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// TftPanel — owns the ST7789 TFT panel plus the one shared full-screen render
// canvas every screen draws into. All rendering rasterizes onto the canvas;
// exactly one push call then moves pixels to the panel, so each screen has a
// single choke point instead of scattered direct panel writes.
//
// Pins and geometry are passed to begin() by the sport/app layer (they live
// in the sport's sport_config.h) — this file stays build-agnostic.
class TftPanel {
 public:
  // Construct + init the panel over software SPI. The 5-pin form is
  // deliberate: on ESP32-S3 the hardware-SPI Adafruit_ST7789 constructor
  // ends up bound to the variant's default VSPI pins instead of the panel
  // pins, leaving a blank screen — software SPI bit-bangs exactly the pins
  // we pass. Do NOT switch to the 3-arg constructor (see repo memory /
  // AGENTS.md). rotation 1 = landscape 320x240 with (0,0) top-left.
  void begin(int csPin, int dcPin, int mosiPin, int sclkPin, int resetPin,
             int nativeWidth, int nativeHeight, int rotation);

  // Double-buffer canvas (153.6 KB at 320x240), lazily allocated ONCE on
  // first use — never from the render loop's hot path. If the heap is
  // starved at boot, falls back to a 1x1 stub so the loop paints something
  // instead of crashing on a null deref.
  GFXcanvas16& canvas();

  // Push the whole canvas to the panel in a single call.
  void pushFull();

  // Push a full-width horizontal band (canvas rows y..y+h-1) in one call —
  // valid because a full-width band is contiguous in the canvas buffer.
  void pushBand(int y, int h);

  // Push an arbitrary sub-rectangle row by row. drawRGBBitmap expects a
  // packed w-wide bitmap, but a window into the wider canvas straddles row
  // strides, so each row is pushed as one contiguous w-pixel run.
  void pushRows(int x, int y, int w, int h);

  // Direct panel access for one-off screens that draw without the canvas
  // (e.g. the AP provisioning page). Don't use from normal render paths.
  Adafruit_ST7789& raw();

  int width() const { return w_; }
  int height() const { return h_; }

 private:
  Adafruit_ST7789* tft_ = nullptr;
  int w_ = 0;
  int h_ = 0;
};

extern TftPanel tftPanel;
