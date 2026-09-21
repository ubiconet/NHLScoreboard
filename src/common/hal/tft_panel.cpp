#include "tft_panel.h"

// Single instance, driven from the Arduino loop core only.
TftPanel tftPanel;

void TftPanel::begin(int csPin, int dcPin, int mosiPin, int sclkPin,
                     int resetPin, int nativeWidth, int nativeHeight,
                     int rotation) {
  if (tft_ == nullptr) {
    tft_ = new Adafruit_ST7789(csPin, dcPin, mosiPin, sclkPin, resetPin);
  }
  tft_->init(nativeWidth, nativeHeight);
  tft_->setRotation(rotation);
  // Logical (post-rotation) frame size the canvas and push calls use.
  w_ = (rotation % 2 == 1) ? nativeHeight : nativeWidth;
  h_ = (rotation % 2 == 1) ? nativeWidth : nativeHeight;
}

GFXcanvas16& TftPanel::canvas() {
  // Allocation comment preserved from the original getCanvas(): if the heap
  // is starved at boot (large news sprite + JSON docs all live alongside
  // this), new may return nullptr; fall back to a tiny 1x1 canvas so the
  // loop at least paints *something* instead of crashing.
  static GFXcanvas16* canvasPtr = nullptr;
  if (canvasPtr == nullptr) {
    canvasPtr = new GFXcanvas16(w_, h_);
    if (canvasPtr == nullptr) {
      static GFXcanvas16 stub(1, 1);
      return stub;
    }
  }
  return *canvasPtr;
}

void TftPanel::pushFull() {
  if (tft_ == nullptr) return;
  GFXcanvas16& c = canvas();
  tft_->drawRGBBitmap(0, 0, c.getBuffer(), w_, h_);
}

void TftPanel::pushBand(int y, int h) {
  if (tft_ == nullptr) return;
  GFXcanvas16& c = canvas();
  tft_->drawRGBBitmap(0, y, c.getBuffer() + (size_t)y * w_, w_, h);
}

void TftPanel::pushRows(int x, int y, int w, int h) {
  if (tft_ == nullptr) return;
  GFXcanvas16& c = canvas();
  for (int row = 0; row < h; ++row) {
    tft_->drawRGBBitmap(x, y + row,
                        c.getBuffer() + (size_t)(y + row) * w_ + x, w, 1);
  }
}

Adafruit_ST7789& TftPanel::raw() {
  static Adafruit_ST7789* tft = tft_;  // begin() must have run
  return *tft;
}
