#pragma once

#include <Arduino.h>

// Asleep: power saver has the radio off on purpose (the clock keeps time).
// Waking: power saver is re-joining for a scheduled sync.
enum class NetState : uint8_t {
  Idle, Connecting, Portal, Online, NtpWait, Synced, Failed, Asleep, Waking
};

using NetProgressFn = void (*)(const char *msg);

/** Wi‑Fi (WiFiManager) + NTP (SNTP / configTime). */
class Net {
 public:
  void begin(NetProgressFn onProgress = nullptr);
  void update();
  void startPortal();
  void forgetAndReboot();
  /**
   * Drop transmit power to the minimum around an e-paper refresh (true), then
   * restore it (false). On this hardware a full-power Wi‑Fi burst on top of
   * the panel's refresh spike browns the board out when running on battery.
   */
  void radioQuiet(bool quiet);

  /** Menu › Sync clock: re-query NTP now (or wake / retry Wi‑Fi now). */
  void resync();

  /**
   * Power saver: after each sync the radio switches off; it wakes every
   * kSleepMs to re-sync time and weather, then sleeps again. Off = always
   * connected (the old behaviour).
   */
  void setPowerSaver(bool on);
  bool powerSaver() const { return saver_; }
  /** Keep the radio on for at least `ms` from now (wakes it if asleep). */
  void stayAwake(uint32_t ms);
  /** Radio held on by stayAwake() (web UI on demand). */
  bool heldAwake() const { return awakeUntilMs_ != 0; }
  /**
   * main.cpp's say on timing. `othersDone`: nothing else needs the network
   * (weather fetched). `goodMoment`: just after a minute refresh, so a wake's
   * radio burst doesn't land on an e-paper refresh.
   */
  void setSleepGates(bool othersDone, bool goodMoment) {
    othersDone_ = othersDone;
    goodMoment_ = goodMoment;
  }
  /** True once after a scheduled/requested wake connected (fetch weather). */
  bool consumeWoke() {
    const bool w = woke_;
    woke_ = false;
    return w;
  }
  /** Power saver: the last wake couldn't join (reason kept for the screen). */
  uint8_t wakeFailReason() const { return wakeFailReason_; }

  /** Last station disconnect reason from the SDK (0 = none yet), e.g. 200
   *  beacon timeout, 2 auth expired, 15 handshake timeout (wrong password). */
  uint8_t lastDisconnectReason() const;

  /** POSIX TZ string for NTP/localtime — does not affect Wi‑Fi join. */
  void setTimezone(const char *posixTz);
  const char *timezone() const { return tz_; }

  NetState state() const { return state_; }
  bool timeSynced() const { return synced_; }
  bool wifiConnected() const;
  const char *ip() const { return ip_; }

  /**
   * True while a reconnect attempt is in flight. Automatic e-paper refreshes
   * should wait: a refresh during association has been seen to stop the
   * ESP8266 re-joining (status 7) on this hardware.
   */
  bool associating() const;

  bool formatLocalTime(char *timeBuf, size_t timeLen, char *dateBuf = nullptr,
                       size_t dateLen = 0);

  static const char *stateName(NetState s);

 private:
  void startNtp();
  void restartNtp();
  bool trySync();
  void cacheIp();
  bool staUp() const;
  void radioUp();
  void radioDown(uint32_t sleepMs);

  static constexpr uint32_t kSleepMs = 45UL * 60UL * 1000UL;  // between syncs
  static constexpr uint32_t kSleepAfterFailMs = 10UL * 60UL * 1000UL;
  static constexpr uint32_t kMaxAwakeMs = 90UL * 1000UL;  // a scheduled sync

  bool saver_ = true;
  bool othersDone_ = true;
  bool goodMoment_ = true;
  bool wakeNow_ = false;
  bool woke_ = false;
  uint8_t wakeFailReason_ = 0;
  uint32_t nextWakeMs_ = 0;
  uint32_t awakeSinceMs_ = 0;
  uint32_t awakeUntilMs_ = 0;  // stayAwake() hold, 0 = none
  char ssid_[33] = "";         // cached at boot: wakes re-join without flash
  char psk_[65] = "";

  NetState state_ = NetState::Idle;
  bool synced_ = false;
  bool resyncing_ = false;
  uint32_t ntpStartedMs_ = 0;
  uint32_t lastNtpAttemptMs_ = 0;
  uint32_t downSinceMs_ = 0;  // 0 = link up (or not yet seen down)
  uint32_t retryAtMs_ = 0;    // next reconnect attempt (Failed state)
  uint32_t attemptMs_ = 0;    // start of the current attempt, 0 = none
  uint8_t ntpRound_ = 0;
  char ip_[16] = "";
  char tz_[48] = "CET-1CEST,M3.5.0,M10.5.0/3";
};
