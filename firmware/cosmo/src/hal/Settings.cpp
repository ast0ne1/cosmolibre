#include "hal/Settings.h"

#include <EEPROM.h>

// v1 (two fixed slots) — read once for migration, never written.
static constexpr uint32_t kMagicV1 = 0xC05C0552UL;
struct StoredSettingsV1 {
  uint32_t magic;
  uint8_t tzId;
  uint8_t beepOnSync;
  uint8_t primaryKind;
  uint8_t primaryVariant;
  uint8_t secondaryKind;
  uint8_t secondaryVariant;
  uint8_t locValid;
  float lat;
  float lon;
  char locLabel[32];
};

// v2 (one face, three slots) — read once for migration, never written.
static constexpr uint32_t kMagicV2 = 0xC05C0553UL;
struct StoredSettingsV2 {
  uint32_t magic;
  uint8_t tzId;
  uint8_t beepOnSync;
  uint8_t slots[kFaceSlots][2];  // kind, variant
  uint8_t locValid;
  float lat;
  float lon;
  char locLabel[32];
};

// v3/v4 share this header; the UiConfig after it changed between them.
struct StoredHeader {
  uint32_t magic;
  uint8_t tzId;
  uint8_t showIp;
  uint8_t locValid;
  uint8_t reserved;
  float lat;
  float lon;
  char locLabel[32];
};

// v3 (pre-release builds): only the header is reused when migrating.
static constexpr uint32_t kMagicV3 = 0xC05C0554UL;

// v4: + 3 alarms, hold-to-back. Its UiConfig is v5's minus the trailing
// wifiAlways, so it loads through the v5 struct with that field reset.
static constexpr uint32_t kMagicV4 = 0xC05C0555UL;

// v5: + Wi-Fi power saver / always on. Bump magic when the layout changes (and
// migrate the previous one in load()).
static constexpr uint32_t kMagicV5 = 0xC05C0556UL;
struct StoredSettingsV5 {
  StoredHeader h;
  UiConfig ui;  // POD; same compiler writes and reads it
};

static constexpr size_t kEepromSize = 256;
static_assert(sizeof(StoredSettingsV5) <= kEepromSize, "EEPROM too small");

struct TzEntry {
  char label[26];
  char posix[34];
};

// In flash. Append only: tzId is stored in EEPROM by index.
static const TzEntry kTzTable[] PROGMEM = {
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UK / Ireland", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"UTC", "UTC0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"Australia East (Syd/Mel)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia Brisbane", "AEST-10"},
    {"Australia Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Perth", "AWST-8"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Japan", "JST-9"},
    {"China / Singapore", "CST-8"},
    {"India", "IST-5:30"},
};

static constexpr uint8_t kTzCount = sizeof(kTzTable) / sizeof(kTzTable[0]);

struct Preset {
  char name[16];
  FaceLayout layout;
};

#define SLOT(kind, var)   { (uint8_t)WidgetKind::kind, (uint8_t)var }
#define NONE   { 0, 0 }

// Modelled on the OEM stock apps (TIME & DATE, …) and the GIF compositions.
static const Preset kPresets[] PROGMEM = {
    {"Time & date",
     {{SLOT(Time, TimeVariant::Big24), SLOT(Date, DateVariant::DayMonth), NONE}}},
    {"Time & weather",
     {{SLOT(Time, TimeVariant::Big24), SLOT(Weather, WeatherVariant::Day), NONE}}},
    {"Morning",
     {{SLOT(Time, TimeVariant::Small24), SLOT(Greeting, GreetingVariant::Default),
       SLOT(Weather, WeatherVariant::Description)}}},
    {"Weather focus",
     {{SLOT(Time, TimeVariant::Small24), SLOT(Weather, WeatherVariant::Big),
       SLOT(Weather, WeatherVariant::Details)}}},
    {"Forecast",
     {{SLOT(Time, TimeVariant::Small24), SLOT(Date, DateVariant::DayMonth),
       SLOT(Weather, WeatherVariant::Days3)}}},
};

#undef SLOT
#undef NONE

static constexpr uint8_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

uint8_t Settings::tzCount() { return kTzCount; }
uint8_t Settings::presetCount() { return kPresetCount; }

static char s_tzLabel[sizeof(TzEntry::label)];
static char s_tzPosix[sizeof(TzEntry::posix)];
static char s_presetName[sizeof(Preset::name)];

static const char *copyP(char *buf, size_t len, const char *pgm) {
  strncpy_P(buf, pgm, len - 1);
  buf[len - 1] = 0;
  return buf;
}

const char *Settings::tzLabelAt(uint8_t id) {
  return copyP(s_tzLabel, sizeof(s_tzLabel),
               kTzTable[id < kTzCount ? id : 0].label);
}

const char *Settings::tzPosixAt(uint8_t id) {
  return copyP(s_tzPosix, sizeof(s_tzPosix),
               kTzTable[id < kTzCount ? id : 0].posix);
}

const char *Settings::presetNameAt(uint8_t id) {
  return copyP(s_presetName, sizeof(s_presetName),
               kPresets[id < kPresetCount ? id : 0].name);
}

bool Settings::presetLayoutAt(uint8_t id, FaceLayout &out) {
  if (id >= kPresetCount) {
    return false;
  }
  memcpy_P(&out, &kPresets[id].layout, sizeof(out));
  return true;
}

void Settings::defaults() {
  tzId_ = 0;
  showIp_ = false;
  uiDefaultConfig(ui_);
  presetLayoutAt(0, ui_.faces[0]);  // one page to start; add more in the web UI
  locValid_ = false;
  lat_ = 0;
  lon_ = 0;
  locLabel_[0] = 0;
}

void Settings::begin() {
  EEPROM.begin(kEepromSize);
  load();
}

static SlotConfig validSlot(uint8_t kind, uint8_t variant) {
  if (!isValidSlot(kind, variant)) {
    return {0, 0};
  }
  return {kind, variant};
}

bool Settings::pageUsed(const FaceLayout &p) {
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    if (p.slot[i].kind) return true;
  }
  return false;
}

/** Clamp everything read from EEPROM / the web to values the firmware accepts. */
void Settings::sanitize() {
  bool any = false;
  for (uint8_t f = 0; f < kMaxFaces; f++) {
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      SlotConfig &sl = ui_.faces[f].slot[i];
      sl = validSlot(sl.kind, sl.variant);
    }
    any |= pageUsed(ui_.faces[f]);
  }
  if (!any) presetLayoutAt(0, ui_.faces[0]);
  if (ui_.currentFace >= kMaxFaces || !pageUsed(ui_.faces[ui_.currentFace])) {
    ui_.currentFace = 0;
    while (ui_.currentFace < kMaxFaces - 1 && !pageUsed(ui_.faces[ui_.currentFace])) {
      ui_.currentFace++;
    }
  }
  bool okRotate = false;
  for (uint8_t m : kRotateChoices) okRotate |= m == ui_.rotateMinutes;
  if (!okRotate) ui_.rotateMinutes = 5;
  for (AlarmConfig &a : ui_.alarms) {
    if (a.hour > 23) a.hour = 7;
    if (a.minute > 59) a.minute = 0;
    if (a.days > 2) a.days = 0;
    if (a.chime >= kChimeCount) a.chime = 0;
  }
  if (ui_.volume > 10) ui_.volume = 6;
  ui_.rotate = ui_.rotate != 0;
  ui_.touchBeep = ui_.touchBeep != 0;
  ui_.syncBeep = ui_.syncBeep != 0;
  ui_.nightOn = ui_.nightOn != 0;
  ui_.webUi = ui_.webUi != 0;
  ui_.holdBack = ui_.holdBack != 0;
  ui_.wifiAlways = ui_.wifiAlways != 0;
  if (ui_.nightFromHour > 23) ui_.nightFromHour = 22;
  if (ui_.nightToHour > 23) ui_.nightToHour = 7;
}

void Settings::load() {
  defaults();

  StoredSettingsV5 v5{};
  EEPROM.get(0, v5);
  const StoredHeader &hd = v5.h;
  if ((hd.magic == kMagicV5 || hd.magic == kMagicV4 || hd.magic == kMagicV3) &&
      hd.tzId < kTzCount) {
    tzId_ = hd.tzId;
    showIp_ = hd.showIp != 0;
    locValid_ = hd.locValid != 0;
    lat_ = hd.lat;
    lon_ = hd.lon;
    strncpy(locLabel_, hd.locLabel, sizeof(locLabel_) - 1);
    locLabel_[sizeof(locLabel_) - 1] = 0;
    if (hd.magic == kMagicV5) {
      ui_ = v5.ui;
    } else if (hd.magic == kMagicV4) {
      ui_ = v5.ui;
      ui_.wifiAlways = false;  // new in v5: start in power saver
      Serial.println(F("Settings: migrating v4 (Wi-Fi power saver on)"));
    } else {
      Serial.println(F("Settings: migrating v3 (menu options reset)"));
    }
    sanitize();
    return;
  }

  // Older layouts: keep timezone/location/face. Persisted on the next user
  // Save (never write EEPROM on boot).
  StoredSettingsV2 v2{};
  EEPROM.get(0, v2);
  if (v2.magic == kMagicV2 && v2.tzId < kTzCount) {
    Serial.println(F("Settings: migrating v2"));
    tzId_ = v2.tzId;
    ui_.syncBeep = v2.beepOnSync != 0;
    for (uint8_t i = 0; i < kFaceSlots; i++) {
      ui_.faces[0].slot[i] = validSlot(v2.slots[i][0], v2.slots[i][1]);
    }
    locValid_ = v2.locValid != 0;
    lat_ = v2.lat;
    lon_ = v2.lon;
    strncpy(locLabel_, v2.locLabel, sizeof(locLabel_) - 1);
    locLabel_[sizeof(locLabel_) - 1] = 0;
    sanitize();
    return;
  }

  StoredSettingsV1 v1{};
  EEPROM.get(0, v1);
  if (v1.magic == kMagicV1 && v1.tzId < kTzCount) {
    Serial.println(F("Settings: migrating v1"));
    tzId_ = v1.tzId;
    ui_.syncBeep = v1.beepOnSync != 0;
    locValid_ = v1.locValid != 0;
    lat_ = v1.lat;
    lon_ = v1.lon;
    strncpy(locLabel_, v1.locLabel, sizeof(locLabel_) - 1);
    locLabel_[sizeof(locLabel_) - 1] = 0;
    ui_.faces[0].slot[0] = validSlot(v1.primaryKind, v1.primaryVariant);
    ui_.faces[0].slot[1] = validSlot(v1.secondaryKind, v1.secondaryVariant);
    ui_.faces[0].slot[2] = {0, 0};
    sanitize();
  }
}

bool Settings::save() {
  sanitize();
  StoredSettingsV5 s{};
  s.h.magic = kMagicV5;
  s.h.tzId = tzId_;
  s.h.showIp = showIp_ ? 1 : 0;
  s.h.locValid = locValid_ ? 1 : 0;
  s.h.lat = lat_;
  s.h.lon = lon_;
  strncpy(s.h.locLabel, locLabel_, sizeof(s.h.locLabel) - 1);
  s.h.locLabel[sizeof(s.h.locLabel) - 1] = 0;
  s.ui = ui_;
  EEPROM.put(0, s);
  return EEPROM.commit();  // only writes flash if something changed
}

bool Settings::setTzId(uint8_t id) {
  if (id >= kTzCount) {
    return false;
  }
  tzId_ = id;
  return true;
}

const char *Settings::tzPosix() const { return tzPosixAt(tzId_); }
const char *Settings::tzLabel() const { return tzLabelAt(tzId_); }

const FaceLayout &Settings::layout() const {
  return ui_.faces[ui_.currentFace < kMaxFaces ? ui_.currentFace : 0];
}

void Settings::setSlot(uint8_t i, uint8_t kind, uint8_t variant) {
  if (i >= kFaceSlots || !isValidSlot(kind, variant)) {
    return;
  }
  ui_.faces[ui_.currentFace % kMaxFaces].slot[i] = {kind, variant};
}

void Settings::applyPreset(uint8_t presetId) {
  presetLayoutAt(presetId, ui_.faces[ui_.currentFace % kMaxFaces]);
}

bool Settings::setPages(const FaceLayout *pages, uint8_t current, bool rotate,
                        uint8_t rotateMinutes) {
  bool any = false;
  for (uint8_t f = 0; f < kMaxFaces; f++) any |= pageUsed(pages[f]);
  if (!any) {
    return false;
  }
  for (uint8_t f = 0; f < kMaxFaces; f++) ui_.faces[f] = pages[f];
  ui_.currentFace = current;
  ui_.rotate = rotate;
  ui_.rotateMinutes = rotateMinutes;
  sanitize();  // drops invalid slots, fixes current/interval
  return true;
}

void Settings::setLocation(float lat, float lon, const char *label) {
  lat_ = lat;
  lon_ = lon;
  locValid_ = true;
  if (label) {
    strncpy(locLabel_, label, sizeof(locLabel_) - 1);
    locLabel_[sizeof(locLabel_) - 1] = 0;
  }
}

void Settings::clearLocation() {
  locValid_ = false;
  lat_ = 0;
  lon_ = 0;
  locLabel_[0] = 0;
}
