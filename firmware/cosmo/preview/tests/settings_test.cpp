// Desktop test for src/hal/Settings.cpp: migrations, round trip, validation.
//   built and run by preview/run_tests.py
#include <stdio.h>

#include <EEPROM.h>

#include "hal/Settings.h"

static int g_fail = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
      g_fail++;                                                  \
    }                                                            \
  } while (0)

// The v2 layout as the previous firmware wrote it (copied, not shared).
struct V2 {
  uint32_t magic;
  uint8_t tzId;
  uint8_t beepOnSync;
  uint8_t slots[3][2];
  uint8_t locValid;
  float lat;
  float lon;
  char locLabel[32];
};

static void eraseEeprom() { EEPROM = EEPROMStub(); }

int main() {
  // 1. Blank flash -> defaults, nothing written on boot.
  eraseEeprom();
  {
    Settings s;
    s.begin();
    CHECK(s.tzId() == 0);
    CHECK(Settings::pageUsed(s.ui().faces[0]));
    CHECK(!Settings::pageUsed(s.ui().faces[1]));
    CHECK(s.ui().currentFace == 0);
    CHECK(s.ui().webUi);
    CHECK(!s.showIp());
    CHECK(EEPROM.commits == 0);
  }

  // 2. v2 data (what's on the clock today) migrates: tz, location, face, beep.
  eraseEeprom();
  {
    V2 v{};
    v.magic = 0xC05C0553UL;
    v.tzId = 8;  // Australia East
    v.beepOnSync = 0;
    v.slots[0][0] = 1; v.slots[0][1] = 0;  // Time Big 24h
    v.slots[1][0] = 3; v.slots[1][1] = 0;  // Weather Now
    v.slots[2][0] = 9; v.slots[2][1] = 9;  // garbage -> empty
    v.locValid = 1;
    v.lat = -35.1258f;
    v.lon = 147.3537f;
    strcpy(v.locLabel, "Springfield");
    EEPROM.put(0, v);
    EEPROM.commits = 0;
    EEPROM.dirty = false;

    Settings s;
    s.begin();
    CHECK(s.tzId() == 8);
    CHECK(s.hasLocation());
    CHECK(strcmp(s.locationLabel(), "Springfield") == 0);
    CHECK(s.latitude() < -35.0f && s.longitude() > 147.0f);
    CHECK(!s.beepOnSync());
    CHECK(s.slot(0).kind == 1 && s.slot(0).variant == 0);
    CHECK(s.slot(1).kind == 3 && s.slot(1).variant == 0);
    CHECK(s.slot(2).kind == 0);
    CHECK(EEPROM.commits == 0);  // never write on boot

    // 3. Save -> v3 -> reload keeps everything.
    s.setShowIp(true);
    s.ui().alarms[1] = {true, 6, 45, 1, 3};
    s.ui().volume = 9;
    CHECK(s.save());
    CHECK(EEPROM.commits == 1);
    CHECK(s.save());
    CHECK(EEPROM.commits == 1);  // unchanged -> no flash write
  }
  {
    Settings s;
    s.begin();
    CHECK(s.tzId() == 8);
    CHECK(strcmp(s.locationLabel(), "Springfield") == 0);
    CHECK(s.showIp());
    CHECK(s.slot(0).kind == 1 && s.slot(1).kind == 3);
    CHECK(s.ui().alarms[1].on && s.ui().alarms[1].hour == 6 && s.ui().alarms[1].minute == 45);
    CHECK(s.ui().volume == 9);
  }

  // 4. setPages validation.
  {
    Settings s;
    s.begin();
    FaceLayout pages[kMaxFaces]{};
    CHECK(!s.setPages(pages, 0, true, 5));  // all empty -> rejected
    CHECK(s.slot(0).kind == 1);              // unchanged

    pages[1].slot[0] = {1, 2};   // page 2: Time Small
    pages[1].slot[1] = {9, 9};   // invalid -> dropped
    pages[3].slot[0] = {4, 0};   // page 4: Date
    CHECK(s.setPages(pages, 0, true, 7));  // current=empty page, bad interval
    CHECK(s.ui().currentFace == 1);         // first used page
    CHECK(s.ui().rotateMinutes == 5);       // 7 isn't a choice
    CHECK(s.ui().rotate);
    CHECK(s.ui().faces[1].slot[1].kind == 0);
    CHECK(s.layout().slot[0].kind == 1 && s.layout().slot[0].variant == 2);

    // Legacy single-face editing touches the current page only.
    s.setSlot(2, 2, 1);
    CHECK(s.ui().faces[1].slot[2].kind == 2);
    CHECK(s.ui().faces[3].slot[2].kind == 0);
  }

  // 5. Corrupt v3 values are clamped on load.
  {
    Settings s;
    s.begin();
    s.ui().alarms[0].hour = 99;
    s.ui().alarms[0].chime = 200;
    s.ui().volume = 77;
    s.ui().currentFace = 9;
    s.save();
    Settings t;
    t.begin();
    CHECK(t.ui().alarms[0].hour <= 23);
    CHECK(t.ui().alarms[0].chime < kChimeCount);
    CHECK(t.ui().volume <= 10);
    CHECK(t.ui().currentFace < kMaxFaces && Settings::pageUsed(t.layout()));
  }

  printf(g_fail ? "settings_test: %d FAILED\n" : "settings_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
