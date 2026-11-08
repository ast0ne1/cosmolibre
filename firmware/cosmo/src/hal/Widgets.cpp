#include "hal/Widgets.h"

#include <Arduino.h>

// In flash: on the ESP8266, plain const data and string literals live in RAM.
static const SlotOption kSlotOptions[] PROGMEM = {
    {(uint8_t)WidgetKind::None, 0, "None"},
    {(uint8_t)WidgetKind::Time, (uint8_t)TimeVariant::Big24, "Time · Big 24h"},
    {(uint8_t)WidgetKind::Time, (uint8_t)TimeVariant::Big12, "Time · Big 12h"},
    {(uint8_t)WidgetKind::Time, (uint8_t)TimeVariant::Small24, "Time · Small 24h"},
    {(uint8_t)WidgetKind::Time, (uint8_t)TimeVariant::Small12, "Time · Small 12h"},
    {(uint8_t)WidgetKind::Time, (uint8_t)TimeVariant::Words, "Time · Words"},
    {(uint8_t)WidgetKind::Date, (uint8_t)DateVariant::DayMonth, "Date · Day.Month"},
    {(uint8_t)WidgetKind::Date, (uint8_t)DateVariant::MonthDay, "Date · Month.Day"},
    {(uint8_t)WidgetKind::Date, (uint8_t)DateVariant::Long, "Date · Long"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Now, "Weather · Now"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Day, "Weather · Temp & day"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Description, "Weather · Description"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Details, "Weather · Details"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Big, "Weather · Big icon"},
    {(uint8_t)WidgetKind::Weather, (uint8_t)WeatherVariant::Days3, "Weather · 3 days"},
    {(uint8_t)WidgetKind::Greeting, (uint8_t)GreetingVariant::Default, "Greeting · Capitals"},
    {(uint8_t)WidgetKind::Greeting, (uint8_t)GreetingVariant::Sentence, "Greeting · Sentence"},
    {(uint8_t)WidgetKind::Greeting, (uint8_t)GreetingVariant::Lower, "Greeting · Lowercase"},
};
static constexpr uint8_t kSlotOptionCount =
    sizeof(kSlotOptions) / sizeof(kSlotOptions[0]);

uint8_t slotOptionCount() { return kSlotOptionCount; }

SlotOption slotOptionAt(uint8_t i) {
  SlotOption o{};
  if (i < kSlotOptionCount) {
    memcpy_P(&o, &kSlotOptions[i], sizeof(o));
  } else {
    strncpy(o.label, "None", sizeof(o.label) - 1);
  }
  return o;
}

bool isValidSlot(uint8_t kind, uint8_t variant) {
  for (uint8_t i = 0; i < kSlotOptionCount; i++) {
    if (pgm_read_byte(&kSlotOptions[i].kind) == kind &&
        pgm_read_byte(&kSlotOptions[i].variant) == variant) {
      return true;
    }
  }
  return false;
}

static const char kGreetings[4][16] PROGMEM = {
    "GOOD MORNING", "GOOD AFTERNOON", "GOOD EVENING", "GOOD NIGHT"};

const char *greetingForHour(int hour, char *buf, size_t len) {
  int i = 3;
  if (hour >= 5 && hour < 11) {
    i = 0;
  } else if (hour >= 11 && hour < 17) {
    i = 1;
  } else if (hour >= 17 && hour < 21) {
    i = 2;
  }
  strncpy_P(buf, kGreetings[i], len - 1);
  buf[len - 1] = 0;
  return buf;
}
