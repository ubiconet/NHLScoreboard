#pragma once

#include <Adafruit_GFX.h>

// Small text helpers shared by every screen. All operate on any Adafruit_GFX
// target (the shared render canvas, or the panel directly during setup).

// Prints at most maxChars characters so fixed columns don't overflow.
void printClipped(Adafruit_GFX& target, const char* text, size_t maxChars);

// Draws text horizontally centered on centerX.
void drawCenteredText(Adafruit_GFX& target, const char* text, int centerX, int y);

// Draws left-aligned word-wrapped text and returns the y just past the last
// line drawn.
int drawWrappedText(Adafruit_GFX& target, const char* text, int x, int y,
                    int maxWidth, int charWidth, int lineHeight, int maxLines);
