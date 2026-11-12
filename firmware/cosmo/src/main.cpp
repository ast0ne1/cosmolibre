/**
 * CosmoLibre — pages of widgets, touch menu, alarms/timer (ED037TC1, touch,
 * audio, Wi‑Fi/NTP, WebUI). On-device UX: docs/device-ux.md (logic in Ui).
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <time.h>
#include <GxEPD2_BW.h>

#include "hal/Touch.h"
#include "hal/Audio.h"
#include "hal/Net.h"
#include "hal/WebUI.h"
#include "hal/Settings.h"
#include "hal/Weather.h"
#include "hal/Face.h"
#include "hal/Ui.h"

GxEPD2_BW<GxEPD2_370_TC1, GxEPD2_370_TC1::HEIGHT / 2> display(
    GxEPD2_370_TC1(/*CS=*/15, /*DC=*/4, /*RST=*/5, /*BUSY=*/16));

static Touch g_touch;
static Audio g_audio;
static Net g_net;
static WebUI g_web;
static Settings g_settings;
static Weather g_weather;
static Face g_face;                 // splash + web thumbnails
static Ui g_ui(g_settings.ui());    // pads, menu, pages, alarms (draws the screen)

// Partial refreshes leave ghosting on this panel: force a full refresh at
// least hourly and after this many partial updates.
static constexpr uint8_t kMaxPartials = 30;

static char g_status[40] = "starting";
static char g_ipStr[16] = "";
static char g_lastOverlay[24] = "";
static bool g_needFull = true;
static bool g_needScreen = false;  // whole-panel partial (menu navigation)
static bool g_needStatus = false;
static bool g_needSlot[kFaceSlots] = {};
static uint8_t g_partials = 0;
static uint32_t g_lastSec = 0;
static NetState g_lastNetState = NetState::Idle;
static bool g_wasSynced = false;
static int g_lastMinute = -1;
static int g_lastHour = -1;
static uint32_t g_lastPadMs = 0;
static bool g_weatherPending = false;  // power saver: fetch before sleeping

static void refreshStatusFromNet() {
  if (g_net.ip()[0]) {
    strncpy(g_ipStr, g_net.ip(), sizeof(g_ipStr) - 1);
  }
  if (g_net.state() == NetState::NtpWait && g_ipStr[0]) {
    snprintf(g_status, sizeof(g_status), "ntp %s", g_ipStr);
  } else if (g_net.state() == NetState::Synced) {
    // All good: keep the face clean unless the user opted in to seeing the IP
    // (it's always in Menu › Info and the web UI).
    if (g_settings.showIp() && g_ipStr[0]) {
      snprintf(g_status, sizeof(g_status), "%s", g_ipStr);
    } else {
      g_status[0] = 0;
    }
  } else if (g_net.state() == NetState::Asleep || g_net.state() == NetState::Waking) {
    // Power saver: radio off between syncs is normal. Only a failed wake shows.
    if (g_net.wakeFailReason()) {
      // Reason code, plus what the radio could see after a failed boot join
      // ("none", "0/7", "-61 c6": see Net::scanNote), so a clock with no
      // serial cable can still be diagnosed from its screen.
      snprintf(g_status, sizeof(g_status), "wifi r%u%s%s", g_net.wakeFailReason(),
               g_net.scanNote()[0] ? " " : "", g_net.scanNote());
    } else {
      g_status[0] = 0;
    }
  } else if (g_net.state() == NetState::Failed && g_net.lastDisconnectReason()) {
    // The SDK's reason code, so a drop on battery (no serial) can be read off
    // the screen.
    snprintf(g_status, sizeof(g_status), "wifi fail r%u", g_net.lastDisconnectReason());
  } else {
    snprintf(g_status, sizeof(g_status), "%s", Net::stateName(g_net.state()));
  }
}

/** Local time, or nullptr until NTP has synced. */
static const struct tm *localNow(struct tm &buf) {
  if (!g_net.timeSynced()) {
    return nullptr;
  }
  time_t t = time(nullptr);
  localtime_r(&t, &buf);
  return &buf;
}

/** Snapshot of everything the face draws; `tmBuf` must outlive the result. */
static FaceInput faceInput(struct tm &tmBuf) {
  return FaceInput{&g_settings.layout(), &g_weather.snap(), localNow(tmBuf),
                   g_status};
}

static bool faceShowing() { return !g_ui.inMenu() && !g_ui.ringing(); }

static void markSlots(WidgetKind kind) {
  if (!faceShowing()) {
    return;  // the menu/alarm card is up; the next full refresh catches up
  }
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    if ((WidgetKind)g_settings.slot(i).kind == kind) {
      g_needSlot[i] = true;
    }
  }
}

/**
 * Draw whatever the UI shows (face, menu, alarm card). Full refresh, or a
 * partial refresh of one window — GxEPD2 clips drawing to the window.
 */
static void paint(bool full, int16_t x = 0, int16_t y = 0, int16_t w = 0,
                  int16_t h = 0) {
  struct tm tmBuf;
  const FaceInput in = faceInput(tmBuf);
  if (full) {
    ESP.wdtDisable();
    display.setFullWindow();
  } else {
    ESP.wdtFeed();
    display.setPartialWindow(x, y, w, h);
  }
  g_net.radioQuiet(true);  // no full-power Wi‑Fi burst during the refresh
  display.firstPage();
  do {
    g_ui.draw(display, display.width(), display.height(), in, millis());
    yield();
  } while (display.nextPage());
  g_net.radioQuiet(false);
  if (full) {
    ESP.wdtEnable(8000);
    g_partials = 0;
  } else {
    g_partials++;
  }
}

static void paintSlot(uint8_t slot) {
  FaceRect r;
  if (Face::slotRect(g_settings.layout(), slot, display.width(), r)) {
    paint(false, r.x, r.y, r.w, r.h);
  }
}

static void paintStatus() {
  const FaceRect r = Face::statusRect(display.width(), display.height());
  paint(false, r.x, r.y, r.w, r.h);
}

/** /api/face.bmp without a draft: exactly what the panel shows. */
static void drawScreenForWeb(Adafruit_GFX &gfx, void *) {
  struct tm tmBuf;
  const FaceInput in = faceInput(tmBuf);
  g_ui.draw(gfx, 280, 480, in, millis());
}

static void paintJoiningSplash() {
  ESP.wdtDisable();
  display.setFullWindow();
  g_net.radioQuiet(true);
  display.firstPage();
  do {
    g_face.drawSplash(display, display.width(), display.height(), g_status);
    yield();
  } while (display.nextPage());
  g_net.radioQuiet(false);
  ESP.wdtEnable(8000);
}

/** Shown while the setup hotspot runs (it blocks): the original's layout. */
static void paintWifiSetupCard() {
  const Card card{"WIFI SETUP", CardIcon::Wifi, "START WIFI",
                  {"USE YOUR PHONE", "OR PC WIFI TO", "JOIN COSMOLIBRE"},
                  "OPEN 192.168.4.1", false};
  ESP.wdtDisable();
  display.setFullWindow();
  g_net.radioQuiet(true);
  display.firstPage();
  do {
    g_face.drawCard(display, display.width(), display.height(), card);
    yield();
  } while (display.nextPage());
  g_net.radioQuiet(false);
  ESP.wdtEnable(8000);
}

static void openPortal() {
  snprintf(g_status, sizeof(g_status), "WiFi setup: CosmoLibre");
  paintWifiSetupCard();
  g_web.stop();
  g_net.startPortal();  // blocks until saved or timed out
  refreshStatusFromNet();
  g_web.begin(&g_net, &g_settings, &g_weather, &g_face);
  g_needFull = true;
  g_wasSynced = g_net.timeSynced();
}

static void forgetWifi() {
  snprintf(g_status, sizeof(g_status), "forget WiFi...");
  paint(true);
  g_web.stop();
  g_net.forgetAndReboot();
}

/**
 * Sounds requested by the UI. Chimes need the OEM WAVs in flash (step 4 of
 * docs/device-ux.md); until then a chime is a short beep pattern.
 */
static void playSound(int8_t id) {
  if (id == kUiSoundNone) {
    return;
  }
  g_audio.setVolume(g_settings.ui().volume);
  const uint8_t beeps = id == kUiSoundBeep ? 1 : 3;
  for (uint8_t i = 0; i < beeps; i++) {
    if (i) delay(120);
    g_audio.playBeep();
  }
}

static void updateUiStatus() {
  UiStatus st{};
  snprintf(st.ssid, sizeof(st.ssid), "%s", WiFi.SSID().c_str());
  snprintf(st.ip, sizeof(st.ip), "%s", g_net.ip());
  st.rssi = (int16_t)WiFi.RSSI();
  st.wifiOk = g_net.wifiConnected() && g_net.ip()[0];
  st.radioOff = g_net.state() == NetState::Asleep;
  st.connecting = g_net.state() == NetState::Waking ||
                  g_net.state() == NetState::Connecting;
  st.batteryPct = -1;  // battery telemetry not found yet
  st.uptimeS = millis() / 1000;
  g_ui.setStatus(st);
}

static void handleUiOutputs() {
  const uint16_t a = g_ui.takeActions();
  if (a & kUiSaveConfig) {
    g_settings.save();  // EEPROM commit only writes if something changed
  }
  if (a & kUiWebUiChanged) {
    g_web.setEnabled(g_settings.ui().webUi);
  }
  if (a & kUiWifiMode) {
    g_net.setPowerSaver(!g_settings.ui().wifiAlways);
  }
  if (a & kUiWakeWifi) {
    g_net.stayAwake(15UL * 60UL * 1000UL);
  }
  if (a & kUiSyncNow) {
    g_net.resync();
    g_weather.refreshSoon();
  }
  playSound(g_ui.takeSound());
  switch (g_ui.takeRefresh()) {
    case UiRefresh::Full: g_needFull = true; break;
    case UiRefresh::Partial: g_needScreen = true; break;
    default: break;
  }
  if (a & kUiOpenPortal) {
    openPortal();
  }
  if (a & kUiForgetWifi) {
    forgetWifi();
  }
}

static Pad toPad(TouchPad p) {
  return p == TouchPad::Left ? Pad::Left : p == TouchPad::Right ? Pad::Right : Pad::Middle;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("=== CosmoLibre ==="));
  Serial.flush();

  g_touch.begin();
  g_audio.begin();
  g_weather.begin();
  g_net.setTimezone(Settings::tzPosixAt(0));

  SPI.pins(14 /*SCK*/, 12 /*MISO*/, 13 /*MOSI*/, 15 /*SS*/);
  SPI.begin();
  SPI.setFrequency(2000000);

  // initial=false: skip GxEPD2's separate clearing refresh — the splash is a
  // full refresh anyway, and each full refresh is a power spike.
  display.init(115200, false, 50, false);
  display.setRotation(0);
  Serial.printf("EPD %dx%d\n", display.width(), display.height());

  snprintf(g_status, sizeof(g_status), "joining...");
  paintJoiningSplash();

  delay(1200);

#ifdef COSMO_NO_WIFI
  // Power diagnostic build (env:cosmo-nowifi): radio fully off, nothing about
  // Wi‑Fi is written to flash, so saved credentials stay untouched.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
  g_lastNetState = g_net.state();
  snprintf(g_status, sizeof(g_status), "wifi off (test)");
  g_needFull = true;
#else
  Serial.println(F("WiFiManager begin..."));
  Serial.flush();
  g_net.setPowerSaver(!g_settings.ui().wifiAlways);
  g_net.begin([](const char *msg) {
    if (!msg || !msg[0]) {
      return;
    }
    Serial.println(msg);
    snprintf(g_status, sizeof(g_status), "%s", msg);
    if (strcmp(msg, "AP setup") == 0) {
      paintWifiSetupCard();  // no saved Wi‑Fi: the hotspot is about to start
    }
  });

  g_lastNetState = g_net.state();
  refreshStatusFromNet();
  g_needFull = true;
#endif

  g_settings.begin();
  g_net.setPowerSaver(!g_settings.ui().wifiAlways);  // now that settings are loaded
  g_net.setTimezone(g_settings.tzPosix());
  g_audio.setVolume(g_settings.ui().volume);
  // No weather fetch here: loop() fetches once the time has synced (the
  // forecast's weekday labels need it), so fetching now would do it twice.
  g_web.setEnabled(g_settings.ui().webUi);
  g_web.setScreenRenderer(drawScreenForWeb, nullptr);
  g_web.begin(&g_net, &g_settings, &g_weather, &g_face);
  updateUiStatus();

  if (g_net.wifiConnected()) {
    g_audio.playBeep();
  }

  Serial.println(F("--- ready: HOLD MIDDLE = menu, TAP L/R = pages ---"));
  Serial.printf("LAN WebUI: http://%s/%s\n", g_ipStr[0] ? g_ipStr : "(no ip)",
                g_settings.ui().webUi ? "" : " (web UI off)");
  Serial.flush();
}

void loop() {
#ifndef COSMO_NO_WIFI
  {
    // Power saver timing: wake just after the minute refresh (seconds 2-30),
    // sleep once weather has had its turn.
    struct tm tb;
    const struct tm *t = localNow(tb);
    const bool goodMoment = !t || (t->tm_sec >= 2 && t->tm_sec <= 30);
    g_net.setSleepGates(!g_weatherPending, goodMoment);
  }
  g_net.update();
  if (g_net.consumeWoke()) {
    g_weather.refreshSoon();
    g_weatherPending = g_settings.hasLocation();
  }
  if (g_web.consumeActivity()) {
    g_net.stayAwake(3UL * 60UL * 1000UL);  // web page open: keep the radio up
  }
  g_web.update();
#endif
  g_touch.update();

  struct tm tmBuf;
  const struct tm *now = localNow(tmBuf);

  if (g_net.wifiConnected() && g_settings.hasLocation() && g_net.ip()[0] &&
      g_net.timeSynced()) {
    g_weather.update(g_settings.latitude(), g_settings.longitude(), true);
    g_weatherPending = false;  // fetched or failed: either way it had its turn
  }
  if (g_weather.consumeUpdated()) {
    markSlots(WidgetKind::Weather);
  }

  // ---- web requests ----
  if (g_web.consumePortalRequest()) {
    openPortal();
  }
  if (g_web.consumeForgetRequest()) {
    forgetWifi();
  }
  if (g_web.consumeBeepRequest()) {
    playSound(kUiSoundBeep);
  }
  if (g_web.consumeWebUiOff()) {
    g_settings.ui().webUi = false;
    g_settings.save();
    g_web.setEnabled(false);
  }
  if (g_web.consumeFaceRefresh()) {
    refreshStatusFromNet();  // "show IP" may have changed
    g_needFull = true;
  }

  // ---- network state ----
  if (g_net.state() != g_lastNetState ||
      (g_net.ip()[0] && strcmp(g_ipStr, g_net.ip()) != 0)) {
    g_lastNetState = g_net.state();
    // Power saver walks Asleep → Waking → Online → NtpWait → Synced → Asleep
    // every sync: only touch the panel if the visible text really changed.
    char prev[sizeof(g_status)];
    memcpy(prev, g_status, sizeof(prev));
    refreshStatusFromNet();
    if (strcmp(prev, g_status) != 0) {
      g_needStatus = true;
    }
    if (!faceShowing()) {
      g_needScreen = true;  // e.g. Info waiting for the IP
    }
    if (!g_wasSynced &&
        (g_net.state() == NetState::NtpWait || g_net.state() == NetState::Synced)) {
      g_needFull = true;
    }
  }

  if (g_net.timeSynced() && !g_wasSynced) {
    g_wasSynced = true;
    refreshStatusFromNet();
    g_needFull = true;
    if (g_settings.beepOnSync() && !g_ui.quiet(now)) {
      playSound(kUiSoundBeep);
    }
  }

  // ---- touch → UI ----
  TouchSample ev;
  while (g_touch.poll(ev)) {
    if (ev.pad == TouchPad::None) {
      continue;
    }
    const Gesture g = ev.event == TouchEvent::Hold ? Gesture::Hold : Gesture::Tap;
    Serial.printf("[%lu] %s %s\n", millis(), g == Gesture::Hold ? "HOLD" : "TAP",
                  Touch::padName(ev.pad));
    g_ui.onPad(toPad(ev.pad), g, millis(), now);
    g_lastPadMs = millis();
  }

  // Full-screen "Wi‑Fi failed" card only while there's no time to show.
  g_ui.setCard(g_net.joinFailing() && !g_net.timeSynced()
                   ? Ui::Card::WifiFailed
                   : Ui::Card::None);

  // ---- once a second: UI clock (alarms, timer, rotation, menu timeout) ----
  const uint32_t sec = millis() / 1000;
  if (sec != g_lastSec) {
    g_lastSec = sec;
    updateUiStatus();
    g_ui.tick(millis(), now);
    if (now) {
      if (now->tm_hour != g_lastHour) {
        // Hourly full refresh clears ghosting and rolls greeting/date over.
        if (g_lastHour >= 0 && faceShowing()) {
          g_needFull = true;
        }
        g_lastHour = now->tm_hour;
      }
      if (now->tm_min != g_lastMinute) {
        g_lastMinute = now->tm_min;
        markSlots(WidgetKind::Time);
      }
    }
    if (faceShowing()) {
      const char *ov = g_ui.overlayStatus(millis());
      if (strcmp(ov ? ov : "", g_lastOverlay) != 0) {
        snprintf(g_lastOverlay, sizeof(g_lastOverlay), "%s", ov ? ov : "");
        g_needStatus = true;  // timer / snooze countdown changed
      }
    }
  }
  handleUiOutputs();

  // ---- e-paper ----
  // While Wi-Fi is re-joining, hold automatic refreshes (clock ticks, weather,
  // rotation): they stay queued and draw in the gap between attempts. Someone
  // using the pads always gets a response.
  const bool userActive = millis() - g_lastPadMs < 5000;
  if (g_net.associating() && !userActive) {
    yield();
    return;
  }
  if (g_partials >= kMaxPartials) {
    g_needFull = true;
  }
  if (g_needFull) {
    paint(true);
    g_needFull = g_needScreen = g_needStatus = false;
    memset(g_needSlot, 0, sizeof(g_needSlot));
  } else if (g_needScreen) {
    paint(false, 0, 0, display.width(), display.height());
    g_needScreen = g_needStatus = false;
    memset(g_needSlot, 0, sizeof(g_needSlot));
  } else if (faceShowing()) {
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      if (g_needSlot[i]) {
        g_needSlot[i] = false;
        paintSlot(i);
      }
    }
    if (g_needStatus) {
      paintStatus();
      g_needStatus = false;
    }
  }

  yield();
}
