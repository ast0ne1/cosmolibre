#pragma once

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include "hal/FaceBitmap.h"
#include "hal/Widgets.h"

class Net;
class Settings;
class Weather;
class Face;

/**
 * LAN web UI: serves web/index.html (gzipped into flash at build time) plus a
 * small JSON API under /api.
 */
class WebUI {
 public:
  void begin(Net *net, Settings *settings, Weather *weather, Face *face);
  void update();
  void stop();

  bool consumePortalRequest();
  bool consumeForgetRequest();
  bool consumeBeepRequest();
  bool consumeFaceRefresh();
  /** The web page asked to switch the web UI off (only the device turns it on). */
  bool consumeWebUiOff();
  /** True once after the page polled /api/status (someone has it open). */
  bool consumeActivity() {
    const bool a = activity_;
    activity_ = false;
    return a;
  }

  /** Off = HTTP server stopped and the mDNS http advert withdrawn. */
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }

  /** Draws exactly what the panel shows (menus included) for /api/face.bmp. */
  void setScreenRenderer(BmpDrawFn fn, void *ctx) {
    screenFn_ = fn;
    screenCtx_ = ctx;
  }

 private:
  bool activity_ = false;
  void ensureStarted();
  /** POSTs must carry X-Cosmo (blocks cross-site form posts). */
  bool requireWrite();
  void sendJson(int code, const String &body);
  void sendOk(const __FlashStringHelper *warning = nullptr);
  void sendError(int code, const __FlashStringHelper *msg);

  void handleIndex();
  void handleStatus();
  void handleConfig();
  void handleFaceBmp();
  void handleWidgetBmp();
  void handleGeocode();
  void handleSettingsPost();
  void handleFacePost();
  void handlePagesPost();
  void handleWifi();
  void handleForget();
  void handleBeep();
  void handleWebUiOff();
  void handleNotFound();

  Net *net_ = nullptr;
  Settings *settings_ = nullptr;
  Weather *weather_ = nullptr;
  Face *face_ = nullptr;
  ESP8266WebServer server_{80};
  bool routesBound_ = false;
  bool started_ = false;
  bool wantPortal_ = false;
  bool wantForget_ = false;
  bool wantBeep_ = false;
  bool wantFaceRefresh_ = false;
  bool wantWebUiOff_ = false;
  bool enabled_ = true;
  const void *mdnsService_ = nullptr;
  BmpDrawFn screenFn_ = nullptr;
  void *screenCtx_ = nullptr;
};
