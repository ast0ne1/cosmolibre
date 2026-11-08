#include "hal/WeatherCodes.h"

#include <Arduino.h>

// Text tables live in flash (ESP8266 keeps plain const strings in RAM).

struct WxText {
  uint8_t code;
  char text[17];
};

static const WxText kDescriptions[] PROGMEM = {
    {0, "CLEAR SKY"},        {1, "MAINLY CLEAR"},     {2, "SLIGHTLY CLOUDY"},
    {3, "OVERCAST"},         {45, "FOG"},             {48, "FOG"},
    {51, "LIGHT DRIZZLE"},   {53, "LIGHT DRIZZLE"},   {55, "DRIZZLE"},
    {56, "FREEZING DRIZZLE"}, {57, "FREEZING DRIZZLE"}, {61, "LIGHT RAIN"},
    {63, "RAIN"},            {65, "HEAVY RAIN"},      {66, "FREEZING RAIN"},
    {67, "FREEZING RAIN"},   {71, "LIGHT SNOW"},      {73, "SNOW"},
    {75, "HEAVY SNOW"},      {77, "SNOW GRAINS"},     {80, "LIGHT SHOWERS"},
    {81, "SHOWERS"},         {82, "HEAVY SHOWERS"},   {85, "SNOW SHOWERS"},
    {86, "SNOW SHOWERS"},    {95, "THUNDERSTORM"},    {96, "STORM AND HAIL"},
    {99, "STORM AND HAIL"},
};

static const char kShort[9][8] PROGMEM = {"CLEAR", "CLOUD", "FOG",
                                          "DRIZZLE", "RAIN", "SNOW",
                                          "SHOWERS", "STORM", "WX"};

static const char *copyOut(const char *pgm, char *buf, size_t len) {
  strncpy_P(buf, pgm, len - 1);
  buf[len - 1] = 0;
  return buf;
}

const char *wxShortLabel(uint8_t code, char *buf, size_t len) {
  uint8_t i;
  if (code == 0) i = 0;
  else if (code <= 3) i = 1;
  else if (code <= 48) i = 2;
  else if (code <= 57) i = 3;
  else if (code <= 67) i = 4;
  else if (code <= 77) i = 5;
  else if (code <= 82) i = 6;
  else if (code <= 86) i = 5;
  else if (code >= 95) i = 7;
  else i = 8;
  return copyOut(kShort[i], buf, len);
}

const char *wxDescription(uint8_t code, char *buf, size_t len) {
  for (const WxText &w : kDescriptions) {
    if (pgm_read_byte(&w.code) == code) {
      return copyOut(w.text, buf, len);
    }
  }
  return wxShortLabel(code, buf, len);
}
