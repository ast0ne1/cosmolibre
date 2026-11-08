// Desktop renderer + simulator for the device UI (src/hal/Ui.cpp, Face.cpp) — long-running, so
// menu state survives between clicks. Driven by preview/preview_server.py.
//
// stdin, one command per line:
//   key=value        inputs: s0..s2 / f0..f3 slots, time, weather, ip, ssid,
//                    rssi, wifi, uptime, sec)
//   !pad L|M|R tap|hold   → replies with one JSON state line
//   !state                → replies with one JSON state line
//   !render               → replies "BMP <n>\n" + n bytes (what the clock shows)
//   !face k:v,k:v,k:v     → BMP of that layout as a face (web editor drafts)
//   !widget k:v           → BMP of one widget at its natural height (toolbox)
//   !reset                → default UiConfig (faces kept)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <string>
#ifdef _WIN32
#include <io.h>
#endif

#include "hal/FaceBitmap.h"
#include "hal/Ui.h"

namespace {

UiConfig g_cfg;
Ui g_ui(g_cfg);
WeatherSnapshot g_wx{};
struct tm g_now {};
bool g_synced = false;
char g_status[48] = "";
UiStatus g_st{};
uint16_t g_actions = 0;  // accumulated until reported
int8_t g_sound = kUiSoundNone;
UiRefresh g_refresh = UiRefresh::None;

uint32_t nowMs() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count() + 1;
}

const struct tm *localNow() {
  if (!g_synced) return nullptr;
  struct tm t = g_now;
  t.tm_isdst = -1;
  mktime(&t);  // fills tm_yday
  g_now.tm_yday = t.tm_yday;
  return &g_now;
}

bool parseSlots(const char *v, FaceLayout &out) {
  FaceLayout l{};
  unsigned k[3] = {0}, var[3] = {0};
  int n = sscanf(v, "%u:%u,%u:%u,%u:%u", &k[0], &var[0], &k[1], &var[1], &k[2],
                 &var[2]);
  if (n < 2) return false;
  for (int i = 0; i < 3 && i * 2 < n; i++) {
    if (isValidSlot((uint8_t)k[i], (uint8_t)var[i])) {
      l.slot[i] = {(uint8_t)k[i], (uint8_t)var[i]};
    }
  }
  out = l;
  return true;
}

void setKey(const char *k, const char *v) {
  int a = atoi(v);
  if (k[0] == 's' && k[1] >= '0' && k[1] < '0' + kFaceSlots && !k[2]) {
    unsigned kind = 0, var = 0;
    if (sscanf(v, "%u:%u", &kind, &var) == 2 && isValidSlot(kind, var)) {
      g_cfg.faces[0].slot[k[1] - '0'] = {(uint8_t)kind, (uint8_t)var};
    }
  } else if (k[0] == 'f' && k[1] >= '0' && k[1] < '0' + kMaxFaces && !k[2]) {
    parseSlots(v, g_cfg.faces[k[1] - '0']);
  } else if (!strcmp(k, "cur")) {
    if (a >= 0 && a < kMaxFaces) g_cfg.currentFace = (uint8_t)a;
  } else if (!strcmp(k, "rotate")) {
    g_cfg.rotate = a != 0;
  } else if (!strcmp(k, "card")) {
    g_ui.setCard(a ? Ui::Card::WifiFailed : Ui::Card::None);
  } else if (!strcmp(k, "webui")) {
    g_cfg.webUi = a != 0;
  } else if (!strcmp(k, "every")) {
    g_cfg.rotateMinutes = (uint8_t)a;
  } else if (!strcmp(k, "synced")) g_synced = a != 0;
  else if (!strcmp(k, "year")) g_now.tm_year = a - 1900;
  else if (!strcmp(k, "mon")) g_now.tm_mon = a - 1;
  else if (!strcmp(k, "mday")) g_now.tm_mday = a;
  else if (!strcmp(k, "wday")) g_now.tm_wday = a;
  else if (!strcmp(k, "hour")) g_now.tm_hour = a;
  else if (!strcmp(k, "min")) g_now.tm_min = a;
  else if (!strcmp(k, "sec")) g_now.tm_sec = a;
  else if (!strcmp(k, "status")) snprintf(g_status, sizeof(g_status), "%s", v);
  else if (!strcmp(k, "ip")) snprintf(g_st.ip, sizeof(g_st.ip), "%s", v);
  else if (!strcmp(k, "ssid")) snprintf(g_st.ssid, sizeof(g_st.ssid), "%s", v);
  else if (!strcmp(k, "rssi")) g_st.rssi = (int16_t)a;
  else if (!strcmp(k, "wifi")) g_st.wifiOk = a != 0;
  else if (!strcmp(k, "uptime")) g_st.uptimeS = (uint32_t)a;
  else if (!strcmp(k, "wx")) g_wx.valid = a != 0;
  else if (!strcmp(k, "temp")) g_wx.tempNow = (int8_t)a;
  else if (!strcmp(k, "code")) g_wx.codeNow = (uint8_t)a;
  else if (!strcmp(k, "cloud")) g_wx.cloudNow = (uint8_t)a;
  else if (!strcmp(k, "hum")) g_wx.humidityNow = (uint8_t)a;
  else if (k[0] == 'd' && k[1] >= '0' && k[1] <= '2' && !k[2]) {
    WeatherDay &d = g_wx.days[k[1] - '0'];
    int mx = 0, mn = 0, code = 0, prob = 0;
    char wd[8] = "";
    if (sscanf(v, "%3[^,],%d,%d,%d,%d", wd, &mx, &mn, &code, &prob) >= 4) {
      snprintf(d.weekday, sizeof(d.weekday), "%s", wd);
      d.tMax = (int8_t)mx;
      d.tMin = (int8_t)mn;
      d.code = (uint8_t)code;
      d.precipProb = (uint8_t)prob;
    }
  }
}

/** Collect what main.cpp would act on. */
void drain() {
  g_actions |= g_ui.takeActions();
  int8_t s = g_ui.takeSound();
  if (s != kUiSoundNone) g_sound = s;
  UiRefresh r = g_ui.takeRefresh();
  if ((uint8_t)r > (uint8_t)g_refresh) g_refresh = r;
}

void printState() {
  drain();
  std::string a;
  auto add = [&](uint16_t bit, const char *name) {
    if (g_actions & bit) a += std::string(a.empty() ? "" : ",") + "\"" + name + "\"";
  };
  add(kUiSaveConfig, "save");
  add(kUiOpenPortal, "portal");
  add(kUiForgetWifi, "forget");
  add(kUiWebUiChanged, "webui");
  add(kUiSyncNow, "sync");
  char sound[24] = "null";
  if (g_sound == kUiSoundBeep) {
    snprintf(sound, sizeof(sound), "\"beep\"");
  } else if (g_sound >= 0) {
    char f[16];
    snprintf(sound, sizeof(sound), "\"%s\"", chimeFile((uint8_t)g_sound, f, sizeof(f)));
  }
  static const char *kRefresh[] = {"none", "partial", "full"};
  printf("{\"inMenu\":%s,\"ringing\":%s,\"timer\":%s,\"face\":%u,"
         "\"rotate\":%s,\"every\":%u,\"webUi\":%s,"
         "\"volume\":%u,\"sound\":%s,\"refresh\":\"%s\",\"actions\":[%s]}\n",
         g_ui.inMenu() ? "true" : "false", g_ui.ringing() ? "true" : "false",
         g_ui.timerRunning() ? "true" : "false", g_cfg.currentFace,
         g_cfg.rotate ? "true" : "false", g_cfg.rotateMinutes,
         g_cfg.webUi ? "true" : "false", g_cfg.volume, sound,
         kRefresh[(uint8_t)g_refresh], a.c_str());
  fflush(stdout);
  g_actions = 0;
  g_sound = kUiSoundNone;
  g_refresh = UiRefresh::None;
}

std::string g_bmp;
void sinkString(void *, const uint8_t *data, size_t len) {
  g_bmp.append((const char *)data, len);
}

struct UiDraw {
  int16_t w, h;
  const FaceInput *in;
  uint32_t ms;
};

void drawUi(Adafruit_GFX &gfx, void *ctx) {
  UiDraw *d = static_cast<UiDraw *>(ctx);
  g_ui.draw(gfx, d->w, d->h, *d->in, d->ms);
}

void renderUiBmp(int16_t w, int16_t h, const FaceInput &in, uint32_t ms) {
  UiDraw d{w, h, &in, ms};
  renderBmp(w, h, drawUi, &d, sinkString, nullptr);
}

Face g_face;

struct WidgetDraw {
  FaceRect r;
  SlotConfig slot;
  const FaceInput *in;
};

void drawWidget(Adafruit_GFX &gfx, void *ctx) {
  WidgetDraw *d = static_cast<WidgetDraw *>(ctx);
  gfx.fillScreen(GxEPD_WHITE);
  g_face.drawWidget(gfx, d->r, d->slot, *d->in);
}

void sendBmp() {
  printf("BMP %u\n", (unsigned)g_bmp.size());
  fwrite(g_bmp.data(), 1, g_bmp.size(), stdout);
  fflush(stdout);
}

}  // namespace

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  uiDefaultConfig(g_cfg);
  g_st.batteryPct = -1;

  char line[512];
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == '!') {
      const uint32_t ms = nowMs();
      g_ui.setStatus(g_st);
      g_ui.tick(ms, localNow());
      if (!strncmp(line, "!pad ", 5)) {
        const char p = line[5];
        const bool hold = strstr(line + 6, "hold") != nullptr;
        const Pad pad = p == 'L' ? Pad::Left : p == 'R' ? Pad::Right : Pad::Middle;
        g_ui.onPad(pad, hold ? Gesture::Hold : Gesture::Tap, ms, localNow());
        printState();
      } else if (!strcmp(line, "!state")) {
        printState();
      } else if (!strcmp(line, "!render")) {
        drain();
        FaceInput in{&g_cfg.faces[0], &g_wx, localNow(), g_status};
        g_bmp.clear();
        renderUiBmp(280, 480, in, ms);
        sendBmp();
      } else if (!strcmp(line, "!setupcard")) {
        struct D {
          static void draw(Adafruit_GFX &g, void *) {
            const Card card{"WIFI SETUP", CardIcon::Wifi, "START WIFI",
                            {"USE YOUR PHONE", "OR PC WIFI TO", "JOIN COSMOLIBRE"},
                            "OPEN 192.168.4.1", false};
            g_face.drawCard(g, 280, 480, card);
          }
        };
        g_bmp.clear();
        renderBmp(280, 480, D::draw, nullptr, sinkString, nullptr);
        sendBmp();
      } else if (!strcmp(line, "!splash")) {
        struct D {
          static void draw(Adafruit_GFX &g, void *) {
            g_face.drawSplash(g, 280, 480, "join Home WiFi");
          }
        };
        g_bmp.clear();
        renderBmp(280, 480, D::draw, nullptr, sinkString, nullptr);
        sendBmp();
      } else if (!strncmp(line, "!face ", 6)) {
        FaceLayout layout{};
        parseSlots(line + 6, layout);
        FaceInput in{&layout, &g_wx, localNow(), g_status};
        g_bmp.clear();
        renderFaceBmp(g_face, 280, 480, in, sinkString, nullptr);
        sendBmp();
      } else if (!strncmp(line, "!widget ", 8)) {
        unsigned kind = 0, var = 0;
        SlotConfig slot{0, 0};
        if (sscanf(line + 8, "%u:%u", &kind, &var) == 2 && isValidSlot(kind, var)) {
          slot = {(uint8_t)kind, (uint8_t)var};
        }
        static const FaceLayout kEmpty{};
        FaceInput in{&kEmpty, &g_wx, localNow(), ""};
        WidgetDraw d{{0, 0, 280, Face::widgetHeight(slot)}, slot, &in};
        g_bmp.clear();
        renderBmp(280, d.r.h, drawWidget, &d, sinkString, nullptr);
        sendBmp();
      } else if (!strcmp(line, "!reset")) {
        FaceLayout faces[kMaxFaces];
        memcpy(faces, g_cfg.faces, sizeof(faces));
        uiDefaultConfig(g_cfg);
        memcpy(g_cfg.faces, faces, sizeof(faces));
        printState();
      }
      continue;
    }
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    setKey(line, eq + 1);
  }
  return 0;
}
