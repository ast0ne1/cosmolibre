#include "hal/WebUI.h"
#include "hal/Net.h"
#include "hal/Settings.h"
#include "hal/Weather.h"
#include "hal/Face.h"
#include "hal/FaceBitmap.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <time.h>

// Generated from web/index.html by scripts/embed_web.py (pre-build step).
#include "generated/web_index.h"

#include "hal/Version.h"
static const char kWriteHeader[] = "X-Cosmo";

// ---- JSON helpers --------------------------------------------------------

static void jsonStr(String &o, const char *s) {
  o += '"';
  for (const char *p = s ? s : ""; *p; p++) {
    const char c = *p;
    switch (c) {
      case '"':
        o += F("\\\"");
        break;
      case '\\':
        o += F("\\\\");
        break;
      case '\n':
        o += F("\\n");
        break;
      case '\r':
        o += F("\\r");
        break;
      case '\t':
        o += F("\\t");
        break;
      default:
        if ((uint8_t)c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)(uint8_t)c);
          o += esc;
        } else {
          o += c;
        }
    }
  }
  o += '"';
}

/** Same as jsonStr for a flash string (F("...")). */
static void jsonStr(String &o, const __FlashStringHelper *fs) {
  char buf[64];
  strncpy_P(buf, (PGM_P)fs, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  jsonStr(o, buf);
}

/** Append an integer explicitly (String's char overloads would add a char). */
static void jsonNum(String &o, long v) { o += String(v); }

static void jsonSlot(String &o, SlotConfig s) {
  o += F("{\"kind\":");
  jsonNum(o, s.kind);
  o += F(",\"variant\":");
  jsonNum(o, s.variant);
  o += '}';
}

// ---- lifecycle -----------------------------------------------------------

void WebUI::begin(Net *net, Settings *settings, Weather *weather, Face *face) {
  net_ = net;
  settings_ = settings;
  weather_ = weather;
  face_ = face;
  wantPortal_ = false;
  wantForget_ = false;
  wantBeep_ = false;
  wantFaceRefresh_ = false;
  wantWebUiOff_ = false;
  if (!routesBound_) {
    server_.collectHeaders(kWriteHeader);
    server_.on("/", HTTP_GET, [this]() { handleIndex(); });
    server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
    server_.on("/api/config", HTTP_GET, [this]() { handleConfig(); });
    server_.on("/api/face.bmp", HTTP_GET, [this]() { handleFaceBmp(); });
    server_.on("/api/widget.bmp", HTTP_GET, [this]() { handleWidgetBmp(); });
    server_.on("/api/geocode", HTTP_GET, [this]() { handleGeocode(); });
    server_.on("/api/settings", HTTP_POST, [this]() { handleSettingsPost(); });
    server_.on("/api/face", HTTP_POST, [this]() { handleFacePost(); });
    server_.on("/api/pages", HTTP_POST, [this]() { handlePagesPost(); });
    server_.on("/api/wifi", HTTP_POST, [this]() { handleWifi(); });
    server_.on("/api/forget", HTTP_POST, [this]() { handleForget(); });
    server_.on("/api/beep", HTTP_POST, [this]() { handleBeep(); });
    server_.on("/api/webui", HTTP_POST, [this]() { handleWebUiOff(); });
    server_.onNotFound([this]() { handleNotFound(); });
    routesBound_ = true;
  }
  stop();
  ensureStarted();
}

void WebUI::stop() {
  if (!started_) {
    return;
  }
  server_.stop();
  if (mdnsService_) {
    MDNS.removeService(mdnsService_);
    mdnsService_ = nullptr;
  }
  started_ = false;
}

void WebUI::setEnabled(bool on) {
  if (on == enabled_) {
    return;
  }
  enabled_ = on;
  Serial.printf("WebUI %s\n", on ? "enabled" : "disabled");
  if (!on) {
    stop();
  }
}

void WebUI::ensureStarted() {
  if (!enabled_ || !net_ || !net_->wifiConnected() || !net_->ip()[0]) {
    return;
  }
  if (started_) {
    return;
  }
  server_.begin();
  mdnsService_ = MDNS.addService(nullptr, "http", "tcp", 80);
  started_ = true;
  Serial.printf("WebUI http://%s/\n", net_->ip());
}

void WebUI::update() {
  if (!net_) {
    return;
  }
  if (!enabled_) {
    MDNS.update();  // keep answering cosmolibre.local (no http advert)
    return;
  }
  if (!net_->wifiConnected() || !net_->ip()[0]) {
    stop();
    return;
  }
  ensureStarted();
  if (started_) {
    server_.handleClient();
    MDNS.update();
  }
}

bool WebUI::consumePortalRequest() {
  if (!wantPortal_) return false;
  wantPortal_ = false;
  return true;
}
bool WebUI::consumeForgetRequest() {
  if (!wantForget_) return false;
  wantForget_ = false;
  return true;
}
bool WebUI::consumeBeepRequest() {
  if (!wantBeep_) return false;
  wantBeep_ = false;
  return true;
}
bool WebUI::consumeWebUiOff() {
  if (!wantWebUiOff_) return false;
  wantWebUiOff_ = false;
  return true;
}
bool WebUI::consumeFaceRefresh() {
  if (!wantFaceRefresh_) return false;
  wantFaceRefresh_ = false;
  return true;
}

// ---- response helpers ----------------------------------------------------

void WebUI::sendJson(int code, const String &body) {
  server_.sendHeader(F("Cache-Control"), F("no-store"));
  server_.send(code, F("application/json"), body);
}

void WebUI::sendOk(const __FlashStringHelper *warning) {
  String o = F("{\"ok\":true");
  if (warning) {
    o += F(",\"warning\":");
    jsonStr(o, warning);
  }
  o += '}';
  sendJson(200, o);
}

void WebUI::sendError(int code, const __FlashStringHelper *msg) {
  String o = F("{\"ok\":false,\"error\":");
  jsonStr(o, msg);
  o += '}';
  sendJson(code, o);
}

bool WebUI::requireWrite() {
  // Browsers cannot add custom headers to cross-origin form posts without a
  // CORS preflight (which we never approve), so this blocks drive-by CSRF.
  if (!server_.hasHeader(kWriteHeader)) {
    sendError(403, F("missing X-Cosmo header"));
    return false;
  }
  if (!settings_ || !net_) {
    sendError(503, F("unavailable"));
    return false;
  }
  return true;
}

// ---- handlers ------------------------------------------------------------

void WebUI::handleIndex() {
  server_.sendHeader(F("Content-Encoding"), F("gzip"));
  server_.sendHeader(F("Cache-Control"), F("no-cache"));
  server_.send_P(200, PSTR("text/html; charset=utf-8"),
                 (PGM_P)kWebIndexGz, kWebIndexGzLen);
}

void WebUI::handleStatus() {
  activity_ = true;
  String o;
  o.reserve(900);
  o += '{';
  o += F("\"fw\":\"" COSMOLIBRE_VERSION "\"");
  o += F(",\"uptime\":");
  jsonNum(o, (uint32_t)(millis() / 1000));
  o += F(",\"heap\":");
  jsonNum(o, ESP.getFreeHeap());
  o += F(",\"ip\":");
  jsonStr(o, net_ ? net_->ip() : "");
  o += F(",\"ssid\":");
  jsonStr(o, WiFi.SSID().c_str());
  o += F(",\"rssi\":");
  jsonNum(o, WiFi.RSSI());
  o += F(",\"state\":");
  jsonStr(o, net_ ? Net::stateName(net_->state()) : "");
  o += F(",\"stateId\":");
  jsonNum(o, net_ ? (int)net_->state() : 0);

  const bool synced = net_ && net_->timeSynced();
  o += F(",\"synced\":");
  o += synced ? F("true") : F("false");
  if (synced) {
    time_t now = time(nullptr);
    struct tm ti;
    localtime_r(&now, &ti);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min,
             ti.tm_sec);
    o += F(",\"time\":");
    jsonStr(o, buf);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ti.tm_year + 1900,
             ti.tm_mon + 1, ti.tm_mday);
    o += F(",\"date\":");
    jsonStr(o, buf);
  }

  if (settings_) {
    o += F(",\"tz\":");
    jsonStr(o, settings_->tzLabel());
    o += F(",\"location\":");
    if (settings_->hasLocation()) {
      jsonStr(o, settings_->locationLabel());
    } else {
      o += F("null");
    }
    o += F(",\"slots\":[");
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      if (i) o += ',';
      jsonSlot(o, settings_->slot(i));
    }
    o += ']';
  }

  if (weather_) {
    const WeatherSnapshot &wx = weather_->snap();
    o += F(",\"weather\":{\"valid\":");
    o += wx.valid ? F("true") : F("false");
    if (wx.valid) {
      o += F(",\"tempNow\":");
      jsonNum(o, wx.tempNow);
      o += F(",\"codeNow\":");
      jsonNum(o, wx.codeNow);
      o += F(",\"label\":");
      char label[17];
      jsonStr(o, wxDescription(wx.codeNow, label, sizeof(label)));
      o += F(",\"cloud\":");
      jsonNum(o, wx.cloudNow);
      o += F(",\"humidity\":");
      jsonNum(o, wx.humidityNow);
      o += F(",\"ageSec\":");
      jsonNum(o, (uint32_t)((millis() - wx.fetchedMs) / 1000));
      o += F(",\"days\":[");
      for (int i = 0; i < 3; i++) {
        if (i) o += ',';
        o += F("{\"wd\":");
        jsonStr(o, wx.days[i].weekday);
        o += F(",\"max\":");
        jsonNum(o, wx.days[i].tMax);
        o += F(",\"min\":");
        jsonNum(o, wx.days[i].tMin);
        o += F(",\"code\":");
        jsonNum(o, wx.days[i].code);
        o += F(",\"rain\":");
        jsonNum(o, wx.days[i].precipProb);
        o += F(",\"label\":");
        char shortLabel[8];
        jsonStr(o, wxShortLabel(wx.days[i].code, shortLabel, sizeof(shortLabel)));
        o += '}';
      }
      o += ']';
    }
    o += '}';
  }
  o += '}';
  sendJson(200, o);
}

void WebUI::handleConfig() {
  if (!settings_) {
    sendError(503, F("unavailable"));
    return;
  }
  String o;
  o.reserve(1600);
  o += F("{\"tzId\":");
  jsonNum(o, settings_->tzId());
  o += F(",\"timezones\":[");
  for (uint8_t i = 0; i < Settings::tzCount(); i++) {
    if (i) o += ',';
    jsonStr(o, Settings::tzLabelAt(i));
  }
  o += F("],\"beepOnSync\":");
  o += settings_->beepOnSync() ? F("true") : F("false");
  o += F(",\"showIp\":");
  o += settings_->showIp() ? F("true") : F("false");
  o += F(",\"location\":");
  if (settings_->hasLocation()) {
    o += F("{\"label\":");
    jsonStr(o, settings_->locationLabel());
    o += F(",\"lat\":");
    o += String(settings_->latitude(), 4);
    o += F(",\"lon\":");
    o += String(settings_->longitude(), 4);
    o += '}';
  } else {
    o += F("null");
  }
  o += F(",\"slotCount\":");
  jsonNum(o, kFaceSlots);
  o += F(",\"slots\":[");
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    if (i) o += ',';
    jsonSlot(o, settings_->slot(i));
  }
  const UiConfig &ui = settings_->ui();
  o += F("],\"maxPages\":");
  jsonNum(o, kMaxFaces);
  o += F(",\"pages\":[");
  for (uint8_t f = 0; f < kMaxFaces; f++) {
    if (f) o += ',';
    o += '[';
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      if (i) o += ',';
      jsonSlot(o, ui.faces[f].slot[i]);
    }
    o += ']';
  }
  o += F("],\"currentPage\":");
  jsonNum(o, ui.currentFace);
  o += F(",\"rotate\":");
  o += ui.rotate ? F("true") : F("false");
  o += F(",\"rotateMinutes\":");
  jsonNum(o, ui.rotateMinutes);
  o += F(",\"rotateChoices\":[");
  for (uint8_t i = 0; i < sizeof(kRotateChoices); i++) {
    if (i) o += ',';
    jsonNum(o, kRotateChoices[i]);
  }
  o += F("],\"presets\":[");
  for (uint8_t i = 0; i < Settings::presetCount(); i++) {
    FaceLayout p{};
    Settings::presetLayoutAt(i, p);
    if (i) o += ',';
    o += F("{\"name\":");
    jsonStr(o, Settings::presetNameAt(i));
    o += F(",\"slots\":[");
    for (uint8_t j = 0; j < kFaceSlots; j++) {
      if (j) o += ',';
      jsonSlot(o, p.slot[j]);
    }
    o += F("]}");
  }
  o += F("],\"slotOptions\":[");
  for (uint8_t i = 0; i < slotOptionCount(); i++) {
    const SlotOption opt = slotOptionAt(i);
    if (i) o += ',';
    o += F("{\"kind\":");
    jsonNum(o, opt.kind);
    o += F(",\"variant\":");
    jsonNum(o, opt.variant);
    o += F(",\"label\":");
    jsonStr(o, opt.label);
    o += '}';
  }
  o += F("]}");
  sendJson(200, o);
}

void WebUI::handleGeocode() {
  String q = server_.arg("q");
  q.trim();
  if (!weather_ || q.length() == 0 || q.length() > 64) {
    sendError(400, F("enter a city name"));
    return;
  }
  static constexpr int kMax = 5;
  CityHit hits[kMax];
  const int n = weather_->searchCities(q.c_str(), hits, kMax);
  if (n < 0) {
    sendError(502, F("lookup failed — check the internet connection"));
    return;
  }
  String o = F("{\"ok\":true,\"results\":[");
  for (int i = 0; i < n; i++) {
    if (i) o += ',';
    o += F("{\"name\":");
    jsonStr(o, hits[i].name);
    o += F(",\"region\":");
    jsonStr(o, hits[i].region);
    o += F(",\"country\":");
    jsonStr(o, hits[i].country);
    o += F(",\"lat\":");
    o += String(hits[i].lat, 4);
    o += F(",\"lon\":");
    o += String(hits[i].lon, 4);
    o += '}';
  }
  o += F("]}");
  sendJson(200, o);
}

void WebUI::handleSettingsPost() {
  if (!requireWrite()) {
    return;
  }
  const __FlashStringHelper *warning = nullptr;
  if (server_.hasArg("tz")) {
    settings_->setTzId((uint8_t)server_.arg("tz").toInt());
  }
  if (server_.hasArg("showIp")) {
    settings_->setShowIp(server_.arg("showIp") == "1");
  }
  if (server_.hasArg("beep")) {
    settings_->setBeepOnSync(server_.arg("beep") == "1");
  }
  if (server_.hasArg("city")) {
    String city = server_.arg("city");
    city.trim();
    const bool hasCoords = server_.hasArg("lat") && server_.hasArg("lon");
    const float lat = server_.arg("lat").toFloat();
    const float lon = server_.arg("lon").toFloat();
    if (city.length() == 0) {
      settings_->clearLocation();
    } else if (hasCoords && lat >= -90 && lat <= 90 && lon >= -180 &&
               lon <= 180) {
      // Candidate picked in the UI from /api/geocode.
      settings_->setLocation(lat, lon, city.c_str());
      if (weather_) weather_->refreshSoon();
    } else if (!settings_->hasLocation() ||
               city != settings_->locationLabel()) {
      // Name only (no pick): take the best match.
      CityHit hit{};
      if (weather_ && weather_->searchCities(city.c_str(), &hit, 1) == 1) {
        settings_->setLocation(hit.lat, hit.lon, hit.name[0] ? hit.name : city.c_str());
        weather_->refreshSoon();
      } else {
        Serial.println(F("Geocode failed"));
        warning = F("City not found — location unchanged");
      }
    }
  }
  settings_->save();
  net_->setTimezone(settings_->tzPosix());
  wantFaceRefresh_ = true;
  sendOk(warning);
}

static bool parseKindVar(const String &s, uint8_t &kind, uint8_t &var) {
  int c = s.indexOf(':');
  if (c < 0) {
    return false;
  }
  kind = (uint8_t)s.substring(0, c).toInt();
  var = (uint8_t)s.substring(c + 1).toInt();
  return isValidSlot(kind, var);
}

static const char *const kSlotArgs[kFaceSlots] = {"s0", "s1", "s2"};

void WebUI::handleFacePost() {
  if (!requireWrite()) {
    return;
  }
  if (server_.hasArg("preset") && server_.arg("preset").length()) {
    settings_->applyPreset((uint8_t)server_.arg("preset").toInt());
  } else {
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      uint8_t k = 0, v = 0;
      if (server_.hasArg(kSlotArgs[i]) &&
          parseKindVar(server_.arg(kSlotArgs[i]), k, v)) {
        settings_->setSlot(i, k, v);
      }
    }
  }
  settings_->save();
  wantFaceRefresh_ = true;
  sendOk();
}

static const char *const kPageArgs[kMaxFaces] = {"p0", "p1", "p2", "p3"};

/** p0..p3 = "k:v,k:v,k:v"; current; rotate=0/1; every=<minutes>. */
void WebUI::handlePagesPost() {
  if (!requireWrite()) {
    return;
  }
  FaceLayout pages[kMaxFaces]{};
  for (uint8_t f = 0; f < kMaxFaces; f++) {
    const String arg = server_.arg(kPageArgs[f]);
    int start = 0;
    for (uint8_t i = 0; i < kFaceSlots && start <= (int)arg.length(); i++) {
      int comma = arg.indexOf(',', start);
      if (comma < 0) comma = arg.length();
      uint8_t k = 0, v = 0;
      if (parseKindVar(arg.substring(start, comma), k, v)) {
        pages[f].slot[i] = {k, v};
      }
      start = comma + 1;
    }
  }
  const uint8_t current = (uint8_t)server_.arg("current").toInt();
  const bool rotate = server_.arg("rotate") == "1";
  const uint8_t every = (uint8_t)server_.arg("every").toInt();
  if (!settings_->setPages(pages, current, rotate, every)) {
    sendError(400, F("keep at least one page"));
    return;
  }
  settings_->save();
  wantFaceRefresh_ = true;
  sendOk();
}

static void bmpSink(void *ctx, const uint8_t *data, size_t len) {
  static_cast<ESP8266WebServer *>(ctx)->sendContent((const char *)data, len);
}

static void bmpYield() {
  yield();
  ESP.wdtFeed();
}

namespace {
struct WidgetDraw {
  Face *face;
  FaceRect r;
  SlotConfig slot;
  const FaceInput *in;
};
void drawWidgetBand(Adafruit_GFX &g, void *ctx) {
  WidgetDraw *d = static_cast<WidgetDraw *>(ctx);
  g.fillScreen(GxEPD_WHITE);
  d->face->drawWidget(g, d->r, d->slot, *d->in);
}
}  // namespace

/** One widget at its natural size — thumbnails for the web UI toolbox. */
void WebUI::handleWidgetBmp() {
  uint8_t k = 0, v = 0;
  if (!face_ || !weather_ || !parseKindVar(server_.arg("w"), k, v)) {
    sendError(400, F("bad widget"));
    return;
  }
  struct tm tmBuf;
  const struct tm *now = nullptr;
  if (net_ && net_->timeSynced()) {
    time_t t = time(nullptr);
    localtime_r(&t, &tmBuf);
    now = &tmBuf;
  }
  static const FaceLayout kEmpty{};
  const FaceInput in{&kEmpty, &weather_->snap(), now, ""};
  const SlotConfig slot{k, v};
  WidgetDraw d{face_, {0, 0, 280, Face::widgetHeight(slot)}, slot, &in};
  server_.sendHeader(F("Cache-Control"), F("no-store"));
  server_.setContentLength(faceBmpSize(280, d.r.h));
  server_.send(200, F("image/bmp"), "");
  renderBmp(280, d.r.h, drawWidgetBand, &d, bmpSink, &server_, bmpYield);
}

void WebUI::handleFaceBmp() {
  if (!settings_ || !face_ || !weather_) {
    sendError(503, F("unavailable"));
    return;
  }
  // Optional draft layout (?s0=1:0&s1=4:0&s2=0:0) so the editor can preview
  // unsaved changes with real time/weather.
  bool draft = false;
  for (uint8_t i = 0; i < kFaceSlots; i++) draft |= server_.hasArg(kSlotArgs[i]);
  if (!draft && screenFn_) {
    server_.sendHeader(F("Cache-Control"), F("no-store"));
    server_.setContentLength(faceBmpSize(280, 480));
    server_.send(200, F("image/bmp"), "");
    renderBmp(280, 480, screenFn_, screenCtx_, bmpSink, &server_, bmpYield);
    return;
  }
  FaceLayout layout = settings_->layout();
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    uint8_t k = 0, v = 0;
    if (server_.hasArg(kSlotArgs[i]) &&
        parseKindVar(server_.arg(kSlotArgs[i]), k, v)) {
      layout.slot[i] = {k, v};
    }
  }
  struct tm tmBuf;
  const struct tm *now = nullptr;
  if (net_ && net_->timeSynced()) {
    time_t t = time(nullptr);
    localtime_r(&t, &tmBuf);
    now = &tmBuf;
  }
  const char *status = net_ ? net_->ip() : "";
  const FaceInput in{&layout, &weather_->snap(), now, status};

  constexpr int16_t kW = 280, kH = 480;
  server_.sendHeader(F("Cache-Control"), F("no-store"));
  server_.setContentLength(faceBmpSize(kW, kH));
  server_.send(200, F("image/bmp"), "");
  renderFaceBmp(*face_, kW, kH, in, bmpSink, &server_, bmpYield);
}

void WebUI::handleWifi() {
  if (!requireWrite()) {
    return;
  }
  wantPortal_ = true;
  sendOk();
}

void WebUI::handleForget() {
  if (!requireWrite()) {
    return;
  }
  wantForget_ = true;
  sendOk();
}

void WebUI::handleBeep() {
  if (!requireWrite()) {
    return;
  }
  wantBeep_ = true;
  sendOk();
}

/** Only "off" is accepted here: turning it back on happens on the device. */
void WebUI::handleWebUiOff() {
  if (!requireWrite()) {
    return;
  }
  if (server_.arg("on") != "0") {
    sendError(400, F("the web UI can only be turned on from the clock"));
    return;
  }
  wantWebUiOff_ = true;  // main stops the server after this reply is sent
  sendOk();
}

void WebUI::handleNotFound() {
  if (server_.uri().startsWith(F("/api/"))) {
    sendError(404, F("not found"));
    return;
  }
  // Old bookmarks (/face, /settings) land on the single-page UI.
  server_.sendHeader(F("Location"), F("/"), true);
  server_.send(302, F("text/plain"), "");
}
