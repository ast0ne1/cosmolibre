#pragma once

#include <stddef.h>
#include <stdint.h>

/** Line-art icon families drawn by Face (see Face::drawIcon). */
enum class WxIcon : uint8_t {
  Sun,
  PartlyCloudy,
  Cloud,
  Fog,
  Drizzle,
  Rain,
  Snow,
  Storm,
  Drop,   // details: rain chance
  Waves,  // details: humidity
};

/** Open-Meteo WMO weather code → icon. */
inline WxIcon wxIconForCode(uint8_t code) {
  if (code <= 1) return WxIcon::Sun;
  if (code == 2) return WxIcon::PartlyCloudy;
  if (code == 3) return WxIcon::Cloud;
  if (code <= 48) return WxIcon::Fog;
  if (code <= 57) return WxIcon::Drizzle;
  if (code <= 67) return WxIcon::Rain;
  if (code <= 77) return WxIcon::Snow;
  if (code <= 82) return WxIcon::Rain;
  if (code <= 86) return WxIcon::Snow;
  if (code >= 95) return WxIcon::Storm;
  return WxIcon::Cloud;
}

/** Short label (≤ 7 chars), copied into buf (≥ 8). */
const char *wxShortLabel(uint8_t code, char *buf, size_t len);

/** OEM "Weather Description" style label (≤ 16 chars), copied into buf (≥ 17). */
const char *wxDescription(uint8_t code, char *buf, size_t len);
