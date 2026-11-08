#pragma once

#include <Adafruit_GFX.h>

#ifndef GxEPD_BLACK
#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF
#endif

/** Shared Cosmo fonts + text helpers (fonts compiled once, in Typo.cpp). */
namespace typo {

extern const GFXfont *const SerifXL;  // Time Big
extern const GFXfont *const SerifL;   // Time Small
extern const GFXfont *const SerifText;       // wordmark "cosmo" (bold)
extern const GFXfont *const SerifTextLight;  // wordmark "libre" (regular)
extern const GFXfont *const MonoL;    // date digits, temperatures
extern const GFXfont *const MonoM;    // greeting, words, titles
extern const GFXfont *const MonoS;    // labels, menu rows
extern const GFXfont *const MonoXS;   // status, hints

struct Ink {
  int16_t x1, y1;
  uint16_t w, h;
};

Ink measure(Adafruit_GFX &g, const GFXfont *f, const char *s);
int16_t capHeight(Adafruit_GFX &g, const GFXfont *f);
void textAt(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
            int16_t baseline, uint16_t color = GxEPD_BLACK);
/** Centre the *ink* of s horizontally on cx. */
void textCentered(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t cx,
                  int16_t baseline, uint16_t color = GxEPD_BLACK);
/** Right-align the ink of s to x. */
void textRight(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
               int16_t baseline, uint16_t color = GxEPD_BLACK);

/** Letter-spaced text (the original Cosmo look). The gap shrinks to fit maxW. */
void textSpaced(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t x,
                int16_t baseline, int16_t maxW, int16_t gap = 3,
                uint16_t color = GxEPD_BLACK);
/** Width textSpaced would use (before any shrinking). */
int16_t spacedWidth(Adafruit_GFX &g, const GFXfont *f, const char *s, int16_t gap);
void textSpacedCentered(Adafruit_GFX &g, const GFXfont *f, const char *s,
                        int16_t cx, int16_t baseline, int16_t gap = 3,
                        uint16_t color = GxEPD_BLACK);

// Flash-string (F("...")) versions: keep UI copy out of the ESP8266's RAM.
Ink measure(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s);
void textAt(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s,
            int16_t x, int16_t baseline, uint16_t color = GxEPD_BLACK);
void textCentered(Adafruit_GFX &g, const GFXfont *f,
                  const __FlashStringHelper *s, int16_t cx, int16_t baseline,
                  uint16_t color = GxEPD_BLACK);
void textRight(Adafruit_GFX &g, const GFXfont *f, const __FlashStringHelper *s,
               int16_t x, int16_t baseline, uint16_t color = GxEPD_BLACK);

}  // namespace typo
