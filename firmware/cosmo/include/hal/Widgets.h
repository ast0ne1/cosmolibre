#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Widget catalogue — modelled on the OEM "Add Element" picker of the
 * original app. In scope: Time, Date, Weather, Greeting.

 *
 * Numeric values are persisted in EEPROM: append, never renumber.
 */

enum class WidgetKind : uint8_t {
  None = 0,
  Time = 1,
  Greeting = 2,
  Weather = 3,
  Date = 4,
};

enum class TimeVariant : uint8_t {
  Big24 = 0,    // OEM "Time Big": stacked HH / MM serif
  Big12 = 1,
  Small24 = 2,  // OEM "Time Small": HH:MM serif
  Small12 = 3,
  Words = 4,    // TWENTY / THREE
};

enum class WeatherVariant : uint8_t {
  Now = 0,          // icon + temperature + high/low
  Day = 1,          // OEM "Temperature And Day": icon + temp + weekday
  Days3 = 2,        // three-day forecast
  Description = 3,  // OEM "Weather Description": SLIGHTLY CLOUDY / 29°C
  Details = 4,      // OEM "Weather Details": cloud % · rain % · humidity %
  Big = 5,          // OEM "Weather Big": large icon
};

enum class DateVariant : uint8_t {
  DayMonth = 0,  // OEM "Date And Day": 04.11 / WEDNESDAY
  MonthDay = 1,  // 11.04 / WEDNESDAY
  Long = 2,      // 4 NOVEMBER / WEDNESDAY
};

enum class GreetingVariant : uint8_t {
  Default = 0,   // GOOD MORNING
  Sentence = 1,  // Good morning
  Lower = 2,     // good morning
};

struct SlotConfig {
  uint8_t kind;
  uint8_t variant;
};

static constexpr uint8_t kFaceSlots = 3;

/** Widgets stacked top to bottom; None slots are skipped. */
struct FaceLayout {
  SlotConfig slot[kFaceSlots];
};

struct SlotOption {
  uint8_t kind;
  uint8_t variant;
  char label[24];  // UTF-8
};

// Widget picker catalogue — lives in flash (src/hal/Widgets.cpp).
uint8_t slotOptionCount();
/** Copy of entry i (out of range → None). */
SlotOption slotOptionAt(uint8_t i);
bool isValidSlot(uint8_t kind, uint8_t variant);

/** Relative height of a widget when slots share the screen (0 = hidden). */
inline uint8_t widgetWeight(SlotConfig s) {
  switch ((WidgetKind)s.kind) {
    case WidgetKind::None:
      return 0;
    case WidgetKind::Time:
      if (s.variant == (uint8_t)TimeVariant::Big24 ||
          s.variant == (uint8_t)TimeVariant::Big12) {
        return 22;
      }
      return s.variant == (uint8_t)TimeVariant::Words ? 14 : 10;
    case WidgetKind::Weather:
      if (s.variant == (uint8_t)WeatherVariant::Days3 ||
          s.variant == (uint8_t)WeatherVariant::Big) {
        return 14;
      }
      return 10;
    default:
      return 10;
  }
}

/** Greeting for local hour 0–23, copied into buf. */
const char *greetingForHour(int hour, char *buf, size_t len);
