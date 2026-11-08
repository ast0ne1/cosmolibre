#pragma once

#include <stdint.h>
#include <stddef.h>

#include "hal/WeatherCodes.h"

struct WeatherDay {
  int8_t tMax = 0;
  int8_t tMin = 0;
  uint8_t code = 0;
  uint8_t precipProb = 0;  // %
  char weekday[4] = "";
};

struct WeatherSnapshot {
  bool valid = false;
  int8_t tempNow = 0;
  uint8_t codeNow = 0;
  uint8_t cloudNow = 0;     // %
  uint8_t humidityNow = 0;  // %
  WeatherDay days[3]{};
  uint32_t fetchedMs = 0;
};

/** One geocoding candidate (Open-Meteo search). */
struct CityHit {
  char name[32];
  char region[32];  // admin1, e.g. "New South Wales"
  char country[4];  // ISO code, e.g. "AU"
  float lat;
  float lon;
};

#ifdef ARDUINO_ARCH_ESP8266
/** Open-Meteo client — HTTPS with insecure TLS (ESP8266 RAM). */
class Weather {
 public:
  void begin();
  void update(float lat, float lon, bool hasLocation);
  const WeatherSnapshot &snap() const { return snap_; }
  /** True once after each successful fetch (face needs a redraw). */
  bool consumeUpdated() {
    bool u = updated_;
    updated_ = false;
    return u;
  }

  /** Fetch on the next update() (e.g. after the location changed). */
  void refreshSoon() { nextFetchMs_ = 0; }

  /** Up to `max` candidates for a place name (blocking HTTPS); -1 on error. */
  int searchCities(const char *name, CityHit *out, int max);

 private:
  bool fetchForecast(float lat, float lon);
  WeatherSnapshot snap_{};
  uint32_t nextFetchMs_ = 0;
  bool updated_ = false;
};
#endif
