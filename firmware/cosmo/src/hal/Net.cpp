#include "hal/Net.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <time.h>
#include <coredecls.h>

#include "generated/portal_head.h"

static const char *kApName = "CosmoLibre";

// A join attempt gets kAttemptMs with the radio on (and no automatic e-paper
// refresh). If it fails the radio is switched OFF until the next attempt: a
// station that can't find its network otherwise scans non-stop at full power,
// which is the most expensive thing this clock can do.
static constexpr uint32_t kAttemptMs = 60000;
// Seconds until the next attempt, by number of failures in a row.
static const uint16_t kBackoffS[] = {120, 300, 600, 900, 1800};
// Connected but no time server answered: try again after this long.
static constexpr uint32_t kSleepNoTimeMs = 10UL * 60UL * 1000UL;
// wakeFailReason_ values that aren't SDK disconnect reasons.
static constexpr uint8_t kReasonUnknown = 255;
static constexpr uint8_t kReasonNoTime = 254;

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
  return state_ == NetState::Failed || state_ == NetState::Waking;
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

/**
 * Join from a RAM copy of the credentials — the same call as a power saver
 * wake, which has always connected within seconds. persistent(false): only
 * the SDK's *current* config is set, nothing is written to flash.
 *
 * Why not WiFi.begin() / begin(ssid, psk) with persistence on: begin() with
 * persistence compares against the *flash* config and, when that matches,
 * leaves the current config alone. After WiFi.disconnect() (which empties the
 * current config) that meant connecting with an empty SSID — the old second
 * try and every background retry could never succeed.
 *
 * wifi_station_disconnect() is the SDK call: it stops the connect attempt the
 * SDK starts by itself at power-on, so we join from a clean state. It erases
 * nothing (unlike WiFi.disconnect()).
 */
static void staJoin(const char *ssid, const char *psk) {
  wifi_station_disconnect();
  delay(100);
  WiFi.persistent(false);
  WiFi.begin(ssid, psk);
  WiFi.persistent(true);
}

/** Boot log: what the SDK has stored, and which channels it may use. */
static void logStationSetup() {
  struct station_config sc;
  memset(&sc, 0, sizeof(sc));
  wifi_station_get_config_default(&sc);
  Serial.printf("Stored config: bssid_set=%u auth>=%u rssi>=%d\n", sc.bssid_set,
                (unsigned)sc.threshold.authmode, sc.threshold.rssi);
  wifi_country_t c;
  memset(&c, 0, sizeof(c));
  if (wifi_get_country(&c)) {
    Serial.printf("Country %.2s channels %u-%u policy=%u\n", c.cc, c.schan,
                  c.schan + c.nchan - 1, c.policy);
  }
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
    // Portal closed without a save (a save reboots): nothing to join, so the
    // radio stays off until the hotspot is opened again from the menu.
    wakeFailReason_ = kReasonUnknown;
    radioDown(0);
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

  // Do NOT open SoftAP on failure: that kept the radio from clean STA retries.
  logStationSetup();
  WiFi.setAutoReconnect(true);
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    Serial.printf("STA try %u/2\n", attempt);
    staJoin(ssid_, psk_);
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
  // Retries continue from update(), with the radio off in between. SoftAP only
  // via the menu / empty creds.
  const uint8_t why = g_discReason;  // before the scan's own disconnect (8)
  scanForDiagnostics();
  joinFailed(why);
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
  WiFi.persistent(false);  // before forceSleepWake(): it restores a mode too
  WiFi.forceSleepWake();
  delay(1);
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

/**
 * Look only, after a join has already failed: what can the radio see?
 * scanNetworks() stops the station with the SDK's wifi_station_disconnect(),
 * not WiFi.disconnect(), so the stored credentials are untouched.
 */
void Net::scanForDiagnostics() {
  scanNote_[0] = 0;
  const int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    Serial.printf("Scan: no networks at all (%d)\n", n);
    snprintf(scanNote_, sizeof(scanNote_), "none");
    return;
  }
  int best = -1;
  uint16_t channels = 0;  // bit c = something on channel c
  for (int i = 0; i < n; i++) {
    const int ch = WiFi.channel(i);
    if (ch > 0 && ch < 16) {
      channels |= 1u << ch;
    }
    if (WiFi.SSID(i) == ssid_ && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) {
      best = i;
    }
  }
  char chList[40] = "";
  for (int ch = 1; ch < 16; ch++) {
    if (channels & (1u << ch)) {
      const size_t len = strlen(chList);
      snprintf(chList + len, sizeof(chList) - len, len ? ",%d" : "%d", ch);
    }
  }
  Serial.printf("Scan: channels in use %s\n", chList);
  if (best < 0) {
    Serial.printf("Scan: %d networks, '%s' not among them\n", n, ssid_);
    snprintf(scanNote_, sizeof(scanNote_), "0/%d", n);
  } else {
    Serial.printf("Scan: %d networks, '%s' at %d dBm on channel %d\n", n, ssid_,
                  (int)WiFi.RSSI(best), (int)WiFi.channel(best));
    snprintf(scanNote_, sizeof(scanNote_), "%d c%d", (int)WiFi.RSSI(best),
             (int)WiFi.channel(best));
  }
  WiFi.scanDelete();
}

void Net::joinFailed(uint8_t reason) {
  wakeFailReason_ = reason ? reason : kReasonUnknown;
  const uint8_t last = sizeof(kBackoffS) / sizeof(kBackoffS[0]) - 1;
  uint32_t waitS = kBackoffS[failCount_ < last ? failCount_ : last];
  if (failCount_ < 255) {
    failCount_++;
  }
  if (awakeUntilMs_) {
    waitS = 20;  // someone is waiting for the network (Connect / web UI)
  }
  Serial.printf("WiFi join failed (reason %u, %u in a row)\n", reason, failCount_);
  radioDown(waitS * 1000UL);
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
    wakeNow_ = true;  // also cuts a retry back-off short
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
  // The zone goes in with the servers: configTime(0, 0, ...) resets TZ to
  // UTC, so every re-sync (each power saver wake) put the clock on UTC.
  switch (ntpRound_ % 4) {
    case 0:
      configTime(tz_, "pool.ntp.org", "time.cloudflare.com");
      break;
    case 1:
      configTime(tz_, "0.europe.pool.ntp.org", "1.europe.pool.ntp.org");
      break;
    case 2:
      configTime(tz_, "time.google.com", "time.windows.com");
      break;
    default:
      configTime(tz_, "216.239.35.0", "216.239.35.4");
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
      failCount_ = 0;
      woke_ = true;
      state_ = NetState::Online;
      startNtp();
    } else if (nowMs - awakeSinceMs_ > kAttemptMs) {
      joinFailed(g_discReason);
    }
    return;
  }

  if (state_ == NetState::Failed) {
    // The link dropped and a re-join is in flight (started below).
    if (staUp()) {
      cacheIp();
      Serial.printf("WiFi recovered ip=%s\n", ip_);
      attemptMs_ = 0;
      wakeFailReason_ = 0;
      failCount_ = 0;
      awakeSinceMs_ = nowMs;
      woke_ = true;
      state_ = NetState::Online;
      startNtp();
    } else if (nowMs - attemptMs_ >= kAttemptMs) {
      joinFailed(g_discReason);  // radio off; update() wakes it for the retry
    }
    return;
  }

  if (saver_ && awakeUntilMs_ == 0) {
    // Synced and nobody needs the network: radio off until the next sync.
    if (state_ == NetState::Synced && othersDone_ && nowMs - awakeSinceMs_ > 5000) {
      radioDown(kSleepMs);
      return;
    }
    // A sync that drags on (no time server, weather stuck...) mustn't keep the
    // radio up. A clock with no time yet gets longer before giving up.
    if (nowMs - awakeSinceMs_ > (synced_ ? kMaxAwakeMs : 3 * kMaxAwakeMs)) {
      Serial.println(F("WiFi sync window over"));
      if (state_ == NetState::Synced) {
        radioDown(kSleepMs);
      } else {
        if (!synced_) {
          wakeFailReason_ = kReasonNoTime;  // joined, but still no time
        }
        radioDown(kSleepNoTimeMs);
      }
      return;
    }
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
        attemptMs_ = nowMs;
        Serial.printf("WiFi lost (reason %u), re-joining\n", g_discReason);
        // Same radio settings as the boot join that works (startNtp() goes
        // back to modem sleep once connected).
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
        WiFi.setOutputPower(kTxDbm);
        staJoin(ssid_, psk_);  // RAM credentials; nothing written to flash
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
  if (state_ == NetState::Asleep) {
    // Power saver has the radio in forced sleep: the hotspot can't start
    // until it's awake again. Nothing here is written to flash.
    WiFi.persistent(false);
    WiFi.forceSleepWake();
    delay(1);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    delay(100);
  }
  state_ = NetState::Portal;
  disconnectKeepCreds();
  runPortal(nullptr);
  if (staUp()) {
    cacheIp();
    MDNS.begin("cosmolibre");
    wakeFailReason_ = 0;
    failCount_ = 0;
    awakeSinceMs_ = millis();
    woke_ = true;
    state_ = NetState::Online;
    startNtp();
  } else {
    // Closed without a save (a save reboots): back to the saved network.
    radioDown(5000);
  }
}

void Net::resync() {
  if (state_ == NetState::Asleep) {
    wakeNow_ = true;
  } else if (staUp()) {
    state_ = NetState::Online;  // update() restarts NTP from here
  }
  // Failed: a re-join is already in flight.
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
