#include "hal/Weather.h"

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <memory>
#include <time.h>

static constexpr uint32_t kFetchIntervalMs = 45UL * 60UL * 1000UL;

void Weather::begin() {
  snap_ = {};
  nextFetchMs_ = 0;
  updated_ = false;
}

static bool httpsGet(const String &url, String &body) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  // BearSSL's default 16 KB receive buffer doesn't fit next to the web server:
  // a city search from the web UI ran the heap dry and the clock restarted.
  // Open-Meteo's servers honour TLS "max fragment length", so ask for 2 KB
  // records (saves about 14 KB). Replies are a few KB at most.
  client->setBufferSizes(2048, 512);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(*client, url)) {
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d (heap %u) %s\n", code, ESP.getFreeHeap(), url.c_str());
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return body.length() > 0;
}

/**
 * Bounds of a top-level JSON object value, e.g. section(body, "current").
 * Open-Meteo puts "current_units" (same keys, unit strings) *before* the
 * values, so every lookup must be scoped to the right section.
 */
static bool section(const String &s, const char *name, int &from, int &to) {
  String key = String('"') + name + F("\":{");
  int i = s.indexOf(key);
  if (i < 0) {
    return false;
  }
  from = i + key.length();
  int depth = 1;
  for (int j = from; j < (int)s.length(); j++) {
    if (s[j] == '{' || s[j] == '[') depth++;
    if (s[j] == '}' || s[j] == ']') depth--;
    if (depth == 0) {
      to = j;
      return true;
    }
  }
  return false;
}

/** Index just past `"key":` within [from, to), or -1. */
static int valueAt(const String &s, const char *key, int from, int to) {
  String k = String('"') + key + F("\":");
  int i = s.indexOf(k, from);
  if (i < 0 || i >= to) {
    return -1;
  }
  return i + k.length();
}

static bool parseFloatIn(const String &s, const char *key, int from, int to,
                         float &out) {
  int v = valueAt(s, key, from, to);
  if (v < 0) {
    return false;
  }
  out = s.substring(v, v + 16).toFloat();
  return true;
}

static bool parseIntArrayIn(const String &s, const char *key, int from, int to,
                            int *out, int maxN, int &count) {
  count = 0;
  int v = valueAt(s, key, from, to);
  if (v < 0 || s[v] != '[') {
    return false;
  }
  int end = s.indexOf(']', v);
  if (end < 0 || end > to) {
    return false;
  }
  int p = v + 1;
  while (p < end && count < maxN) {
    while (p < end && (s[p] == ' ' || s[p] == ',')) p++;
    if (p >= end) break;
    // toFloat handles "12.4" and rounds properly below; null → 0.
    out[count++] = (int)lroundf(s.substring(p, p + 12).toFloat());
    int comma = s.indexOf(',', p);
    if (comma < 0 || comma > end) break;
    p = comma + 1;
  }
  return count > 0;
}

/** Copy the JSON string value of `key` within [from, to) into out. */
static void stringIn(const String &s, const char *key, int from, int to,
                     char *out, size_t len) {
  out[0] = 0;
  int v = valueAt(s, key, from, to);
  if (v < 0 || s[v] != '"') {
    return;
  }
  int end = s.indexOf('"', v + 1);
  if (end < 0 || end > to) {
    return;
  }
  String val = s.substring(v + 1, end);
  strncpy(out, val.c_str(), len - 1);
  out[len - 1] = 0;
}

int Weather::searchCities(const char *name, CityHit *out, int max) {
  if (!name || !name[0] || max <= 0) {
    return 0;
  }
  // Percent-encode everything but unreserved chars (handles &, ?, UTF-8).
  static const char kHex[] = "0123456789ABCDEF";
  String q;
  for (const char *p = name; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      q += (char)c;
    } else {
      q += '%';
      q += kHex[c >> 4];
      q += kHex[c & 0x0F];
    }
  }
  String url =
      String(F("https://geocoding-api.open-meteo.com/v1/search?name=")) + q +
      F("&count=") + max + F("&language=en&format=json");
  String body;
  if (!httpsGet(url, body)) {
    return -1;
  }
  int r = body.indexOf(F("\"results\":["));
  if (r < 0) {
    return 0;  // no matches
  }
  // Walk the top-level objects of the results array.
  int n = 0;
  int p = r + 11;
  while (n < max) {
    int start = body.indexOf('{', p);
    if (start < 0) break;
    int depth = 0, end = -1;
    for (int j = start; j < (int)body.length(); j++) {
      if (body[j] == '{') depth++;
      if (body[j] == '}' && --depth == 0) {
        end = j;
        break;
      }
    }
    if (end < 0) break;
    CityHit &h = out[n];
    float lat = 0, lon = 0;
    if (parseFloatIn(body, "latitude", start, end, lat) &&
        parseFloatIn(body, "longitude", start, end, lon)) {
      h.lat = lat;
      h.lon = lon;
      stringIn(body, "name", start, end, h.name, sizeof(h.name));
      stringIn(body, "admin1", start, end, h.region, sizeof(h.region));
      stringIn(body, "country_code", start, end, h.country, sizeof(h.country));
      n++;
    }
    p = end + 1;
    if (p < (int)body.length() && body[p] == ']') break;
  }
  return n;
}

bool Weather::fetchForecast(float lat, float lon) {
  char url[300];
  snprintf_P(url, sizeof(url),
             PSTR("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,weather_code,cloud_cover,relative_humidity_2m"
                  "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
                  "precipitation_probability_max"
                  "&forecast_days=3&timezone=auto"),
             lat, lon);
  String body;
  if (!httpsGet(String(url), body)) {
    return false;
  }

  int cFrom = 0, cTo = 0, dFrom = 0, dTo = 0;
  if (!section(body, "current", cFrom, cTo) ||
      !section(body, "daily", dFrom, dTo)) {
    Serial.println(F("Weather: unexpected response"));
    return false;
  }

  WeatherSnapshot s = snap_;
  float f = 0;
  if (parseFloatIn(body, "temperature_2m", cFrom, cTo, f)) {
    s.tempNow = (int8_t)lroundf(f);
  }
  if (parseFloatIn(body, "weather_code", cFrom, cTo, f)) {
    s.codeNow = (uint8_t)f;
  }
  if (parseFloatIn(body, "cloud_cover", cFrom, cTo, f)) {
    s.cloudNow = (uint8_t)constrain(lroundf(f), 0, 100);
  }
  if (parseFloatIn(body, "relative_humidity_2m", cFrom, cTo, f)) {
    s.humidityNow = (uint8_t)constrain(lroundf(f), 0, 100);
  }

  int tmax[3] = {0}, tmin[3] = {0}, codes[3] = {0}, prob[3] = {0};
  int nMax = 0, nMin = 0, nCode = 0, nProb = 0;
  parseIntArrayIn(body, "temperature_2m_max", dFrom, dTo, tmax, 3, nMax);
  parseIntArrayIn(body, "temperature_2m_min", dFrom, dTo, tmin, 3, nMin);
  parseIntArrayIn(body, "weather_code", dFrom, dTo, codes, 3, nCode);
  parseIntArrayIn(body, "precipitation_probability_max", dFrom, dTo, prob, 3,
                  nProb);

  time_t now = time(nullptr);
  for (int i = 0; i < 3; i++) {
    s.days[i].tMax = (int8_t)(i < nMax ? tmax[i] : 0);
    s.days[i].tMin = (int8_t)(i < nMin ? tmin[i] : 0);
    s.days[i].code = (uint8_t)(i < nCode ? codes[i] : 0);
    s.days[i].precipProb = (uint8_t)(i < nProb ? constrain(prob[i], 0, 100) : 0);
    if (now > 1704067200) {
      time_t day = now + (time_t)i * 86400;
      struct tm ti;
      localtime_r(&day, &ti);
      static const char kDow[7][4] PROGMEM = {"SUN", "MON", "TUE", "WED",
                                              "THU", "FRI", "SAT"};
      strncpy_P(s.days[i].weekday, kDow[ti.tm_wday % 7],
                sizeof(s.days[i].weekday) - 1);
    } else {
      snprintf(s.days[i].weekday, sizeof(s.days[i].weekday), "D%d", i);
    }
  }
  s.valid = true;
  s.fetchedMs = millis();
  snap_ = s;
  updated_ = true;
  char label[8];
  Serial.printf_P(PSTR("Weather OK now=%d %s cloud=%u hum=%u days=%d\n"),
                  snap_.tempNow, wxShortLabel(snap_.codeNow, label, sizeof(label)),
                  snap_.cloudNow, snap_.humidityNow, nMax);
  return true;
}

void Weather::update(float lat, float lon, bool hasLocation) {
  if (!hasLocation) {
    return;
  }
  uint32_t now = millis();
  if (nextFetchMs_ != 0 && (int32_t)(now - nextFetchMs_) < 0) {
    return;
  }
  if (fetchForecast(lat, lon)) {
    nextFetchMs_ = now + kFetchIntervalMs;
  } else {
    nextFetchMs_ = now + 120000;  // retry in 2 min
  }
}
