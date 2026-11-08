#pragma once

#include <Adafruit_GFX.h>
#include <time.h>

#include "hal/Face.h"
#include "hal/Widgets.h"

/*
 * On-device interaction: face paging, menu, alarms, timer.
 * Spec: docs/device-ux.md. Hardware-free (no GPIO / Wi-Fi / EEPROM), so the
 * desktop preview runs this exact code; main.cpp feeds it pad events and acts
 * on the actions it requests.
 */

enum class Pad : uint8_t { Left, Middle, Right };
enum class Gesture : uint8_t { Tap, Hold };

static constexpr uint8_t kMaxFaces = 4;
static constexpr uint8_t kAlarmCount = 3;  // as the original
static constexpr uint8_t kChimeCount = 5;
static constexpr uint8_t kRotateChoices[] = {1, 2, 5, 10, 15, 30, 60};

enum class AlarmDays : uint8_t { Every = 0, Weekdays = 1, Weekends = 2 };

struct AlarmConfig {
  bool on;
  uint8_t hour;
  uint8_t minute;
  uint8_t days;   // AlarmDays
  uint8_t chime;  // 0..kChimeCount-1
};

/** Everything the UI can change; persisted by the owner (Settings later). */
struct UiConfig {
  FaceLayout faces[kMaxFaces];  // all-None face = unused (pages set up in the web UI)
  uint8_t currentFace;
  bool rotate;            // auto-advance through used faces
  uint8_t rotateMinutes;  // one of kRotateChoices
  AlarmConfig alarms[kAlarmCount];
  uint8_t volume;  // 0..10
  bool touchBeep;
  bool syncBeep;
  bool nightOn;
  uint8_t nightFromHour;
  uint8_t nightToHour;
  bool webUi;
  bool holdBack;  // hold ● = back in menus (the ↰ header item always works)
  bool wifiAlways;  // false = power saver (radio off between syncs)
};

void uiDefaultConfig(UiConfig &cfg);

/** Live device facts shown in the Wi‑Fi / Info screens. */
struct UiStatus {
  char ssid[33];
  char ip[16];
  int16_t rssi;
  bool wifiOk;
  bool radioOff;    // power saver has Wi-Fi asleep (normal, not a fault)
  bool connecting;  // joining right now
  int8_t batteryPct;  // -1 = unknown
  uint32_t uptimeS;
};

/** Requests for main.cpp (bitmask from Ui::takeActions). */
enum UiAction : uint16_t {
  kUiSaveConfig = 1 << 0,   // config changed → persist
  kUiOpenPortal = 1 << 1,   // start Wi‑Fi setup hotspot
  kUiForgetWifi = 1 << 2,   // erase Wi‑Fi + reboot
  kUiWebUiChanged = 1 << 3, // start/stop the web server per cfg.webUi
  kUiSyncNow = 1 << 4,      // re-sync time (NTP) and weather now
  kUiWifiMode = 1 << 5,     // cfg.wifiAlways changed
  kUiWakeWifi = 1 << 6,     // power saver: radio on for a while (web UI)
};

enum class UiRefresh : uint8_t { None, Partial, Full };

/** Sound request: a chime index, a short beep, or nothing. */
static constexpr int8_t kUiSoundNone = -1;
static constexpr int8_t kUiSoundBeep = 100;

const char *chimeName(uint8_t i, char *buf, size_t len);  // "CHURCH BELL"
const char *chimeFile(uint8_t i, char *buf, size_t len);  // "churchbell"

class Ui {
 public:
  explicit Ui(UiConfig &cfg) : cfg_(cfg) {}

  void setStatus(const UiStatus &s) { status_ = s; }

  /** A touch gesture. `now` = local time or nullptr if not synced. */
  void onPad(Pad pad, Gesture g, uint32_t nowMs, const struct tm *now);
  /** Call often: alarms, timer, snooze, menu timeout. */
  void tick(uint32_t nowMs, const struct tm *now);

  /** Draw whatever the screen should show now (face, menu, ringing card). */
  void draw(Adafruit_GFX &gfx, int16_t w, int16_t h, const FaceInput &face,
            uint32_t nowMs);

  uint16_t takeActions();
  UiRefresh takeRefresh();
  int8_t takeSound();

  /**
   * Bottom-line text for an active timer/snooze, or nullptr. Deliberately
   * coarse (whole minutes, then 5 s steps in the last minute) so the e-paper
   * only refreshes when the text changes.
   */
  const char *overlayStatus(uint32_t nowMs);
  /** Inside night-mode quiet hours (no sounds except alarms). */
  bool quiet(const struct tm *now) const { return quietHours(now); }

  /**
   * Full-screen card instead of the face (menu/alarms still take priority).
   * WifiFailed: tap ● = "TRY AGAIN" (kUiSyncNow).
   */
  enum class Card : uint8_t { None, WifiFailed };
  void setCard(Card c);

  bool inMenu() const { return depth_ > 0; }
  bool ringing() const { return ring_ != Ring::None; }
  bool timerRunning() const { return timerEndMs_ != 0; }
  const FaceLayout &currentLayout() const;

 private:
  enum class Screen : uint8_t {
    Root, Faces, Alarms, AlarmEdit, Timer, Settings, Sound, Night, Wifi, Info,
    WebUi,  // Wi-Fi › CONNECT NOW: status, then address + QR
  };
  enum class Ring : uint8_t { None, Alarm, Timer };
  enum class EditTarget : uint8_t { None, AlarmTime, TimerMinutes, Volume, NightFrom, NightTo };
  enum class ConfirmKind : uint8_t { None, Forget, Portal };

  struct Frame {
    Screen screen;
    uint8_t param;   // e.g. alarm index
    uint8_t cursor;  // 0 = the ↰ back item in the header, n = row n-1
  };

  struct Row {
    const char *label;  // PROGMEM, may be nullptr (value-only row)
    char value[24];
    bool selectable;
    uint8_t icon;  // MenuIcon (Ui.cpp), 0 = none
  };
  static constexpr uint8_t kMaxRows = 10;

  uint8_t buildRows(const Frame &f, Row *rows) const;
  void selectRow(Frame &f, uint8_t row, uint32_t nowMs);
  void moveCursor(Frame &f, int dir);
  void push(Screen s, uint8_t param = 0);
  void pop();
  void closeMenu();
  void startEdit(EditTarget t, uint8_t param);
  void applyEdit();
  void startRing(Ring r, uint32_t nowMs);
  void stopRing();
  bool quietHours(const struct tm *now) const;
  /** persist=false for automatic rotation (don't wear EEPROM). */
  void pageFace(int dir, bool persist = true);
  uint8_t usedFaceCount() const;
  bool faceUsed(uint8_t i) const;
  void setRefresh(UiRefresh r);

  void drawMenu(Adafruit_GFX &gfx, int16_t w, int16_t h);
  void drawEdit(Adafruit_GFX &gfx, int16_t w, int16_t h);
  void drawConfirm(Adafruit_GFX &gfx, int16_t w, int16_t h);
  void drawInfo(Adafruit_GFX &gfx, int16_t w, int16_t h);
  void drawWebUi(Adafruit_GFX &gfx, int16_t w, int16_t h);
  void drawQr(Adafruit_GFX &g, int16_t w, int16_t top, int16_t bottom);
  void drawRing(Adafruit_GFX &gfx, int16_t w, int16_t h, const FaceInput &face);
  void drawFaceDots(Adafruit_GFX &gfx, int16_t w, int16_t h);

  UiConfig &cfg_;
  UiStatus status_{};
  Face face_;

  Frame stack_[4]{};
  uint8_t depth_ = 0;  // 0 = showing the face

  EditTarget edit_ = EditTarget::None;
  uint8_t editParam_ = 0;
  uint8_t editField_ = 0;
  int16_t editVal_[2] = {0, 0};

  ConfirmKind confirm_ = ConfirmKind::None;
  uint8_t confirmYes_ = 0;  // 0 = NO selected (safe default)

  Ring ring_ = Ring::None;
  uint8_t ringAlarm_ = 0;
  uint32_t ringStartMs_ = 0;
  uint32_t snoozeUntilMs_ = 0;
  uint32_t firedKey_[kAlarmCount] = {};  // minute stamp already fired

  uint32_t timerEndMs_ = 0;
  uint16_t timerMinutes_ = 10;  // last custom value

  uint32_t lastInputMs_ = 0;
  uint32_t lastRotateMs_ = 0;
  uint32_t nowMs_ = 0;
  uint32_t lastRingSoundMs_ = 0;
  char statusBuf_[24] = "";
  Card card_ = Card::None;
  uint16_t actions_ = 0;
  UiRefresh refresh_ = UiRefresh::None;
  int8_t sound_ = kUiSoundNone;
};
