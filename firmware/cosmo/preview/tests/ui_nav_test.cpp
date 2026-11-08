// Desktop test for src/hal/Ui.cpp menu navigation: the ↰ back item, wrapping,
// and the optional hold-to-go-back. Built and run by preview/run_tests.py.
#include <stdio.h>

#include "hal/Ui.h"

static int g_fail = 0;
#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      g_fail++;                                              \
    }                                                        \
  } while (0)

static uint32_t t = 1000;
static void tap(Ui &ui, Pad p) { ui.onPad(p, Gesture::Tap, t += 100, nullptr); }
static void hold(Ui &ui, Pad p) { ui.onPad(p, Gesture::Hold, t += 900, nullptr); }

int main() {
  UiConfig cfg;
  uiDefaultConfig(cfg);
  cfg.faces[0].slot[0] = {1, 0};
  Ui ui(cfg);

  // Hold ● opens the menu; the dot starts on the first item (APPS).
  hold(ui, Pad::Middle);
  CHECK(ui.inMenu());

  // ◀ from the first item lands on ↰; ● there closes the top-level menu.
  tap(ui, Pad::Left);
  tap(ui, Pad::Middle);
  CHECK(!ui.inMenu());

  // ▶ wraps: 5 items + ↰ = 6 positions → 6 taps returns to APPS.
  hold(ui, Pad::Middle);
  for (int i = 0; i < 6; i++) tap(ui, Pad::Right);
  tap(ui, Pad::Middle);  // APPS → submenu
  CHECK(ui.inMenu());
  // In a submenu, ↰ goes back one level (still in the menu).
  tap(ui, Pad::Left);    // first item → ↰
  tap(ui, Pad::Middle);  // back to the top level
  CHECK(ui.inMenu());
  tap(ui, Pad::Left);    // ↰ at the top level
  tap(ui, Pad::Middle);
  CHECK(!ui.inMenu());

  // Hold-to-back on (default): a hold in the top level closes the menu.
  hold(ui, Pad::Middle);
  hold(ui, Pad::Middle);
  CHECK(!ui.inMenu());

  // Hold-to-back off: a hold in a menu does nothing.
  cfg.holdBack = false;
  hold(ui, Pad::Middle);
  CHECK(ui.inMenu());
  hold(ui, Pad::Middle);
  CHECK(ui.inMenu());
  tap(ui, Pad::Left);
  tap(ui, Pad::Middle);  // ↰ still works
  CHECK(!ui.inMenu());

  // Settings › HOLD TO BACK toggles the option (and asks to save).
  cfg.holdBack = true;
  ui.takeActions();
  hold(ui, Pad::Middle);
  for (int i = 0; i < 3; i++) tap(ui, Pad::Right);  // → SETTINGS
  tap(ui, Pad::Middle);
  for (int i = 0; i < 3; i++) tap(ui, Pad::Right);  // SOUND → … → HOLD TO BACK
  tap(ui, Pad::Middle);
  CHECK(!cfg.holdBack);
  CHECK(ui.takeActions() & kUiSaveConfig);

  printf(g_fail ? "ui_nav_test: %d FAILED\n" : "ui_nav_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
