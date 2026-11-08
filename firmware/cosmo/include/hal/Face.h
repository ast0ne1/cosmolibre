#pragma once

#include <Adafruit_GFX.h>
#include <time.h>

#include "hal/Weather.h"
#include "hal/Widgets.h"

#ifndef GxEPD_BLACK
#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF
#endif

/** Degree sign slot in the generated Cosmo* fonts (see scripts/make_fonts.py). */
#define FACE_DEG "\x7F"

enum class CardIcon : uint8_t { None, Wifi, Cross };

/**
 * Full-screen message card in the original's layout: TITLE, icon, HEADING,
 * up to 3 letter-spaced lines, footer (optionally with the • cursor). Strings
 * are plain RAM strings (copy F() text first).
 */
struct Card {
  const char *title;
  CardIcon icon;
  const char *heading;
  const char *lines[3];
  const char *footer;
  bool footerSelected;
};

/** Everything the face needs — no globals, so it also renders on the desktop. */
struct FaceInput {
  const FaceLayout *layout;
  const WeatherSnapshot *wx;
  const struct tm *now;  // local time, nullptr until NTP has synced
  const char *status;    // small bottom line (IP / net state), may be empty
};

struct FaceRect {
  int16_t x, y, w, h;
};

/**
 * Cosmo-style stacked widget face for the 280×480 panel. Slots share the
 * height by widgetWeight(); empty slots collapse.
 */
class Face {
 public:
  void drawFull(Adafruit_GFX &gfx, int16_t w, int16_t h, const FaceInput &in);
  /** Full-screen message card (Wi‑Fi setup, Wi‑Fi failed). */
  void drawCard(Adafruit_GFX &gfx, int16_t w, int16_t h, const Card &card);
  /** Boot screen while Wi‑Fi joins. */
  void drawSplash(Adafruit_GFX &gfx, int16_t w, int16_t h, const char *status);
  /** Redraw one slot at its full-draw position (for a partial refresh). */
  void drawSlotOnly(Adafruit_GFX &gfx, int16_t w, uint8_t slot,
                    const FaceInput &in);
  /** One widget alone in r (web UI toolbox thumbnails). */
  void drawWidget(Adafruit_GFX &gfx, const FaceRect &r, SlotConfig slot,
                  const FaceInput &in) {
    drawSlot(gfx, r, slot, in);
  }
  /** Height a widget typically gets on the panel (thumbnail size). */
  static int16_t widgetHeight(SlotConfig slot);
  /** Absolute rectangle of a slot; false if the slot is empty. */
  static bool slotRect(const FaceLayout &layout, uint8_t slot, int16_t w,
                       FaceRect &out);
  /** Rectangle of the bottom status line. */
  static FaceRect statusRect(int16_t w, int16_t h);

  static constexpr int16_t kContentTop = 12;
  static constexpr int16_t kStatusTop = 452;

 private:
  void drawSlot(Adafruit_GFX &gfx, const FaceRect &r, SlotConfig slot,
                const FaceInput &in);
  void drawTime(Adafruit_GFX &gfx, const FaceRect &r, uint8_t variant,
                const struct tm *now);
  void drawDate(Adafruit_GFX &gfx, const FaceRect &r, uint8_t variant,
                const struct tm *now);
  void drawGreeting(Adafruit_GFX &gfx, const FaceRect &r, uint8_t variant,
                    const struct tm *now);
  void drawWeather(Adafruit_GFX &gfx, const FaceRect &r, uint8_t variant,
                   const WeatherSnapshot &wx, const struct tm *now);
  void drawStatus(Adafruit_GFX &gfx, int16_t w, int16_t h, const char *status);
};

/** Line-art weather icon centred on (cx, cy), roughly size×size. */
void faceDrawIcon(Adafruit_GFX &gfx, WxIcon icon, int16_t cx, int16_t cy,
                  int16_t size);
