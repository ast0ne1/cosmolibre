#include "hal/Typo.h"

#include <string.h>

#include "fonts/CosmoMonoL.h"
#include "fonts/CosmoMonoM.h"
#include "fonts/CosmoMonoS.h"
#include "fonts/CosmoMonoXS.h"
#include "fonts/CosmoSerifL.h"
#include "fonts/CosmoSerifText.h"
#include "fonts/CosmoSerifTextLight.h"
#include "fonts/CosmoSerifXL.h"

namespace typo {

const GFXfont *const SerifXL = &CosmoSerifXL;
const GFXfont *const SerifL = &CosmoSerifL;
const GFXfont *const SerifText = &CosmoSerifText;
const GFXfont *const SerifTextLight = &CosmoSerifTextLight;
const GFXfont *const MonoL = &CosmoMonoL;
const GFXfont *const MonoM = &CosmoMonoM;
const GFXfont *const MonoS = &CosmoMonoS;
const GFXfont *const MonoXS = &CosmoMonoXS;

Ink measure(Adafruit_GFX &g, const GFXfont *f, const char *s) {
  g.setFont(f);
  g.setTextSize(1);
  g.setTextWrap(false);
  Ink i{};
  g.getTextBounds(s, 0, 0, &i.x1, &i.y1, &i.w, &i.h);
  return i;
}

int16_t capHeight(Adafruit_GFX &g, const GFXfont *f) {
  return (int16_t)measure(g, f, "0").h;
}

void textAt(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
            int16_t baseline, uint16_t color) {
  g.setFont(f);
  g.setTextColor(color);
  g.setCursor(x, baseline);
  g.print(s);
}

void textCentered(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t cx,
                  int16_t baseline, uint16_t color) {
  Ink i = measure(g, f, s);
  textAt(g, f, s, cx - (int16_t)i.w / 2 - i.x1, baseline, color);
}

void textRight(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
               int16_t baseline, uint16_t color) {
  Ink i = measure(g, f, s);
  textAt(g, f, s, x - (int16_t)i.w - i.x1, baseline, color);
}

int16_t spacedWidth(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t gap) {
  const int n = (int)strlen(s);
  return (int16_t)(measure(g, f, s).w + (n > 1 ? gap * (n - 1) : 0));
}

void textSpaced(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
                int16_t baseline, int16_t maxW, int16_t gap, uint16_t color) {
  const int n = (int)strlen(s);
  const Ink ink = measure(g, f, s);
  if (n > 1 && ink.w + gap * (n - 1) > maxW) {
    gap = (int16_t)((maxW - (int16_t)ink.w) / (n - 1));
    if (gap < 0) gap = 0;
  }
  g.setFont(f);
  g.setTextColor(color);
  g.setCursor(x - ink.x1, baseline);
  for (const char *c = s; *c; c++) {
    g.print(*c);
    g.setCursor(g.getCursorX() + gap, baseline);
  }
}

void textSpacedCentered(Adafruit_GFX &g, const GFXfont *f, const char *s,
                        int16_t cx, int16_t baseline, int16_t gap, uint16_t color) {
  const int16_t w = spacedWidth(g, f, s, gap);
  textSpaced(g, f, s, cx - w / 2, baseline, w + 1, gap, color);
}

Ink measure(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s) {
  g.setFont(f);
  g.setTextSize(1);
  g.setTextWrap(false);
  Ink i{};
  g.getTextBounds(s, 0, 0, &i.x1, &i.y1, &i.w, &i.h);
  return i;
}

void textAt(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s,
            int16_t x, int16_t baseline, uint16_t color) {
  g.setFont(f);
  g.setTextColor(color);
  g.setCursor(x, baseline);
  g.print(s);
}

void textCentered(Adafruit_GFX &g, const GFXfont *f,
                  const __FlashStringHelper *s, int16_t cx, int16_t baseline,
                  uint16_t color) {
  Ink i = measure(g, f, s);
  textAt(g, f, s, cx - (int16_t)i.w / 2 - i.x1, baseline, color);
}

void textRight(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s,
               int16_t x, int16_t baseline, uint16_t color) {
  Ink i = measure(g, f, s);
  textAt(g, f, s, x - (int16_t)i.w - i.x1, baseline, color);
}

}  // namespace typo
