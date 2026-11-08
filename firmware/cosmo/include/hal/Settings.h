#pragma once

#include <Arduino.h>
#include "hal/Ui.h"
#include "hal/Widgets.h"

/**
 * EEPROM settings — timezone, location, pages, alarms, sound, night mode,
 * web UI switch. Does not touch Wi‑Fi creds. Only written on an explicit
 * save — never on boot, so a flaky first boot can't disturb flash.
 */
class Settings {
 public:
  void begin();
  bool save();

  uint8_t tzId() const { return tzId_; }
  bool setTzId(uint8_t id);
  const char *tzPosix() const;
  const char *tzLabel() const;

  bool beepOnSync() const { return ui_.syncBeep; }
  void setBeepOnSync(bool on) { ui_.syncBeep = on; }

  /** Show the IP on the face's bottom line even when all is well. */
  bool showIp() const { return showIp_; }
  void setShowIp(bool on) { showIp_ = on; }

  /** Pages, alarms, sound, night mode, web UI switch (owned by Ui). */
  UiConfig &ui() { return ui_; }
  const UiConfig &ui() const { return ui_; }

  // The page currently shown (callers that edit "the face" edit this page).
  const FaceLayout &layout() const;
  SlotConfig slot(uint8_t i) const {
    return i < kFaceSlots ? layout().slot[i] : SlotConfig{0, 0};
  }
  /** Ignores invalid kind/variant combinations. */
  void setSlot(uint8_t i, uint8_t kind, uint8_t variant);
  void applyPreset(uint8_t presetId);

  /**
   * Replace all pages + selection + rotation (web UI). Invalid slots become
   * empty; `current` falls back to the first used page. False if no page is
   * used (nothing changed).
   */
  bool setPages(const FaceLayout *pages, uint8_t current, bool rotate,
                uint8_t rotateMinutes);
  static bool pageUsed(const FaceLayout &p);

  bool hasLocation() const { return locValid_; }
  float latitude() const { return lat_; }
  float longitude() const { return lon_; }
  const char *locationLabel() const { return locLabel_; }
  void setLocation(float lat, float lon, const char *label);
  void clearLocation();

  // Tables live in flash. The *At() string accessors return a shared static
  // buffer — use the result before calling the same accessor again.
  static uint8_t tzCount();
  static const char *tzLabelAt(uint8_t id);
  static const char *tzPosixAt(uint8_t id);
  static uint8_t presetCount();
  static const char *presetNameAt(uint8_t id);
  static bool presetLayoutAt(uint8_t id, FaceLayout &out);

 private:
  void load();
  void defaults();
  void sanitize();

  uint8_t tzId_ = 0;
  bool showIp_ = false;
  UiConfig ui_{};
  bool locValid_ = false;
  float lat_ = 0;
  float lon_ = 0;
  char locLabel_[32] = "";
};
