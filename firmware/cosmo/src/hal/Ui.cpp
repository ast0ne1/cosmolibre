#include "hal/Ui.h"

#include <Arduino.h>
#include <qrcode.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal/Typo.h"
#include "hal/Version.h"

using namespace typo;

namespace {

constexpr uint32_t kMenuTimeoutMs = 30000;
constexpr uint32_t kSnoozeMs = 9UL * 60UL * 1000UL;
constexpr uint32_t kRingMaxMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kRingRepeatMs = 10000;
constexpr uint8_t kTimerChime = 0;  // BOWL
constexpr uint16_t kTimerPresets[] = {5, 10, 15, 30};

const char kChimeNames[kChimeCount][12] PROGMEM = {
    "BOWL", "CHURCH BELL", "CUCKOO", "FUTURISTIC", "ROOSTER"};
const char kChimeFiles[kChimeCount][11] PROGMEM = {
    "bowl", "churchbell", "cuckoo", "futuristic", "rooster"};
const char kDayModes[3][10] PROGMEM = {"EVERY DAY", "WEEKDAYS", "WEEKENDS"};

enum MenuIcon : uint8_t {
  kIconNone = 0,
  kIconApps,
  kIconBell,
  kIconBellOff,
  kIconHourglass,
  kIconGear,
  kIconSync,
  kIconSpeaker,
  kIconMoon,
  kIconWifi,
  kIconInfo,
  kIconScreen,
  kIconBack,
};

const char *P(char *buf, size_t len, const char *pgm) {
  strncpy_P(buf, pgm, len - 1);
  buf[len - 1] = 0;
  return buf;
}

const char *onOff(bool on) { return on ? "ON" : "OFF"; }

void fmtClock(char *buf, size_t len, uint32_t ms) {
  uint32_t s = (ms + 999) / 1000;
  if (s >= 3600) {
    snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  } else {
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60),
             (unsigned long)(s % 60));
  }
}

const char *widgetShort(uint8_t kind) {
  switch ((WidgetKind)kind) {
    case WidgetKind::Time: return "TIME";
    case WidgetKind::Date: return "DATE";
    case WidgetKind::Weather: return "WX";
    case WidgetKind::Greeting: return "HELLO";
    default: return "";
  }
}

}  // namespace

const char *chimeName(uint8_t i, char *buf, size_t len) {
  return P(buf, len, kChimeNames[i < kChimeCount ? i : 0]);
}

const char *chimeFile(uint8_t i, char *buf, size_t len) {
  return P(buf, len, kChimeFiles[i < kChimeCount ? i : 0]);
}

void uiDefaultConfig(UiConfig &c) {
  memset(&c, 0, sizeof(c));
  c.currentFace = 0;
  c.rotate = false;
  c.rotateMinutes = 5;
  // Three alarms, off, at the original's 6:30 / 7:30 / 8:30.
  for (uint8_t i = 0; i < kAlarmCount; i++) {
    c.alarms[i] = {false, (uint8_t)(6 + i), 30, (uint8_t)AlarmDays::Every, (uint8_t)(i % kChimeCount)};
  }
  c.volume = 6;
  c.touchBeep = false;
  c.syncBeep = true;
  c.nightOn = false;
  c.nightFromHour = 22;
  c.nightToHour = 7;
  c.webUi = true;
  c.holdBack = true;
  c.wifiAlways = false;
}

// ---- state helpers --------------------------------------------------------

bool Ui::faceUsed(uint8_t i) const {
  if (i >= kMaxFaces) return false;
  for (uint8_t s = 0; s < kFaceSlots; s++) {
    if (cfg_.faces[i].slot[s].kind) return true;
  }
  return false;
}

const FaceLayout &Ui::currentLayout() const {
  return cfg_.faces[cfg_.currentFace < kMaxFaces ? cfg_.currentFace : 0];
}

void Ui::setRefresh(UiRefresh r) {
  if ((uint8_t)r > (uint8_t)refresh_) refresh_ = r;
}

void Ui::setCard(Card c) {
  if (c != card_) {
    card_ = c;
    if (depth_ == 0 && ring_ == Ring::None) setRefresh(UiRefresh::Full);
  }
}

uint16_t Ui::takeActions() {
  uint16_t a = actions_;
  actions_ = 0;
  return a;
}

UiRefresh Ui::takeRefresh() {
  UiRefresh r = refresh_;
  refresh_ = UiRefresh::None;
  return r;
}

int8_t Ui::takeSound() {
  int8_t s = sound_;
  sound_ = kUiSoundNone;
  return s;
}

bool Ui::quietHours(const struct tm *now) const {
  if (!cfg_.nightOn || !now) return false;
  const int h = now->tm_hour, a = cfg_.nightFromHour, b = cfg_.nightToHour;
  return a <= b ? (h >= a && h < b) : (h >= a || h < b);
}

uint8_t Ui::usedFaceCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kMaxFaces; i++) n += faceUsed(i);
  return n;
}

void Ui::pageFace(int dir, bool persist) {
  uint8_t i = cfg_.currentFace;
  for (uint8_t step = 0; step < kMaxFaces; step++) {
    i = (uint8_t)((i + kMaxFaces + dir) % kMaxFaces);
    if (faceUsed(i)) break;
  }
  if (i != cfg_.currentFace && faceUsed(i)) {
    cfg_.currentFace = i;
    if (persist) actions_ |= kUiSaveConfig;
    setRefresh(UiRefresh::Full);
  }
}

void Ui::push(Screen s, uint8_t param) {
  if (depth_ >= sizeof(stack_) / sizeof(stack_[0])) return;
  Frame f{s, param, 0};
  Row rows[kMaxRows];
  const uint8_t n = buildRows(f, rows);
  uint8_t r = 0;
  while (r < n && !rows[r].selectable) r++;
  f.cursor = r < n ? (uint8_t)(r + 1) : 0;  // first item, else the ↰ back item
  stack_[depth_++] = f;
  setRefresh(depth_ == 1 ? UiRefresh::Full : UiRefresh::Partial);
}

void Ui::pop() {
  if (depth_ == 0) return;
  if (--depth_ == 0) {
    closeMenu();
  } else {
    setRefresh(UiRefresh::Partial);
  }
}

void Ui::closeMenu() {
  depth_ = 0;
  edit_ = EditTarget::None;
  confirm_ = ConfirmKind::None;
  setRefresh(UiRefresh::Full);
}

void Ui::moveCursor(Frame &f, int dir) {
  Row rows[kMaxRows];
  const uint8_t n = buildRows(f, rows);
  const uint8_t positions = n + 1;  // 0 = ↰ back
  uint8_t c = f.cursor;
  for (uint8_t step = 0; step < positions; step++) {
    c = (uint8_t)((c + positions + dir) % positions);
    if (c == 0 || rows[c - 1].selectable) break;
  }
  f.cursor = c;
  setRefresh(UiRefresh::Partial);
}

// ---- alarms / timer -------------------------------------------------------

void Ui::startRing(Ring r, uint32_t nowMs) {
  depth_ = 0;
  edit_ = EditTarget::None;
  confirm_ = ConfirmKind::None;
  ring_ = r;
  ringStartMs_ = nowMs;
  lastRingSoundMs_ = nowMs;
  sound_ = r == Ring::Timer ? kTimerChime : cfg_.alarms[ringAlarm_].chime;
  setRefresh(UiRefresh::Full);
}

void Ui::stopRing() {
  ring_ = Ring::None;
  setRefresh(UiRefresh::Full);
}

void Ui::tick(uint32_t nowMs, const struct tm *now) {
  nowMs_ = nowMs;

  if (depth_ > 0 && nowMs - lastInputMs_ > kMenuTimeoutMs) {
    closeMenu();
  }

  if (ring_ != Ring::None) {
    if (nowMs - ringStartMs_ > kRingMaxMs) {
      stopRing();
    } else if (nowMs - lastRingSoundMs_ >= kRingRepeatMs) {
      lastRingSoundMs_ = nowMs;
      sound_ = ring_ == Ring::Timer ? kTimerChime : cfg_.alarms[ringAlarm_].chime;
    }
    return;
  }

  if (timerEndMs_ && (int32_t)(nowMs - timerEndMs_) >= 0) {
    timerEndMs_ = 0;
    startRing(Ring::Timer, nowMs);
    return;
  }

  if (snoozeUntilMs_ && (int32_t)(nowMs - snoozeUntilMs_) >= 0) {
    snoozeUntilMs_ = 0;
    startRing(Ring::Alarm, nowMs);
    return;
  }

  // Auto-rotate pages while the face is showing (paused in night mode).
  if (cfg_.rotate && depth_ == 0 && usedFaceCount() > 1 && !quietHours(now)) {
    if (lastRotateMs_ == 0) lastRotateMs_ = nowMs;
    if (nowMs - lastRotateMs_ >= cfg_.rotateMinutes * 60000UL) {
      lastRotateMs_ = nowMs;
      pageFace(1, false);
    }
  }

  if (!now) return;
  const uint32_t key =
      ((uint32_t)(now->tm_year * 366 + now->tm_yday) * 1440U) +
      (uint32_t)(now->tm_hour * 60 + now->tm_min);
  for (uint8_t i = 0; i < kAlarmCount; i++) {
    const AlarmConfig &a = cfg_.alarms[i];
    if (!a.on || a.hour != now->tm_hour || a.minute != now->tm_min) continue;
    const bool weekend = now->tm_wday == 0 || now->tm_wday == 6;
    if (a.days == (uint8_t)AlarmDays::Weekdays && weekend) continue;
    if (a.days == (uint8_t)AlarmDays::Weekends && !weekend) continue;
    if (firedKey_[i] == key) continue;
    firedKey_[i] = key;
    ringAlarm_ = i;
    snoozeUntilMs_ = 0;
    startRing(Ring::Alarm, nowMs);
    return;
  }
}

// ---- input ----------------------------------------------------------------

void Ui::onPad(Pad pad, Gesture g, uint32_t nowMs, const struct tm *now) {
  lastInputMs_ = nowMs;
  nowMs_ = nowMs;

  if (ring_ != Ring::None) {
    if (g == Gesture::Hold && pad == Pad::Middle && ring_ == Ring::Alarm) {
      snoozeUntilMs_ = nowMs + kSnoozeMs;
      if (!snoozeUntilMs_) snoozeUntilMs_ = 1;
    }
    stopRing();
    return;
  }

  if (cfg_.touchBeep && !quietHours(now)) {
    sound_ = kUiSoundBeep;
  }

  if (confirm_ != ConfirmKind::None) {
    if (g == Gesture::Hold && pad == Pad::Middle) {
      confirm_ = ConfirmKind::None;
    } else if (pad != Pad::Middle) {
      confirmYes_ ^= 1;
    } else {
      const ConfirmKind k = confirm_;
      confirm_ = ConfirmKind::None;
      if (confirmYes_) {
        actions_ |= k == ConfirmKind::Forget ? kUiForgetWifi : kUiOpenPortal;
        closeMenu();
        return;
      }
    }
    setRefresh(UiRefresh::Partial);
    return;
  }

  if (edit_ != EditTarget::None) {
    if (g == Gesture::Hold && pad == Pad::Middle) {
      edit_ = EditTarget::None;  // cancel
    } else if (pad == Pad::Middle) {
      const uint8_t fields = edit_ == EditTarget::AlarmTime ? 2 : 1;
      if (editField_ + 1 < fields) {
        editField_++;
      } else {
        applyEdit();
        return;
      }
    } else {
      int16_t lo = 0, hi = 59;
      switch (edit_) {
        case EditTarget::AlarmTime: hi = editField_ == 0 ? 23 : 59; break;
        case EditTarget::TimerMinutes: lo = 1; hi = 180; break;
        case EditTarget::Volume: hi = 10; break;
        default: hi = 23; break;  // night hours
      }
      const int16_t span = hi - lo + 1;
      const int16_t step = (g == Gesture::Hold ? 10 : 1) * (pad == Pad::Right ? 1 : -1);
      int16_t &v = editVal_[editField_];
      v = (int16_t)(lo + ((v - lo + step) % span + span) % span);
    }
    setRefresh(UiRefresh::Partial);
    return;
  }

  if (depth_ > 0) {
    Frame &f = stack_[depth_ - 1];
    if (g == Gesture::Hold && pad == Pad::Middle) {
      if (cfg_.holdBack) pop();  // optional shortcut (Settings › HOLD TO BACK)
    } else if (f.screen == Screen::Info || f.screen == Screen::WebUi) {
      if (pad == Pad::Middle) pop();
    } else if (pad == Pad::Middle) {
      if (f.cursor == 0) {
        pop();  // ↰ in the header
      } else {
        selectRow(f, f.cursor - 1, nowMs);
      }
    } else {
      moveCursor(f, pad == Pad::Right ? 1 : -1);
    }
    return;
  }

  // On the face (or the Wi‑Fi failed card).
  if (card_ == Card::WifiFailed && g == Gesture::Tap && pad == Pad::Middle) {
    actions_ |= kUiSyncNow;  // TRY AGAIN
    return;
  }
  if (g == Gesture::Hold && pad == Pad::Middle) {
    push(Screen::Root);
  } else if (g == Gesture::Tap && pad == Pad::Left) {
    pageFace(-1);
    lastRotateMs_ = nowMs;
  } else if (g == Gesture::Tap && pad == Pad::Right) {
    pageFace(1);
    lastRotateMs_ = nowMs;
  }
}

void Ui::startEdit(EditTarget t, uint8_t param) {
  edit_ = t;
  editParam_ = param;
  editField_ = 0;
  switch (t) {
    case EditTarget::AlarmTime:
      editVal_[0] = cfg_.alarms[param].hour;
      editVal_[1] = cfg_.alarms[param].minute;
      break;
    case EditTarget::TimerMinutes: editVal_[0] = (int16_t)timerMinutes_; break;
    case EditTarget::Volume: editVal_[0] = cfg_.volume; break;
    case EditTarget::NightFrom: editVal_[0] = cfg_.nightFromHour; break;
    case EditTarget::NightTo: editVal_[0] = cfg_.nightToHour; break;
    default: break;
  }
  setRefresh(UiRefresh::Partial);
}

void Ui::applyEdit() {
  const EditTarget t = edit_;
  edit_ = EditTarget::None;
  switch (t) {
    case EditTarget::AlarmTime:
      cfg_.alarms[editParam_].hour = (uint8_t)editVal_[0];
      cfg_.alarms[editParam_].minute = (uint8_t)editVal_[1];
      cfg_.alarms[editParam_].on = true;  // setting a time arms it
      break;
    case EditTarget::TimerMinutes:
      timerMinutes_ = (uint16_t)editVal_[0];
      timerEndMs_ = nowMs_ + timerMinutes_ * 60000UL;
      if (!timerEndMs_) timerEndMs_ = 1;
      closeMenu();
      return;
    case EditTarget::Volume:
      cfg_.volume = (uint8_t)editVal_[0];
      sound_ = kUiSoundBeep;  // hear the new level
      break;
    case EditTarget::NightFrom: cfg_.nightFromHour = (uint8_t)editVal_[0]; break;
    case EditTarget::NightTo: cfg_.nightToHour = (uint8_t)editVal_[0]; break;
    default: break;
  }
  actions_ |= kUiSaveConfig;
  setRefresh(UiRefresh::Partial);
}

// ---- menu content -----------------------------------------------------------

uint8_t Ui::buildRows(const Frame &f, Row *rows) const {
  uint8_t n = 0;
  auto add = [&](const char *label, bool selectable, uint8_t icon = 0) -> Row & {
    Row &r = rows[n++];
    r.label = label;
    r.value[0] = 0;
    r.selectable = selectable;
    r.icon = icon;
    return r;
  };

  switch (f.screen) {
    case Screen::Root:
      // Mirrors the original Cosmo II menu.
      add(PSTR("APPS"), true, kIconApps);
      add(PSTR("ALARM"), true, kIconBell);
      add(PSTR("TIMER"), true, kIconHourglass);
      add(PSTR("SETTINGS"), true, kIconGear);
      add(PSTR("SYNC CLOCK"), true, kIconSync);
      break;
    case Screen::Settings:
      snprintf(add(PSTR("SOUND"), true, kIconSpeaker).value, sizeof(Row::value),
               "%u", cfg_.volume);
      snprintf(add(PSTR("NIGHT MODE"), true, kIconMoon).value, sizeof(Row::value),
               "%s", onOff(cfg_.nightOn));
      snprintf(add(PSTR("WI-FI"), true, kIconWifi).value, sizeof(Row::value), "%s",
               status_.wifiOk ? "OK" : "OFF");
      snprintf(add(PSTR("HOLD TO BACK"), true, kIconBack).value, sizeof(Row::value),
               "%s", onOff(cfg_.holdBack));
      add(PSTR("INFO"), true, kIconInfo);
      add(PSTR("CLEAN SCREEN"), true, kIconScreen);
      break;
    case Screen::Faces:
      snprintf(add(PSTR("ROTATE"), true).value, sizeof(Row::value), "%s",
               onOff(cfg_.rotate));
      snprintf(add(PSTR("EVERY"), true).value, sizeof(Row::value), "%u MIN",
               cfg_.rotateMinutes);
      for (uint8_t i = 0; i < kMaxFaces; i++) {
        if (!faceUsed(i)) continue;
        Row &r = add(nullptr, true);
        int len = snprintf(r.value, sizeof(r.value), "%u ", i + 1);
        bool first = true;
        for (uint8_t s = 0; s < kFaceSlots; s++) {
          const char *w = widgetShort(cfg_.faces[i].slot[s].kind);
          if (!w[0]) continue;
          len += snprintf(r.value + len, sizeof(r.value) - len, "%s%s",
                          first ? "" : "+", w);
          first = false;
          if (len >= (int)sizeof(r.value) - 1) break;
        }
        if (i == cfg_.currentFace && len < (int)sizeof(r.value) - 3) {
          strcat(r.value, " *");
        }
      }
      break;
    case Screen::Alarms:
      for (uint8_t i = 0; i < kAlarmCount; i++) {
        Row &r = add(PSTR("ALARM"), true, cfg_.alarms[i].on ? kIconBell : kIconBellOff);
        snprintf(r.value, sizeof(r.value), "%u:%02u", cfg_.alarms[i].hour,
                 cfg_.alarms[i].minute);
        continue;
        if (cfg_.alarms[i].on) {
          snprintf(r.value, sizeof(r.value), "%02u:%02u", cfg_.alarms[i].hour,
                   cfg_.alarms[i].minute);
        } else {
          snprintf(r.value, sizeof(r.value), "OFF");
        }
      }
      break;
    case Screen::AlarmEdit: {
      const AlarmConfig &a = cfg_.alarms[f.param];
      snprintf(add(PSTR("ALARM"), true).value, sizeof(Row::value), "%s", onOff(a.on));
      snprintf(add(PSTR("TIME"), true).value, sizeof(Row::value), "%02u:%02u",
               a.hour, a.minute);
      P(add(PSTR("DAYS"), true).value, sizeof(Row::value), kDayModes[a.days % 3]);
      chimeName(a.chime, add(PSTR("SOUND"), true).value, sizeof(Row::value));
      break;
    }
    case Screen::Timer:
      if (timerEndMs_) {
        fmtClock(add(PSTR("STOP"), true).value, sizeof(Row::value),
                 timerEndMs_ - nowMs_);
        add(PSTR("ADD 5 MIN"), true);
      } else {
        add(PSTR("5 MIN"), true);
        add(PSTR("10 MIN"), true);
        add(PSTR("15 MIN"), true);
        add(PSTR("30 MIN"), true);
        snprintf(add(PSTR("CUSTOM"), true).value, sizeof(Row::value), "%u MIN",
                 timerMinutes_);
      }
      break;
    case Screen::Sound:
      snprintf(add(PSTR("VOLUME"), true).value, sizeof(Row::value), "%u",
               cfg_.volume);
      snprintf(add(PSTR("TOUCH BEEP"), true).value, sizeof(Row::value), "%s",
               onOff(cfg_.touchBeep));
      snprintf(add(PSTR("SYNC BEEP"), true).value, sizeof(Row::value), "%s",
               onOff(cfg_.syncBeep));
      add(PSTR("TEST SOUND"), true);
      break;
    case Screen::Night:
      snprintf(add(PSTR("NIGHT MODE"), true).value, sizeof(Row::value), "%s",
               onOff(cfg_.nightOn));
      snprintf(add(PSTR("FROM"), true).value, sizeof(Row::value), "%02u:00",
               cfg_.nightFromHour);
      snprintf(add(PSTR("TO"), true).value, sizeof(Row::value), "%02u:00",
               cfg_.nightToHour);
      break;
    case Screen::Wifi:
      snprintf(add(nullptr, false).value, sizeof(Row::value), "%.20s",
               status_.wifiOk && status_.ssid[0] ? status_.ssid
               : status_.radioOff                ? "ASLEEP (SAVER)"
                                                 : "NOT CONNECTED");
      snprintf(add(nullptr, false).value, sizeof(Row::value), "%s",
               status_.ip[0] ? status_.ip : "NO IP");
      if (status_.wifiOk) {
        snprintf(add(nullptr, false).value, sizeof(Row::value), "SIGNAL %d DBM",
                 status_.rssi);
      } else {
        snprintf(add(nullptr, false).value, sizeof(Row::value), "SIGNAL --");
      }
      snprintf(add(PSTR("MODE"), true).value, sizeof(Row::value), "%s",
               cfg_.wifiAlways ? "ALWAYS" : "SAVER");
      snprintf(add(PSTR("CONNECT"), true).value, sizeof(Row::value), "15 MIN");
      add(PSTR("SETUP HOTSPOT"), true);
      snprintf(add(PSTR("WEB UI"), true).value, sizeof(Row::value), "%s",
               onOff(cfg_.webUi));
      add(PSTR("FORGET WI-FI"), true);
      break;
    case Screen::Info:
    case Screen::WebUi:
      break;
  }
  return n;
}

void Ui::selectRow(Frame &f, uint8_t row, uint32_t nowMs) {
  switch (f.screen) {
    case Screen::Root:
      switch (row) {
        case 0: push(Screen::Faces); break;
        case 1: push(Screen::Alarms); break;
        case 2: push(Screen::Timer); break;
        case 3: push(Screen::Settings); break;
        case 4:
          actions_ |= kUiSyncNow;
          sound_ = kUiSoundBeep;  // acknowledge
          closeMenu();
          break;
      }
      return;
    case Screen::Settings:
      switch (row) {
        case 0: push(Screen::Sound); break;
        case 1: push(Screen::Night); break;
        case 2: push(Screen::Wifi); break;
        case 3:
          cfg_.holdBack = !cfg_.holdBack;
          actions_ |= kUiSaveConfig;
          setRefresh(UiRefresh::Partial);
          break;
        case 4: push(Screen::Info); break;
        case 5: closeMenu(); break;  // full refresh clears ghosting
      }
      return;
    case Screen::Faces: {
      if (row == 0) {
        cfg_.rotate = !cfg_.rotate;
        lastRotateMs_ = nowMs;
        actions_ |= kUiSaveConfig;
        setRefresh(UiRefresh::Partial);
        return;
      }
      if (row == 1) {
        uint8_t next = kRotateChoices[0];
        for (uint8_t c : kRotateChoices) {
          if (c > cfg_.rotateMinutes) {
            next = c;
            break;
          }
        }
        cfg_.rotateMinutes = next;  // cycles 1 → 2 → 5 … 60 → 1
        lastRotateMs_ = nowMs;
        actions_ |= kUiSaveConfig;
        setRefresh(UiRefresh::Partial);
        return;
      }
      row -= 2;
      uint8_t seen = 0;
      for (uint8_t i = 0; i < kMaxFaces; i++) {
        if (!faceUsed(i)) continue;
        if (seen++ == row) {
          cfg_.currentFace = i;
          lastRotateMs_ = nowMs;
          actions_ |= kUiSaveConfig;
          closeMenu();
          return;
        }
      }
      return;
    }
    case Screen::Alarms:
      push(Screen::AlarmEdit, row);
      return;
    case Screen::AlarmEdit: {
      AlarmConfig &a = cfg_.alarms[f.param];
      switch (row) {
        case 0: a.on = !a.on; break;
        case 1: startEdit(EditTarget::AlarmTime, f.param); return;
        case 2: a.days = (uint8_t)((a.days + 1) % 3); break;
        case 3:
          a.chime = (uint8_t)((a.chime + 1) % kChimeCount);
          sound_ = (int8_t)a.chime;  // preview the new chime
          break;
      }
      actions_ |= kUiSaveConfig;
      setRefresh(UiRefresh::Partial);
      return;
    }
    case Screen::Timer:
      if (timerEndMs_) {
        if (row == 0) {
          timerEndMs_ = 0;
          closeMenu();
        } else {
          timerEndMs_ += 5UL * 60000UL;
          setRefresh(UiRefresh::Partial);
        }
      } else if (row < 4) {
        timerEndMs_ = nowMs + kTimerPresets[row] * 60000UL;
        if (!timerEndMs_) timerEndMs_ = 1;
        closeMenu();
      } else {
        startEdit(EditTarget::TimerMinutes, 0);
      }
      return;
    case Screen::Sound:
      switch (row) {
        case 0: startEdit(EditTarget::Volume, 0); return;
        case 1: cfg_.touchBeep = !cfg_.touchBeep; break;
        case 2: cfg_.syncBeep = !cfg_.syncBeep; break;
        case 3: sound_ = kUiSoundBeep; return;
      }
      actions_ |= kUiSaveConfig;
      setRefresh(UiRefresh::Partial);
      return;
    case Screen::Night:
      switch (row) {
        case 0:
          cfg_.nightOn = !cfg_.nightOn;
          actions_ |= kUiSaveConfig;
          setRefresh(UiRefresh::Partial);
          return;
        case 1: startEdit(EditTarget::NightFrom, 0); return;
        case 2: startEdit(EditTarget::NightTo, 0); return;
      }
      return;
    case Screen::Wifi:
      switch (row) {
        case 3:
          cfg_.wifiAlways = !cfg_.wifiAlways;
          actions_ |= kUiSaveConfig | kUiWifiMode;
          setRefresh(UiRefresh::Partial);
          return;
        case 4:
          // Radio on for 15 min, then the web UI's address + QR (Info).
          actions_ |= kUiWakeWifi;
          push(Screen::WebUi);
          return;
        case 5:
          confirm_ = ConfirmKind::Portal;
          confirmYes_ = 0;
          setRefresh(UiRefresh::Partial);
          return;
        case 6:
          // Reversible from here, so no confirm (see docs/device-ux.md).
          cfg_.webUi = !cfg_.webUi;
          actions_ |= kUiSaveConfig | kUiWebUiChanged;
          setRefresh(UiRefresh::Partial);
          return;
        case 7:
          confirm_ = ConfirmKind::Forget;
          confirmYes_ = 0;
          setRefresh(UiRefresh::Partial);
          return;
      }
      return;
    case Screen::Info:
    case Screen::WebUi:
      return;
  }
}

// ---- drawing ----------------------------------------------------------------

namespace {

constexpr int16_t kMargin = 16;
constexpr int16_t kRowsTop = 92;   // menu list (below the header)
constexpr int16_t kRowH = 50;
constexpr int16_t kLetterGap = 3;  // letter-spacing of menu labels (as the original)

int16_t R(float v) { return (int16_t)lroundf(v); }

void thick(Adafruit_GFX &g, float x0, float y0, float x1, float y1, float t = 2.2f) {
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len > 0.5f) {
    const float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    g.fillTriangle(R(x0 + nx), R(y0 + ny), R(x1 + nx), R(y1 + ny), R(x1 - nx), R(y1 - ny), GxEPD_BLACK);
    g.fillTriangle(R(x0 + nx), R(y0 + ny), R(x1 - nx), R(y1 - ny), R(x0 - nx), R(y0 - ny), GxEPD_BLACK);
  }
  g.fillCircle(R(x0), R(y0), R(t / 2 - 0.2f), GxEPD_BLACK);
  g.fillCircle(R(x1), R(y1), R(t / 2 - 0.2f), GxEPD_BLACK);
}

void arc(Adafruit_GFX &g, float cx, float cy, float r, float a0, float a1) {
  const int seg = 10;
  float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
  for (int k = 1; k <= seg; k++) {
    const float a = a0 + (a1 - a0) * k / seg;
    const float x = cx + r * cosf(a), y = cy + r * sinf(a);
    thick(g, px, py, x, y);
    px = x;
    py = y;
  }
}

void ring(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t r) {
  g.drawCircle(cx, cy, r, GxEPD_BLACK);
  g.drawCircle(cx, cy, r - 1, GxEPD_BLACK);
}

/** Line icons in the style of the original menu, ~24 px, centred on (cx, cy). */
void menuIcon(Adafruit_GFX &g, uint8_t icon, int16_t cx, int16_t cy) {
  const float PI_F = 3.14159265f;
  switch (icon) {
    case kIconApps:
      for (int i = 0; i < 4; i++) {
        const int16_t x = cx - 10 + (i % 2) * 11, y = cy - 10 + (i / 2) * 11;
        g.drawRoundRect(x, y, 9, 9, 2, GxEPD_BLACK);
        g.drawRoundRect(x + 1, y + 1, 7, 7, 1, GxEPD_BLACK);
      }
      break;
    case kIconBell:
      arc(g, cx, cy - 3, 7, PI_F, 2 * PI_F);           // dome
      thick(g, cx - 7, cy - 3, cx - 9, cy + 6);        // flanks
      thick(g, cx + 7, cy - 3, cx + 9, cy + 6);
      thick(g, cx - 11, cy + 7, cx + 11, cy + 7);      // rim
      g.fillCircle(cx, cy - 11, 2, GxEPD_BLACK);       // knob
      g.fillCircle(cx, cy + 11, 2, GxEPD_BLACK);       // clapper
      break;
    case kIconBellOff:
      menuIcon(g, kIconBell, cx, cy);
      thick(g, cx - 12, cy - 12, cx + 12, cy + 12, 2.6f);
      break;
    case kIconHourglass:
      thick(g, cx - 8, cy - 11, cx + 8, cy - 11);
      thick(g, cx - 8, cy + 11, cx + 8, cy + 11);
      thick(g, cx - 6, cy - 11, cx + 6, cy + 11);
      thick(g, cx + 6, cy - 11, cx - 6, cy + 11);
      break;
    case kIconGear:
      ring(g, cx, cy, 7);
      ring(g, cx, cy, 3);
      for (int i = 0; i < 8; i++) {
        const float a = i * PI_F / 4;
        thick(g, cx + 7 * cosf(a), cy + 7 * sinf(a), cx + 11 * cosf(a), cy + 11 * sinf(a), 3.2f);
      }
      break;
    case kIconSync:
      arc(g, cx, cy, 9, -0.85f * PI_F, -0.15f * PI_F);  // upper arc →
      arc(g, cx, cy, 9, 0.15f * PI_F, 0.85f * PI_F);    // lower arc ←
      thick(g, cx + 8, cy - 4, cx + 9, cy - 10);        // arrow heads
      thick(g, cx + 8, cy - 4, cx + 2, cy - 5);
      thick(g, cx - 8, cy + 4, cx - 9, cy + 10);
      thick(g, cx - 8, cy + 4, cx - 2, cy + 5);
      break;
    case kIconSpeaker:
      g.fillRect(cx - 11, cy - 4, 5, 8, GxEPD_BLACK);
      g.fillTriangle(cx - 7, cy, cx + 1, cy - 9, cx + 1, cy + 9, GxEPD_BLACK);
      arc(g, cx + 2, cy, 5, -0.3f * PI_F, 0.3f * PI_F);
      arc(g, cx + 2, cy, 10, -0.3f * PI_F, 0.3f * PI_F);
      break;
    case kIconMoon:
      g.fillCircle(cx, cy, 10, GxEPD_BLACK);
      g.fillCircle(cx + 5, cy - 4, 9, GxEPD_WHITE);
      break;
    case kIconWifi:
      arc(g, cx, cy + 9, 6, -0.75f * PI_F, -0.25f * PI_F);
      arc(g, cx, cy + 9, 12, -0.75f * PI_F, -0.25f * PI_F);
      arc(g, cx, cy + 9, 18, -0.72f * PI_F, -0.28f * PI_F);
      g.fillCircle(cx, cy + 9, 2, GxEPD_BLACK);
      break;
    case kIconInfo:
      ring(g, cx, cy, 11);
      g.fillCircle(cx, cy - 5, 2, GxEPD_BLACK);
      thick(g, cx, cy - 1, cx, cy + 6);
      break;
    case kIconBack:
      thick(g, cx - 9, cy + 2, cx + 7, cy + 2);       // ↰ like the header
      thick(g, cx - 9, cy + 2, cx - 4, cy - 3);
      thick(g, cx - 9, cy + 2, cx - 4, cy + 7);
      thick(g, cx + 7, cy + 2, cx + 7, cy - 8);
      break;
    case kIconScreen:
      g.drawRoundRect(cx - 8, cy - 11, 16, 22, 3, GxEPD_BLACK);
      g.drawRoundRect(cx - 7, cy - 10, 14, 20, 2, GxEPD_BLACK);
      thick(g, cx - 3, cy + 5, cx + 4, cy - 4, 1.6f);
      break;
    default:
      break;
  }
}

void backArrow(Adafruit_GFX &g, int16_t y) {
  thick(g, kMargin + 2, y, kMargin + 20, y, 2.0f);      // shaft
  thick(g, kMargin + 2, y, kMargin + 7, y - 5, 2.0f);   // head
  thick(g, kMargin + 2, y, kMargin + 7, y + 5, 2.0f);
  thick(g, kMargin + 20, y, kMargin + 20, y - 8, 2.0f); // tail up (↰)
}

/** Top level: "↰ cosmolibre" — "cosmo" bold, "libre" regular (original: "← cosmoII"). */
void drawWordmark(Adafruit_GFX &g) {
  backArrow(g, 40);
  g.setFont(SerifText);
  g.setTextColor(GxEPD_BLACK);
  g.setCursor(kMargin + 30, 50);
  g.print(F("cosmo"));
  g.setFont(SerifTextLight);  // continue from the bold word's advance
  g.print(F("libre"));
}

/** Submenus: "↰ ALARM" in bold letter-spaced mono capitals, as the original. */
void drawTitle(Adafruit_GFX &g, int16_t w, const char *title) {
  backArrow(g, 40);
  textSpaced(g, MonoM, title, kMargin + 30, 52, w - kMargin - 30 - 8, 3);
}

void drawTitle(Adafruit_GFX &g, int16_t w, const __FlashStringHelper *title) {
  char buf[20];
  strncpy_P(buf, (PGM_P)title, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  drawTitle(g, w, buf);
}

void drawHint(Adafruit_GFX &g, int16_t h, const __FlashStringHelper *hint) {
  textAt(g, MonoXS, hint, kMargin, h - 12);
}

}  // namespace

void Ui::draw(Adafruit_GFX &gfx, int16_t w, int16_t h, const FaceInput &face,
              uint32_t nowMs) {
  nowMs_ = nowMs;
  if (ring_ != Ring::None) {
    drawRing(gfx, w, h, face);
  } else if (confirm_ != ConfirmKind::None) {
    drawConfirm(gfx, w, h);
  } else if (edit_ != EditTarget::None) {
    drawEdit(gfx, w, h);
  } else if (depth_ > 0 && stack_[depth_ - 1].screen == Screen::Info) {
    drawInfo(gfx, w, h);
  } else if (depth_ > 0 && stack_[depth_ - 1].screen == Screen::WebUi) {
    drawWebUi(gfx, w, h);
  } else if (depth_ > 0) {
    drawMenu(gfx, w, h);
  } else if (card_ == Card::WifiFailed) {
    char t[16], hdg[12], l0[20], l1[20], l2[8], ft[12];
    strncpy_P(t, PSTR("WIFI SETUP"), sizeof(t));
    strncpy_P(hdg, PSTR("FAILED"), sizeof(hdg));
    strncpy_P(l0, PSTR("COSMOLIBRE NOT"), sizeof(l0));
    strncpy_P(l1, PSTR("CONNECTED TO"), sizeof(l1));
    strncpy_P(l2, PSTR("WIFI"), sizeof(l2));
    strncpy_P(ft, PSTR("TRY AGAIN"), sizeof(ft));
    const ::Card card{t, CardIcon::Cross, hdg, {l0, l1, l2}, ft, true};
    face_.drawCard(gfx, w, h, card);
  } else {
    FaceInput in = face;
    in.layout = &currentLayout();
    if (const char *ov = overlayStatus(nowMs)) in.status = ov;
    face_.drawFull(gfx, w, h, in);
    drawFaceDots(gfx, w, h);
  }
}

const char *Ui::overlayStatus(uint32_t nowMs) {
  if (timerEndMs_) {
    const uint32_t left = (int32_t)(timerEndMs_ - nowMs) > 0 ? timerEndMs_ - nowMs : 0;
    if (left > 60000) {
      snprintf(statusBuf_, sizeof(statusBuf_), "TIMER %lu MIN",
               (unsigned long)((left + 59999) / 60000));
    } else {
      // Last minute: round up to 5 s steps (60, 55 … 5).
      const unsigned secs = (unsigned)((left + 4999) / 5000) * 5;
      snprintf(statusBuf_, sizeof(statusBuf_), "TIMER %u:%02u", secs / 60, secs % 60);
    }
    return statusBuf_;
  }
  if (snoozeUntilMs_) {
    snprintf(statusBuf_, sizeof(statusBuf_), "SNOOZE %lu MIN",
             (unsigned long)((snoozeUntilMs_ - nowMs + 59999) / 60000));
    return statusBuf_;
  }
  return nullptr;
}

void Ui::drawFaceDots(Adafruit_GFX &g, int16_t w, int16_t h) {
  uint8_t used = 0;
  for (uint8_t i = 0; i < kMaxFaces; i++) used += faceUsed(i);
  if (used < 2) return;
  int16_t x = w - kMargin - 4;
  for (int8_t i = kMaxFaces - 1; i >= 0; i--) {
    if (!faceUsed(i)) continue;
    if (i == cfg_.currentFace) {
      g.fillCircle(x, h - 15, 4, GxEPD_BLACK);
    } else {
      g.drawCircle(x, h - 15, 4, GxEPD_BLACK);
    }
    x -= 14;
  }
}

void Ui::drawMenu(Adafruit_GFX &g, int16_t w, int16_t h) {
  const Frame &f = stack_[depth_ - 1];
  g.fillScreen(GxEPD_WHITE);

  char title[16];
  const char *t = nullptr;
  switch (f.screen) {
    case Screen::Faces: t = PSTR("APPS"); break;
    case Screen::Alarms: t = PSTR("ALARM"); break;
    case Screen::Timer: t = PSTR("TIMER"); break;
    case Screen::Settings: t = PSTR("SETTINGS"); break;
    case Screen::Sound: t = PSTR("SOUND"); break;
    case Screen::Night: t = PSTR("NIGHT MODE"); break;
    case Screen::Wifi: t = PSTR("WI-FI"); break;
    default: break;
  }
  if (f.screen == Screen::Root) {
    drawWordmark(g);
  } else if (f.screen == Screen::AlarmEdit) {
    snprintf(title, sizeof(title), "ALARM %u", f.param + 1);
    drawTitle(g, w, title);
  } else {
    drawTitle(g, w, P(title, sizeof(title), t));
  }

  Row rows[kMaxRows];
  const uint8_t n = buildRows(f, rows);
  const uint8_t visible = (uint8_t)((h - kRowsTop - 12) / kRowH);
  const uint8_t selRow = f.cursor ? (uint8_t)(f.cursor - 1) : 0;
  const uint8_t first = selRow >= visible ? (uint8_t)(selRow - visible + 1) : 0;
  if (f.cursor == 0) g.fillCircle(kMargin - 8, 40, 3, GxEPD_BLACK);  // dot on ↰
  const int16_t capS = capHeight(g, MonoS);

  for (uint8_t i = first; i < n && i < first + visible; i++) {
    const Row &r = rows[i];
    const int16_t cy = kRowsTop + (i - first) * kRowH + kRowH / 2;
    const int16_t base = cy + capS / 2;
    const bool sel = r.selectable && i + 1 == f.cursor;
    if (sel) g.fillCircle(kMargin - 6, cy, 3, GxEPD_BLACK);  // the original's dot
    int16_t x = kMargin + 6;
    if (r.icon) {
      menuIcon(g, r.icon, kMargin + 20, cy);
      x = kMargin + 48;
    }
    int16_t right = w - kMargin;
    if (r.value[0] && r.label) {
      const int16_t vw = spacedWidth(g, MonoS, r.value, 2);
      textSpaced(g, MonoS, r.value, right - vw, base, vw + 1, 2);
      right -= vw + 12;
    }
    if (r.label) {
      char label[24];
      textSpaced(g, MonoS, P(label, sizeof(label), r.label), x, base, right - x, kLetterGap);
    } else {
      textAt(g, MonoS, r.value, x, base);  // info line (Wi‑Fi status)
    }
  }
  if (first > 0) g.fillTriangle(w / 2 - 6, kRowsTop - 2, w / 2 + 6, kRowsTop - 2, w / 2, kRowsTop - 8, GxEPD_BLACK);
  if (first + visible < n) {
    const int16_t y = kRowsTop + visible * kRowH;
    g.fillTriangle(w / 2 - 6, y, w / 2 + 6, y, w / 2, y + 6, GxEPD_BLACK);
  }
}

void Ui::drawEdit(Adafruit_GFX &g, int16_t w, int16_t h) {
  g.fillScreen(GxEPD_WHITE);
  const __FlashStringHelper *title = F(""), *unit = F("");
  switch (edit_) {
    case EditTarget::AlarmTime:
      title = F("ALARM TIME");
      unit = editField_ ? F("MINUTE") : F("HOUR");
      break;
    case EditTarget::TimerMinutes: title = F("TIMER"); unit = F("MINUTES"); break;
    case EditTarget::Volume: title = F("VOLUME"); unit = F("0 - 10"); break;
    case EditTarget::NightFrom: title = F("NIGHT FROM"); unit = F("HOUR"); break;
    case EditTarget::NightTo: title = F("NIGHT TO"); unit = F("HOUR"); break;
    default: break;
  }
  drawTitle(g, w, title);

  const int16_t cy = h / 2 - 20;
  const int16_t base = cy + capHeight(g, SerifL) / 2;
  char a[8], b[8];
  if (edit_ == EditTarget::AlarmTime) {
    snprintf(a, sizeof(a), "%02d", editVal_[0]);
    snprintf(b, sizeof(b), "%02d", editVal_[1]);
    Ink ia = measure(g, SerifL, a), ic = measure(g, SerifL, ":"),
        ib = measure(g, SerifL, b);
    const int16_t gap = 8;
    const int16_t total = ia.w + gap + ic.w + gap + ib.w;
    const int16_t x0 = w / 2 - total / 2;
    const int16_t xb = x0 + ia.w + gap + ic.w + gap;
    textAt(g, SerifL, a, x0 - ia.x1, base);
    textAt(g, SerifL, F(":"), x0 + ia.w + gap - ic.x1, base);
    textAt(g, SerifL, b, xb - ib.x1, base);
    const int16_t ux = editField_ == 0 ? x0 : xb;
    const int16_t uw = editField_ == 0 ? ia.w : ib.w;
    g.fillRect(ux, base + 12, uw, 5, GxEPD_BLACK);
  } else {
    snprintf(a, sizeof(a), "%d", editVal_[0]);
    Ink ia = measure(g, SerifL, a);
    textCentered(g, SerifL, a, w / 2, base);
    g.fillRect(w / 2 - ia.w / 2, base + 12, ia.w, 5, GxEPD_BLACK);
  }
  textCentered(g, MonoS, unit, w / 2, base + 52);
  const bool last = edit_ != EditTarget::AlarmTime || editField_ == 1;
  drawHint(g, h, last ? F("L/R -/+  HOLD 10  TAP OK") : F("L/R -/+  HOLD 10  TAP NEXT"));
}

void Ui::drawConfirm(Adafruit_GFX &g, int16_t w, int16_t h) {
  g.fillScreen(GxEPD_WHITE);
  const bool forget = confirm_ == ConfirmKind::Forget;
  // Titles must fit MonoM across the panel (≈11 characters).
  drawTitle(g, w, forget ? F("FORGET WIFI") : F("WIFI SETUP"));
  const int16_t capS = capHeight(g, MonoS);
  const __FlashStringHelper *forgetLines[] = {F("ERASES THE SAVED"), F("NETWORK, THEN"),
                                              F("RESTARTS. ARE"), F("YOU SURE?")};
  const __FlashStringHelper *portalLines[] = {F("OPENS THE"), F("COSMOLIBRE HOTSPOT"),
                                              F("THE CLOCK LEAVES"), F("YOUR NETWORK UNTIL"),
                                              F("SETUP IS FINISHED.")};
  const __FlashStringHelper *const *lines = forget ? forgetLines : portalLines;
  const uint8_t n = forget ? 4 : 5;
  for (uint8_t i = 0; i < n; i++) {
    textAt(g, MonoS, lines[i], kMargin, 100 + i * (capS + 12));
  }
  const int16_t bw = (w - 3 * kMargin) / 2, bh = 56, by = 300;
  for (uint8_t i = 0; i < 2; i++) {
    const int16_t bx = kMargin + i * (bw + kMargin);
    const bool sel = confirmYes_ == i;
    if (sel) {
      g.fillRoundRect(bx, by, bw, bh, 8, GxEPD_BLACK);
    } else {
      g.drawRoundRect(bx, by, bw, bh, 8, GxEPD_BLACK);
      g.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 7, GxEPD_BLACK);
    }
    textCentered(g, MonoM, i ? F("YES") : F("NO"), bx + bw / 2,
                 by + bh / 2 + capHeight(g, MonoM) / 2,
                 sel ? GxEPD_WHITE : GxEPD_BLACK);
  }
  drawHint(g, h, F("L/R CHOOSE  TAP CONFIRM"));
}

void Ui::drawInfo(Adafruit_GFX &g, int16_t w, int16_t h) {
  g.fillScreen(GxEPD_WHITE);
  drawTitle(g, w, F("INFO"));
  const int16_t capS = capHeight(g, MonoS);
  char line[32];
  int16_t y = 92;
  auto row = [&](const __FlashStringHelper *label, const char *value) {
    textAt(g, MonoS, label, kMargin + 4, y);
    textRight(g, MonoS, value, w - kMargin - 4, y);
    y += capS + 16;
  };
  row(F("VERSION"), COSMOLIBRE_VERSION);
  const uint32_t up = status_.uptimeS;
  if (up >= 86400) {
    snprintf(line, sizeof(line), "%luD %luH", (unsigned long)(up / 86400),
             (unsigned long)(up / 3600 % 24));
  } else {
    snprintf(line, sizeof(line), "%luH %luM", (unsigned long)(up / 3600),
             (unsigned long)(up / 60 % 60));
  }
  row(F("UPTIME"), line);
  if (status_.batteryPct >= 0) {
    snprintf(line, sizeof(line), "%d%%", status_.batteryPct);
  } else {
    snprintf(line, sizeof(line), "--");
  }
  row(F("BATTERY"), line);
  row(F("IP"), status_.ip[0] ? status_.ip : "--");

  const int16_t areaTop = y + 4, areaBottom = h - 44;
  if (!cfg_.webUi) {
    textCentered(g, MonoS, F("WEB UI IS OFF"), w / 2, (areaTop + areaBottom) / 2);
  } else if (!status_.ip[0]) {
    textCentered(g, MonoS,
                 status_.connecting || status_.radioOff ? F("CONNECTING...")
                                                        : F("NOT CONNECTED"),
                 w / 2, (areaTop + areaBottom) / 2);
  } else {
    drawQr(g, w, areaTop, areaBottom);
  }
  drawHint(g, h, F("TAP OR HOLD MIDDLE: BACK"));
}

/** QR code for http://<ip>/ centred between top and bottom, caption below. */
void Ui::drawQr(Adafruit_GFX &g, int16_t w, int16_t areaTop, int16_t areaBottom) {
  char line[32];
  snprintf(line, sizeof(line), "http://%s/", status_.ip);
  QRCode qr;
  uint8_t buf[106];  // qrcode_getBufferSize(3); version 3 holds up to 53 chars
  qrcode_initText(&qr, buf, 3, ECC_LOW, line);
  const int16_t quiet = 2;
  const int16_t avail = areaBottom - areaTop - 20;
  int16_t scale = avail / (qr.size + 2 * quiet);
  if (scale > 6) scale = 6;
  const int16_t px = qr.size * scale;
  const int16_t x0 = w / 2 - px / 2, y0 = areaTop + (avail - px) / 2;
  for (uint8_t yy = 0; yy < qr.size; yy++) {
    for (uint8_t xx = 0; xx < qr.size; xx++) {
      if (qrcode_getModule(&qr, xx, yy)) {
        g.fillRect(x0 + xx * scale, y0 + yy * scale, scale, scale, GxEPD_BLACK);
      }
    }
  }
  textCentered(g, MonoXS, F("SCAN FOR THE WEB UI"), w / 2, y0 + px + 22);
}

/** Wi-Fi › CONNECT NOW: what the radio is doing, then the address + QR. */
void Ui::drawWebUi(Adafruit_GFX &g, int16_t w, int16_t h) {
  g.fillScreen(GxEPD_WHITE);
  drawTitle(g, w, F("WEB UI"));
  const int16_t mid = h / 2;
  if (!cfg_.webUi) {
    textCentered(g, MonoS, F("WEB UI IS OFF"), w / 2, mid);
    textCentered(g, MonoXS, F("TURN IT ON IN WI-FI"), w / 2, mid + 28);
  } else if (status_.ip[0]) {
    textCentered(g, MonoS, status_.ip, w / 2, 100);
    drawQr(g, w, 116, h - 70);
    textCentered(g, MonoXS, F("ONLINE FOR 15 MIN"), w / 2, h - 44);
  } else if (status_.connecting || status_.radioOff) {
    textCentered(g, MonoS, F("CONNECTING..."), w / 2, mid);
  } else {
    textCentered(g, MonoS, F("NOT CONNECTED"), w / 2, mid);
    textCentered(g, MonoXS, F("RETRYING"), w / 2, mid + 28);
  }
  drawHint(g, h, F("TAP OR HOLD MIDDLE: BACK"));
}

void Ui::drawRing(Adafruit_GFX &g, int16_t w, int16_t h, const FaceInput &face) {
  g.fillScreen(GxEPD_WHITE);
  const bool alarm = ring_ == Ring::Alarm;
  textCentered(g, MonoM, alarm ? F("ALARM") : F("TIMER"), w / 2, 120);
  char big[12];
  if (alarm) {
    const AlarmConfig &a = cfg_.alarms[ringAlarm_];
    snprintf(big, sizeof(big), "%02u:%02u", a.hour, a.minute);
  } else {
    snprintf(big, sizeof(big), "0:00");
  }
  textCentered(g, SerifL, big, w / 2, h / 2 + capHeight(g, SerifL) / 2 - 20);
  char name[16];
  if (alarm) {
    textCentered(g, MonoS, chimeName(cfg_.alarms[ringAlarm_].chime, name, sizeof(name)),
                 w / 2, h / 2 + 70);
  } else {
    textCentered(g, MonoS, F("TIME IS UP"), w / 2, h / 2 + 70);
  }
  textCentered(g, MonoS, F("TAP TO STOP"), w / 2, h - 70);
  if (alarm) textCentered(g, MonoXS, F("HOLD MIDDLE TO SNOOZE"), w / 2, h - 40);
  (void)face;
}
