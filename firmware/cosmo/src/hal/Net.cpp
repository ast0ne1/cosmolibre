#include "hal/Net.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <time.h>
#include <coredecls.h>

#include "generated/portal_head.h"

static const char *kApName = "CosmoLibre";

// After a drop: try to re-join every kRetryMs; each attempt gets kAttemptMs of
// radio quiet (no automatic e-paper refresh), leaving a gap for the clock to
// update between attempts.
static constexpr uint32_t kRetryMs = 90000;
static constexpr uint32_t kAttemptMs = 60000;

// Join at full power (known-good, weak signal here); once connected the radio
// naps between beacons (modem sleep) and drops to minimum power during e-paper
// refreshes — on battery the old always-awake, full-power radio browned out.
static constexpr float kTxDbm = 20.5f;
// 0 dBm was too quiet: the clock dropped off the router (RSSI -61) after the
// first refresh. 10 dBm still cuts most of the TX current.
static constexpr float kQuietTxDbm = 10.0f;

static volatile uint8_t g_discReason = 0;
static WiFiEventHandler g_discHandler;

static volatile bool g_timeCallback = false;

static void onTimeSet(bool from_sntp) {
  if (from_sntp) {
    g_timeCallback = true;
  }
}

/** Disconnect STA without wiping stored SSID/password. */
static void disconnectKeepCreds() {
  WiFi.persistent(false);
  WiFi.disconnect(false);
  WiFi.persistent(true);
  delay(100);
}

/**
 * After WiFiManager has called begin(ssid,pass) with persistence,
 * force SoftAP down, re-commit STA config, reboot into a clean join.
 */
static void onSaveConfig() {
  String ssid = WiFi.SSID();
  String pass = WiFi.psk();
  Serial.printf("Commit WiFi ssid=%s psk_len=%u\n", ssid.c_str(),
                (unsigned)pass.length());
  Serial.flush();

  WiFi.persistent(true);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    delay(400);
  }
  Serial.println(F("Rebooting to join home Wi‑Fi"));
  Serial.flush();
  delay(400);
  ESP.restart();
}

const char *Net::stateName(NetState s) {
  switch (s) {
    case NetState::Connecting:
      return "wifi...";
    case NetState::Portal:
      return "AP setup";
    case NetState::Online:
      return "wifi ok";
    case NetState::NtpWait:
      return "ntp...";
    case NetState::Synced:
      return "ntp ok";
    case NetState::Failed:
      return "wifi fail";
    case NetState::Asleep:
    case NetState::Waking:
      return "";  // normal in power saver: keep the face clean
    default:
      return "net idle";
  }
}

bool Net::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }

bool Net::associating() const {
  return (state_ == NetState::Failed && attemptMs_ != 0) ||
         state_ == NetState::Waking;
}

void Net::cacheIp() {
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    if (ip[0] != 0 && !(ip[0] == 192 && ip[1] == 168 && ip[2] == 4)) {
      strncpy(ip_, ip.toString().c_str(), sizeof(ip_) - 1);
      ip_[sizeof(ip_) - 1] = 0;
      return;
    }
  }
  ip_[0] = 0;
}

bool Net::staUp() const {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress ip = WiFi.localIP();
  if (ip[0] == 0 || (ip[0] == 192 && ip[1] == 168 && ip[2] == 4)) {
    return false;
  }
  return true;
}

static bool waitSta(uint32_t timeoutMs, NetProgressFn onProgress) {
  uint32_t t0 = millis();
  uint32_t lastUi = 0;
  char msg[20];
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      if (ip[0] != 0 && !(ip[0] == 192 && ip[1] == 168 && ip[2] == 4)) {
        return true;
      }
    }
    uint32_t elapsed = (millis() - t0) / 1000;
    if (onProgress && elapsed != lastUi) {
      lastUi = elapsed;
      snprintf(msg, sizeof(msg), "wifi %lus", elapsed);
      onProgress(msg);
    }
    delay(200);
    yield();
    ESP.wdtFeed();
  }
  return false;
}

static void configurePortal(WiFiManager &wm) {
  wm.setConfigPortalTimeout(300);
  wm.setHostname("cosmolibre");
  // Must use connect=true so ESP8266 persists PSK correctly.
  wm.setSaveConnect(true);
  wm.setConnectTimeout(10);
  wm.setConnectRetries(1);
  wm.setBreakAfterConfig(true);
  wm.setSaveConfigCallback(onSaveConfig);
}

static void runPortal(NetProgressFn onProgress) {
  if (onProgress) {
    onProgress("AP setup");
  }
  WiFiManager wm;
  configurePortal(wm);
  // Look only: web/portal.html turns the scan list into a dropdown. Pages,
  // /wifisave and the save callback stay WiFiManager's. The copy lives on the
  // heap only while the portal runs.
  String head = FPSTR(kPortalHead);
  wm.setCustomHeadElement(head.c_str());
  wm.setTitle(F("CosmoLibre"));
  const char *menu[] = {"wifi"};
  wm.setMenu(menu, 1);
  Serial.println(F("Portal CosmoLibre → http://192.168.4.1"));
  Serial.println(F("Save → commit password → reboot → join"));
  wm.startConfigPortal(kApName);
}

void Net::begin(NetProgressFn onProgress) {
  WiFi.persistent(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(kTxDbm);
  WiFi.hostname(F("cosmolibre"));
  state_ = NetState::Connecting;
  synced_ = false;
  ip_[0] = 0;

  settimeofday_cb(onTimeSet);
  g_discHandler = WiFi.onStationModeDisconnected(
      [](const WiFiEventStationModeDisconnected &e) { g_discReason = e.reason; });

  WiFi.mode(WIFI_STA);
  delay(200);

  String ssid = WiFi.SSID();
  String psk = WiFi.psk();
  Serial.printf("Boot SSID='%s' psk_len=%u\n", ssid.c_str(),
                (unsigned)psk.length());
  strncpy(ssid_, ssid.c_str(), sizeof(ssid_) - 1);
  strncpy(psk_, psk.c_str(), sizeof(psk_) - 1);

  if (ssid.length() == 0 || psk.length() == 0) {
    Serial.println(F("Missing SSID or password — portal"));
    state_ = NetState::Portal;
    runPortal(onProgress);
    state_ = NetState::Failed;
    if (onProgress) {
      onProgress("wifi fail");
    }
    return;
  }

  Serial.printf("Trying saved SSID: %s\n", ssid.c_str());
  if (onProgress) {
    char joinMsg[28];
    snprintf(joinMsg, sizeof(joinMsg), "join %.20s", ssid.c_str());
    onProgress(joinMsg);
  }

  // Prefer SDK-stored creds (begin()) — matches the session that got NTP working.
  // Do NOT open SoftAP on failure: that kept the radio from clean STA retries.
  WiFi.setAutoReconnect(true);
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    Serial.printf("STA try %u/2\n", attempt);
    if (attempt > 1) {
      disconnectKeepCreds();
      delay(500);
      WiFi.begin(ssid.c_str(), psk.c_str());
    } else {
      WiFi.begin();
    }
    if (waitSta(35000, onProgress)) {
      cacheIp();
      Serial.printf("WiFi OK ip=%s RSSI=%d ssid=%s\n", ip_, WiFi.RSSI(),
                    ssid.c_str());
      MDNS.begin("cosmolibre");
      state_ = NetState::Online;
      awakeSinceMs_ = millis();
      woke_ = true;
      startNtp();
      return;
    }
    Serial.printf("STA try %u failed status=%d\n", attempt,
                  (int)WiFi.status());
  }

  int st = (int)WiFi.status();
  Serial.printf("Join failed status=%d ssid=%s — stay STA, retry in loop "
                "(hold Middle for portal)\n",
                st, ssid.c_str());
  // Keep trying in background; SoftAP only via hold-Middle / empty creds.
  WiFi.begin(ssid.c_str(), psk.c_str());
  state_ = NetState::Failed;
  if (onProgress) {
    onProgress(st == 6 ? "bad password" : "wifi fail");
  }
}

void Net::setTimezone(const char *posixTz) {
  if (!posixTz || !posixTz[0]) {
    return;
  }
  strncpy(tz_, posixTz, sizeof(tz_) - 1);
  tz_[sizeof(tz_) - 1] = 0;
  setenv("TZ", tz_, 1);
  tzset();
  // If already waiting/synced, refresh interpretation of UTC instantly.
  if (state_ == NetState::NtpWait || state_ == NetState::Synced ||
      state_ == NetState::Online) {
    // keep synced_ if we already have UTC; localtime will use new TZ
  }
  Serial.printf("TZ set %s\n", tz_);
}

uint8_t Net::lastDisconnectReason() const { return g_discReason; }

/*
 * Power saver radio switching. Both directions run with persistent(false), so
 * nothing is written to flash: the SDK's stored SSID/PSK stay as the portal
 * saved them, and WiFi.disconnect() is never called (with persistence on it
 * erases the stored credentials).
 */
void Net::radioDown(uint32_t sleepMs) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // current mode only
  WiFi.forceSleepBegin();
  WiFi.persistent(true);
  state_ = NetState::Asleep;
  ip_[0] = 0;
  attemptMs_ = 0;
  downSinceMs_ = 0;
  nextWakeMs_ = millis() + sleepMs;
  Serial.printf("WiFi asleep, next wake in %lus\n", (unsigned long)(sleepMs / 1000));
}

void Net::radioUp() {
  g_discReason = 0;
  WiFi.forceSleepWake();
  delay(1);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Same radio settings as the boot join that works; startNtp() switches to
  // modem sleep once connected.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(kTxDbm);
  WiFi.begin(ssid_, psk_);  // RAM copy of the stored credentials; no flash write
  WiFi.persistent(true);
  state_ = NetState::Waking;
  awakeSinceMs_ = millis();
  Serial.println(F("WiFi waking"));
}

void Net::setPowerSaver(bool on) {
  saver_ = on;
  if (!on && state_ == NetState::Asleep) {
    wakeNow_ = true;  // always-on: come back now and stay
  }
}

void Net::stayAwake(uint32_t ms) {
  const uint32_t until = millis() + ms;
  if (awakeUntilMs_ == 0 || (int32_t)(until - awakeUntilMs_) > 0) {
    awakeUntilMs_ = until ? until : 1;
  }
  if (state_ == NetState::Asleep) {
    wakeNow_ = true;
  } else if (state_ == NetState::Failed) {
    retryAtMs_ = millis();  // retry now instead of waiting up to 90 s
  }
}

void Net::radioQuiet(bool quiet) {
  if (state_ == NetState::Asleep) {
    return;  // radio is off anyway
  }
  WiFi.setOutputPower(quiet ? kQuietTxDbm : kTxDbm);
}

void Net::startNtp() {
  // Connected (first join, recovery or portal): let the radio nap between
  // beacons from now on. The join itself stays on the known-good NONE_SLEEP.
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  setenv("TZ", tz_, 1);
  tzset();
  g_timeCallback = false;
  ntpRound_ = 0;
  restartNtp();
  state_ = NetState::NtpWait;
  ntpStartedMs_ = millis();
  lastNtpAttemptMs_ = 0;
  // A re-sync after reconnect must not blank a clock that already has time.
  synced_ = time(nullptr) > 1704067200;
  resyncing_ = synced_;  // already has time: wait for a real SNTP reply
  Serial.printf("NTP started TZ=%s\n", tz_);
}

void Net::restartNtp() {
  switch (ntpRound_ % 4) {
    case 0:
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      break;
    case 1:
      configTime(0, 0, "0.europe.pool.ntp.org", "1.europe.pool.ntp.org");
      break;
    case 2:
      configTime(0, 0, "time.google.com", "time.windows.com");
      break;
    default:
      configTime(0, 0, "216.239.35.0", "216.239.35.4");
      break;
  }
  Serial.printf("NTP servers round %u\n", ntpRound_);
  ntpRound_++;
}

bool Net::trySync() {
  // Need an actual SNTP reply (startNtp() clears the flag): a clock that
  // already has time would otherwise count as synced at once, and power saver
  // would switch the radio off before the drift was corrected.
  if (!g_timeCallback && resyncing_) {
    return false;
  }
  g_timeCallback = false;
  time_t now = time(nullptr);
  if (now > 1704067200) {
    synced_ = true;
    state_ = NetState::Synced;
    struct tm ti;
    localtime_r(&now, &ti);
    Serial.printf("NTP synced %04d-%02d-%02d %02d:%02d:%02d (ip=%s)\n",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour,
                  ti.tm_min, ti.tm_sec, ip_);
    return true;
  }
  return false;
}

void Net::update() {
  const uint32_t nowMs = millis();
  if (awakeUntilMs_ && (int32_t)(nowMs - awakeUntilMs_) >= 0) {
    awakeUntilMs_ = 0;
  }

  if (state_ == NetState::Asleep) {
    if (!ssid_[0]) {
      return;  // nothing to join (setup via the portal)
    }
    const bool due = (int32_t)(nowMs - nextWakeMs_) >= 0;
    if (wakeNow_ || (due && goodMoment_)) {
      wakeNow_ = false;
      radioUp();
    }
    return;
  }

  if (state_ == NetState::Waking) {
    if (staUp()) {
      cacheIp();
      Serial.printf("WiFi awake ip=%s RSSI=%d (%lums)\n", ip_, WiFi.RSSI(),
                    (unsigned long)(nowMs - awakeSinceMs_));
      MDNS.notifyAPChange();
      wakeFailReason_ = 0;
      woke_ = true;
      state_ = NetState::Online;
      startNtp();
    } else if (nowMs - awakeSinceMs_ > kAttemptMs) {
      wakeFailReason_ = g_discReason ? g_discReason : 255;
      Serial.printf("WiFi wake failed (reason %u)\n", g_discReason);
      radioDown(kSleepAfterFailMs);
    }
    return;
  }

  if (saver_ && synced_ && awakeUntilMs_ == 0) {
    // Synced and nobody needs the network: radio off until the next sync.
    if (state_ == NetState::Synced && othersDone_ && nowMs - awakeSinceMs_ > 5000) {
      radioDown(kSleepMs);
      return;
    }
    // A sync that drags on (NTP unreachable...) mustn't keep the radio up.
    if (nowMs - awakeSinceMs_ > kMaxAwakeMs) {
      Serial.println(F("WiFi sync window over"));
      if (state_ == NetState::Failed) {
        wakeFailReason_ = g_discReason ? g_discReason : 255;
      }
      radioDown(state_ == NetState::Synced ? kSleepMs : kSleepAfterFailMs);
      return;
    }
  }

  if (state_ == NetState::Failed) {
    if (staUp()) {
      cacheIp();
      Serial.printf("WiFi recovered ip=%s\n", ip_);
      attemptMs_ = 0;
      state_ = NetState::Online;
      startNtp();
      return;
    }
    const uint32_t now = millis();
    if (attemptMs_ && now - attemptMs_ >= kAttemptMs) {
      attemptMs_ = 0;  // give the display a turn
    }
    if (retryAtMs_ == 0 || (int32_t)(now - retryAtMs_) >= 0) {
      retryAtMs_ = now + kRetryMs;
      attemptMs_ = now ? now : 1;
      Serial.printf("WiFi retry (last reason %u)\n", g_discReason);
      // Same radio settings as the boot join that works (startNtp() goes back
      // to modem sleep once connected).
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      WiFi.setOutputPower(kTxDbm);
      // SDK-stored credentials; begin() without args writes nothing to flash.
      WiFi.begin();
    }
    return;
  }

  if (!staUp()) {
    if (state_ != NetState::Portal && state_ != NetState::Connecting) {
      // Keep synced_: the RTC keeps running offline, so the clock stays valid.
      if (downSinceMs_ == 0) {
        downSinceMs_ = millis();
      }
      if (millis() - downSinceMs_ > 8000) {
        state_ = NetState::Failed;
        ip_[0] = 0;
        downSinceMs_ = 0;
        retryAtMs_ = millis() + 5000;
        attemptMs_ = 0;
        Serial.printf("WiFi lost (reason %u)\n", g_discReason);
      }
    }
    return;
  }

  // Link is up: a brief blip that recovered must not count toward the next one.
  downSinceMs_ = 0;
  cacheIp();

  if (state_ == NetState::Online) {
    startNtp();
  }

  if (state_ == NetState::NtpWait) {
    uint32_t now = millis();
    if (now - lastNtpAttemptMs_ >= 500) {
      lastNtpAttemptMs_ = now;
      if (trySync()) {
        return;
      }
    }
    if (now - ntpStartedMs_ > 15000) {
      Serial.printf("NTP still waiting t=%lu ip=%s\n", now, ip_);
      restartNtp();
      ntpStartedMs_ = now;
    }
  }
}

void Net::startPortal() {
  state_ = NetState::Portal;
  synced_ = false;
  disconnectKeepCreds();
  runPortal(nullptr);
  if (staUp()) {
    cacheIp();
    MDNS.begin("cosmolibre");
    state_ = NetState::Online;
    startNtp();
  } else {
    state_ = NetState::Failed;
  }
}

void Net::resync() {
  if (state_ == NetState::Asleep) {
    wakeNow_ = true;
  } else if (staUp()) {
    state_ = NetState::Online;  // update() restarts NTP from here
  } else if (state_ == NetState::Failed) {
    retryAtMs_ = millis();       // reconnect attempt on the next update()
  }
}

void Net::forgetAndReboot() {
  WiFiManager wm;
  wm.resetSettings();
  WiFi.persistent(true);
  WiFi.disconnect(true);
  ESP.eraseConfig();
  delay(200);
  ESP.restart();
}

bool Net::formatLocalTime(char *timeBuf, size_t timeLen, char *dateBuf,
                          size_t dateLen) {
  if (!synced_) {
    return false;
  }
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  if (timeBuf && timeLen) {
    snprintf(timeBuf, timeLen, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
  }
  if (dateBuf && dateLen) {
    snprintf(dateBuf, dateLen, "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1,
             ti.tm_mday);
  }
  return true;
}
