#include <Arduino.h>

#include "common/ui/gfx.h"

void printClipped(Adafruit_GFX& target, const char* text, size_t maxChars) {
  char buf[64];
  strlcpy(buf, text != nullptr ? text : "", sizeof(buf));
  if (strlen(buf) > maxChars) buf[maxChars] = '\0';
  target.print(buf);
}

void drawCenteredText(Adafruit_GFX& target, const char* text, int centerX, int y) {
  int16_t x1;
  int16_t y1;
  uint16_t width;
  uint16_t height;
  target.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  target.setCursor(centerX - static_cast<int>(width) / 2, y);
  target.print(text);
}

int drawWrappedText(Adafruit_GFX& target, const char* text, int x, int y, int maxWidth,
                    int charWidth, int lineHeight, int maxLines) {
  char buffer[256];
  strlcpy(buffer, text != nullptr ? text : "", sizeof(buffer));

  char lineBuf[64] = {};
  int linesDrawn = 0;
  char* word = strtok(buffer, " ");
  while (word != nullptr && linesDrawn < maxLines) {
    char candidate[64];
    snprintf(candidate, sizeof(candidate), "%s%s%s",
             lineBuf, lineBuf[0] != '\0' ? " " : "", word);
    if (static_cast<int>(strlen(candidate)) * charWidth > maxWidth && lineBuf[0] != '\0') {
      target.setCursor(x, y + linesDrawn * lineHeight);
      target.print(lineBuf);
      linesDrawn++;
      strlcpy(lineBuf, word, sizeof(lineBuf));
    } else {
      strlcpy(lineBuf, candidate, sizeof(lineBuf));
    }
    word = strtok(nullptr, " ");
  }
  if (lineBuf[0] != '\0' && linesDrawn < maxLines) {
    target.setCursor(x, y + linesDrawn * lineHeight);
    target.print(lineBuf);
    linesDrawn++;
  }
  return y + linesDrawn * lineHeight;
}
