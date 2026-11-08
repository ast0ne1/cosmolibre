#include "hal/Face.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal/Typo.h"

using namespace typo;


namespace {

// Word tables in flash (ESP8266 keeps plain const strings in RAM).
const char kDayNames[7][10] PROGMEM = {"SUNDAY",   "MONDAY", "TUESDAY",
                                       "WEDNESDAY", "THURSDAY", "FRIDAY",
                                       "SATURDAY"};
const char kMonthNames[12][10] PROGMEM = {
    "JANUARY", "FEBRUARY", "MARCH",     "APRIL",   "MAY",      "JUNE",
    "JULY",    "AUGUST",   "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
const char kOnes[20][10] PROGMEM = {
    "ZERO",    "ONE",      "TWO",      "THREE",   "FOUR",
    "FIVE",    "SIX",      "SEVEN",    "EIGHT",   "NINE",
    "TEN",     "ELEVEN",   "TWELVE",   "THIRTEEN", "FOURTEEN",
    "FIFTEEN", "SIXTEEN",  "SEVENTEEN", "EIGHTEEN", "NINETEEN"};
const char kTens[6][7] PROGMEM = {"", "", "TWENTY", "THIRTY", "FORTY", "FIFTY"};

/** Copy a flash string into buf. */
const char *P(char *buf, size_t len, const char *pgm) {
  strncpy_P(buf, pgm, len - 1);
  buf[len - 1] = 0;
  return buf;
}

constexpr int16_t kInset = 8;

// ---- text ----------------------------------------------------------------

struct Line {
  const GFXfont *font;
  const char *text;
};

/** Stack lines (cap-height boxes + gap) centred on (cx, cy). */
void drawBlock(Adafruit_GFX &g, const Line *lines, int n, int16_t cx,
               int16_t cy, int16_t gap) {
  int16_t caps[4];
  int16_t total = gap * (n - 1);
  for (int i = 0; i < n; i++) {
    caps[i] = capHeight(g, lines[i].font);
    total += caps[i];
  }
  int16_t y = cy - total / 2;
  for (int i = 0; i < n; i++) {
    y += caps[i];
    textCentered(g, lines[i].font, lines[i].text, cx, y);
    y += gap;
  }
}

constexpr int kWrapMax = 3;
constexpr int kWrapLen = 24;

/** Greedy word wrap; returns line count (≤ kWrapMax). */
int wrapText(Adafruit_GFX &g, const GFXfont *f, const char *text, int16_t maxW,
             char out[kWrapMax][kWrapLen]) {
  int n = 0;
  out[0][0] = 0;
  const char *p = text;
  while (*p && n < kWrapMax) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    char word[kWrapLen];
    size_t wl = (size_t)(e - p) < sizeof(word) - 1 ? (size_t)(e - p)
                                                   : sizeof(word) - 1;
    memcpy(word, p, wl);
    word[wl] = 0;
    p = e;
    if (!wl) break;

    char trial[kWrapLen * 2];
    snprintf(trial, sizeof(trial), "%s%s%s", out[n], out[n][0] ? " " : "", word);
    if (out[n][0] && measure(g, f, trial).w > (uint16_t)maxW) {
      if (++n >= kWrapMax) break;
      snprintf(out[n], kWrapLen, "%s", word);
    } else {
      snprintf(out[n], kWrapLen, "%s", trial);
    }
  }
  return out[0][0] ? (n < kWrapMax ? n + 1 : kWrapMax) : 0;
}

void drawWrapped(Adafruit_GFX &g, const GFXfont *f, const char *text,
                 const FaceRect &r, int16_t gap) {
  char lines[kWrapMax][kWrapLen];
  int n = wrapText(g, f, text, r.w - 2 * kInset, lines);
  Line block[kWrapMax];
  for (int i = 0; i < n; i++) block[i] = {f, lines[i]};
  drawBlock(g, block, n, r.x + r.w / 2, r.y + r.h / 2, gap);
}

void numberWords(int n, char *out, size_t len) {
  char tens[8], ones[10];
  if (n < 20) {
    P(out, len, kOnes[n]);
  } else if (n % 10 == 0) {
    P(out, len, kTens[n / 10]);
  } else {
    snprintf(out, len, "%s %s", P(tens, sizeof(tens), kTens[n / 10]),
             P(ones, sizeof(ones), kOnes[n % 10]));
  }
}

// ---- icon primitives -----------------------------------------------------

int16_t R(float v) { return (int16_t)lroundf(v); }

void thickLine(Adafruit_GFX &g, float x0, float y0, float x1, float y1,
               float t, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  int16_t rr = R(t / 2);
  if (len > 0.5f) {
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    g.fillTriangle(R(x0 + nx), R(y0 + ny), R(x1 + nx), R(y1 + ny), R(x1 - nx),
                   R(y1 - ny), c);
    g.fillTriangle(R(x0 + nx), R(y0 + ny), R(x1 - nx), R(y1 - ny), R(x0 - nx),
                   R(y0 - ny), c);
  }
  g.fillCircle(R(x0), R(y0), rr, c);
  g.fillCircle(R(x1), R(y1), rr, c);
}

void ring(Adafruit_GFX &g, float cx, float cy, float r, float t) {
  g.fillCircle(R(cx), R(cy), R(r), GxEPD_BLACK);
  g.fillCircle(R(cx), R(cy), R(r - t), GxEPD_WHITE);
}

/** Cloud silhouette, `grow` px larger all round (outline = black grown + white). */
void cloudShape(Adafruit_GFX &g, float cx, float cy, float s, float grow,
                uint16_t c) {
  g.fillCircle(R(cx - 0.22f * s), R(cy + 0.05f * s), R(0.16f * s + grow), c);
  g.fillCircle(R(cx + 0.02f * s), R(cy - 0.08f * s), R(0.24f * s + grow), c);
  g.fillCircle(R(cx + 0.26f * s), R(cy + 0.07f * s), R(0.14f * s + grow), c);
  int16_t x0 = R(cx - 0.22f * s), x1 = R(cx + 0.26f * s);
  int16_t y0 = R(cy + 0.05f * s), y1 = R(cy + 0.21f * s + grow);
  g.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c);
}

void cloud(Adafruit_GFX &g, float cx, float cy, float s, float t) {
  cloudShape(g, cx, cy, s, t, GxEPD_BLACK);
  cloudShape(g, cx, cy, s, 0, GxEPD_WHITE);
}

void sun(Adafruit_GFX &g, float cx, float cy, float s, float t) {
  ring(g, cx, cy, 0.2f * s, t);
  for (int i = 0; i < 8; i++) {
    float a = (float)i * 3.14159265f / 4.0f;
    float ca = cosf(a), sa = sinf(a);
    thickLine(g, cx + ca * 0.31f * s, cy + sa * 0.31f * s, cx + ca * 0.43f * s,
              cy + sa * 0.43f * s, t, GxEPD_BLACK);
  }
}

void dropShape(Adafruit_GFX &g, float cx, float cy, float s, float grow,
               uint16_t c) {
  float r = 0.24f * s + grow;
  float by = cy + 0.1f * s;
  g.fillCircle(R(cx), R(by), R(r), c);
  g.fillTriangle(R(cx), R(cy - 0.38f * s - grow * 1.6f), R(cx - r * 0.93f),
                 R(by - r * 0.36f), R(cx + r * 0.93f), R(by - r * 0.36f), c);
}

void waves(Adafruit_GFX &g, float cx, float cy, float s, float t) {
  for (int row = 0; row < 3; row++) {
    float y0 = cy - 0.2f * s + row * 0.2f * s;
    float px = cx - 0.36f * s, py = y0;
    const int seg = 14;
    for (int k = 1; k <= seg; k++) {
      float x = cx - 0.36f * s + 0.72f * s * k / seg;
      float y = y0 + 0.06f * s * sinf(2.0f * 3.14159265f * k / (seg / 2.0f));
      thickLine(g, px, py, x, y, t, GxEPD_BLACK);
      px = x;
      py = y;
    }
  }
}

}  // namespace

// ---- icons ---------------------------------------------------------------

void faceDrawIcon(Adafruit_GFX &g, WxIcon icon, int16_t cxi, int16_t cyi,
                  int16_t size) {
  const float s = size, cx = cxi, cy = cyi;
  const float t = fmaxf(2.0f, s / 15.0f);
  switch (icon) {
    case WxIcon::Sun:
      sun(g, cx, cy, s, t);
      break;
    case WxIcon::PartlyCloudy:
      sun(g, cx - 0.16f * s, cy - 0.14f * s, 0.72f * s, t);
      cloud(g, cx + 0.08f * s, cy + 0.08f * s, 0.8f * s, t);
      break;
    case WxIcon::Cloud:
      cloud(g, cx, cy, s, t);
      break;
    case WxIcon::Fog:
      for (int i = 0; i < 4; i++) {
        float y = cy - 0.24f * s + i * 0.16f * s;
        float in = (i % 2) ? 0.08f * s : 0.0f;
        thickLine(g, cx - 0.36f * s + in, y, cx + 0.36f * s - in * 2, y, t,
                  GxEPD_BLACK);
      }
      break;
    case WxIcon::Drizzle:
      cloud(g, cx, cy - 0.14f * s, 0.9f * s, t);
      for (int i = -1; i <= 1; i++) {
        g.fillCircle(R(cx + i * 0.17f * s), R(cy + 0.22f * s), R(t * 0.9f),
                     GxEPD_BLACK);
        if (i < 1) {
          g.fillCircle(R(cx + (i + 0.5f) * 0.17f * s), R(cy + 0.36f * s),
                       R(t * 0.9f), GxEPD_BLACK);
        }
      }
      break;
    case WxIcon::Rain:
      cloud(g, cx, cy - 0.14f * s, 0.9f * s, t);
      for (int i = -1; i <= 1; i++) {
        float x = cx + i * 0.17f * s;
        thickLine(g, x + 0.04f * s, cy + 0.17f * s, x - 0.04f * s,
                  cy + 0.38f * s, t, GxEPD_BLACK);
      }
      break;
    case WxIcon::Snow:
      cloud(g, cx, cy - 0.14f * s, 0.9f * s, t);
      for (int i = -1; i <= 1; i++) {
        float x = cx + i * 0.18f * s, y = cy + 0.29f * s, a = 0.07f * s;
        float tt = fmaxf(1.5f, t * 0.7f);
        thickLine(g, x - a, y, x + a, y, tt, GxEPD_BLACK);
        thickLine(g, x - a * 0.5f, y - a * 0.87f, x + a * 0.5f, y + a * 0.87f,
                  tt, GxEPD_BLACK);
        thickLine(g, x - a * 0.5f, y + a * 0.87f, x + a * 0.5f, y - a * 0.87f,
                  tt, GxEPD_BLACK);
      }
      break;
    case WxIcon::Storm:
      cloud(g, cx, cy - 0.14f * s, 0.9f * s, t);
      g.fillTriangle(R(cx + 0.07f * s), R(cy + 0.1f * s), R(cx - 0.1f * s),
                     R(cy + 0.3f * s), R(cx + 0.03f * s), R(cy + 0.3f * s),
                     GxEPD_BLACK);
      g.fillTriangle(R(cx + 0.1f * s), R(cy + 0.24f * s), R(cx - 0.03f * s),
                     R(cy + 0.24f * s), R(cx - 0.07f * s), R(cy + 0.47f * s),
                     GxEPD_BLACK);
      break;
    case WxIcon::Drop:
      dropShape(g, cx, cy, s, t, GxEPD_BLACK);
      dropShape(g, cx, cy, s, 0, GxEPD_WHITE);
      break;
    case WxIcon::Waves:
      waves(g, cx, cy, s, t);
      break;
  }
}

// ---- layout --------------------------------------------------------------

bool Face::slotRect(const FaceLayout &layout, uint8_t slot, int16_t w,
                    FaceRect &out) {
  if (slot >= kFaceSlots) {
    return false;
  }
  uint16_t total = 0, before = 0;
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    uint8_t wt = widgetWeight(layout.slot[i]);
    total += wt;
    if (i < slot) before += wt;
  }
  const uint8_t wt = widgetWeight(layout.slot[slot]);
  if (!wt || !total) {
    return false;
  }
  const int32_t avail = kStatusTop - 4 - kContentTop;
  const int16_t y0 = kContentTop + (int16_t)(avail * before / total);
  const int16_t y1 = kContentTop + (int16_t)(avail * (before + wt) / total);
  out = {0, y0, w, (int16_t)(y1 - y0)};
  return true;
}

int16_t Face::widgetHeight(SlotConfig slot) {
  const int16_t h = (int16_t)(widgetWeight(slot) * 13);
  return h < 110 ? 110 : h;
}

FaceRect Face::statusRect(int16_t w, int16_t h) {
  return {0, kStatusTop, w, (int16_t)(h - kStatusTop)};
}

void Face::drawFull(Adafruit_GFX &gfx, int16_t w, int16_t h,
                    const FaceInput &in) {
  gfx.fillScreen(GxEPD_WHITE);
  for (uint8_t i = 0; i < kFaceSlots; i++) {
    FaceRect r;
    if (slotRect(*in.layout, i, w, r)) {
      drawSlot(gfx, r, in.layout->slot[i], in);
    }
  }
  drawStatus(gfx, w, h, in.status);
}

void Face::drawCard(Adafruit_GFX &g, int16_t w, int16_t h, const Card &c) {
  g.fillScreen(GxEPD_WHITE);
  const int16_t cx = w / 2;
  if (c.title) textSpacedCentered(g, MonoM, c.title, cx, 52, 4);

  const int16_t iy = 140;
  const float t = 3.0f;
  if (c.icon == CardIcon::Wifi) {
    const float PI_F = 3.14159265f;
    for (int k = 1; k <= 3; k++) {
      const float r = 9.0f * k, a0 = -0.75f * PI_F, a1 = -0.25f * PI_F;
      float px = cx + r * cosf(a0), py = iy + 14 + r * sinf(a0);
      for (int sgm = 1; sgm <= 12; sgm++) {
        const float a = a0 + (a1 - a0) * sgm / 12;
        const float x = cx + r * cosf(a), y = iy + 14 + r * sinf(a);
        thickLine(g, px, py, x, y, t, GxEPD_BLACK);
        px = x;
        py = y;
      }
    }
    g.fillCircle(cx, iy + 14, 4, GxEPD_BLACK);
    g.fillCircle(cx, iy + 14, 2, GxEPD_WHITE);
  } else if (c.icon == CardIcon::Cross) {
    thickLine(g, cx - 14, iy - 14, cx + 14, iy + 14, t, GxEPD_BLACK);
    thickLine(g, cx + 14, iy - 14, cx - 14, iy + 14, t, GxEPD_BLACK);
  }

  if (c.heading) textSpacedCentered(g, MonoS, c.heading, cx, 222, 1);
  const int16_t capXS = capHeight(g, MonoXS);
  for (int i = 0; i < 3; i++) {
    if (c.lines[i] && c.lines[i][0]) {
      textSpacedCentered(g, MonoXS, c.lines[i], cx, 252 + i * (capXS + 14), 6);
    }
  }
  if (c.footer && c.footer[0]) {
    const int16_t fy = h - 28;
    textSpacedCentered(g, MonoXS, c.footer, cx, fy, 6);
    if (c.footerSelected) g.fillCircle(kInset + 4, fy - capXS / 2, 3, GxEPD_BLACK);
  }
}

void Face::drawSplash(Adafruit_GFX &gfx, int16_t w, int16_t h,
                      const char *status) {
  gfx.fillScreen(GxEPD_WHITE);
  // Wordmark as in the menu header: "cosmo" bold, "libre" regular, centred
  // as one word (ink start of "cosmo" to ink end of "libre").
  const Ink a = measure(gfx, SerifText, "cosmo");
  const Ink b = measure(gfx, SerifTextLight, "libre");
  // Advance of "cosmo" (where print() leaves the cursor): print it off-screen.
  gfx.setFont(SerifText);
  gfx.setCursor(0, -200);
  gfx.print("cosmo");
  const int16_t advance = gfx.getCursorX();
  const int16_t total = advance + b.x1 + (int16_t)b.w - a.x1;
  const int16_t left = w / 2 - total / 2 - a.x1;
  gfx.setTextColor(GxEPD_BLACK);
  gfx.setCursor(left, h / 2 - 8);
  gfx.print("cosmo");
  gfx.setFont(SerifTextLight);
  gfx.print("libre");
  textSpacedCentered(gfx, MonoXS, "STARTING", w / 2, h / 2 + 28, 6);
  drawStatus(gfx, w, h, status);
}

void Face::drawSlotOnly(Adafruit_GFX &gfx, int16_t w, uint8_t slot,
                        const FaceInput &in) {
  gfx.fillScreen(GxEPD_WHITE);
  FaceRect r;
  if (slotRect(*in.layout, slot, w, r)) {
    drawSlot(gfx, r, in.layout->slot[slot], in);
  }
}

void Face::drawStatus(Adafruit_GFX &gfx, int16_t w, int16_t h,
                      const char *status) {
  (void)w;
  if (status && status[0]) {
    // Clear of the bezel: the panel edge is hidden by the frame.
    textAt(gfx, MonoXS, status, kInset + 14, h - 20);
  }
}

void Face::drawSlot(Adafruit_GFX &gfx, const FaceRect &r, SlotConfig slot,
                    const FaceInput &in) {
  switch ((WidgetKind)slot.kind) {
    case WidgetKind::Time:
      drawTime(gfx, r, slot.variant, in.now);
      break;
    case WidgetKind::Date:
      drawDate(gfx, r, slot.variant, in.now);
      break;
    case WidgetKind::Greeting:
      drawGreeting(gfx, r, slot.variant, in.now);
      break;
    case WidgetKind::Weather:
      drawWeather(gfx, r, slot.variant, *in.wx, in.now);
      break;
    default:
      break;
  }
}

// ---- widgets -------------------------------------------------------------

void Face::drawTime(Adafruit_GFX &g, const FaceRect &r, uint8_t variant,
                    const struct tm *now) {
  const int16_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  const TimeVariant v = (TimeVariant)variant;
  const bool h12 = v == TimeVariant::Big12 || v == TimeVariant::Small12 ||
                   v == TimeVariant::Words;
  int hour = now ? now->tm_hour : 0;
  int min = now ? now->tm_min : 0;
  int hShown = hour;
  if (h12) {
    hShown = hour % 12;
    if (hShown == 0) hShown = 12;
  }

  char a[24], b[24];
  switch (v) {
    case TimeVariant::Big24:
    case TimeVariant::Big12: {
      snprintf(a, sizeof(a), now ? "%02d" : "--", hShown);
      snprintf(b, sizeof(b), now ? "%02d" : "--", min);
      Line lines[] = {{SerifXL, a}, {SerifXL, b}};
      drawBlock(g, lines, 2, cx, cy, 16);
      break;
    }
    case TimeVariant::Small24:
      snprintf(a, sizeof(a), now ? "%02d:%02d" : "--:--", hShown, min);
      textCentered(g, SerifL, a, cx, cy + capHeight(g, SerifL) / 2);
      break;
    case TimeVariant::Small12: {
      snprintf(a, sizeof(a), now ? "%d:%02d" : "--:--", hShown, min);
      snprintf(b, sizeof(b), "%s", now ? (hour < 12 ? "AM" : "PM") : "");
      Ink ia = measure(g, SerifL, a), ib = measure(g, MonoS, b);
      const int16_t gap = 8;
      int16_t x = cx - (int16_t)(ia.w + gap + ib.w) / 2;
      int16_t base = cy + capHeight(g, SerifL) / 2;
      textAt(g, SerifL, a, x - ia.x1, base);
      textAt(g, MonoS, b, x + ia.w + gap - ib.x1, base);
      break;
    }
    case TimeVariant::Words:
    default: {
      if (!now) {
        drawWrapped(g, MonoM, "--", r, 12);
        break;
      }
      // Spoken style, no "OH": NINE O'CLOCK, NINE PAST NINE (1-9 min),
      // NINE TWENTY NINE.
      char words[48], mins[24];
      numberWords(hShown, a, sizeof(a));
      if (min == 0) {
        snprintf(words, sizeof(words), "%s O'CLOCK", a);
      } else if (min < 10) {
        numberWords(min, mins, sizeof(mins));
        snprintf(words, sizeof(words), "%s PAST %s", mins, a);
      } else {
        numberWords(min, mins, sizeof(mins));
        snprintf(words, sizeof(words), "%s %s", a, mins);
      }
      drawWrapped(g, MonoM, words, r, 12);
      break;
    }
  }
}

void Face::drawDate(Adafruit_GFX &g, const FaceRect &r, uint8_t variant,
                    const struct tm *now) {
  const int16_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  char a[24], day[10] = "", month[10] = "";
  if (now) {
    P(day, sizeof(day), kDayNames[now->tm_wday % 7]);
    P(month, sizeof(month), kMonthNames[now->tm_mon % 12]);
  }
  switch ((DateVariant)variant) {
    case DateVariant::Long: {
      if (now) {
        snprintf(a, sizeof(a), "%d %s", now->tm_mday, month);
        if (measure(g, MonoM, a).w > (uint16_t)(r.w - 2 * kInset)) {
          snprintf(a, sizeof(a), "%d %.3s", now->tm_mday, month);
        }
      } else {
        snprintf(a, sizeof(a), "--");
      }
      Line lines[] = {{MonoM, a}, {MonoS, day}};
      drawBlock(g, lines, 2, cx, cy, 12);
      break;
    }
    case DateVariant::MonthDay:
    case DateVariant::DayMonth:
    default: {
      if (now) {
        bool dm = (DateVariant)variant != DateVariant::MonthDay;
        snprintf(a, sizeof(a), "%02d.%02d",
                 dm ? now->tm_mday : now->tm_mon + 1,
                 dm ? now->tm_mon + 1 : now->tm_mday);
      } else {
        snprintf(a, sizeof(a), "--.--");
      }
      Line lines[] = {{MonoL, a}, {MonoS, day}};
      drawBlock(g, lines, 2, cx, cy, 12);
      break;
    }
  }
}

void Face::drawGreeting(Adafruit_GFX &g, const FaceRect &r, uint8_t variant,
                        const struct tm *now) {
  char msg[16] = "HELLO";
  if (now) {
    greetingForHour(now->tm_hour, msg, sizeof(msg));  // stored in capitals
  }
  if (variant != (uint8_t)GreetingVariant::Default) {
    const bool sentence = variant == (uint8_t)GreetingVariant::Sentence;
    for (char *c = msg; *c; c++) {
      if (*c >= 'A' && *c <= 'Z' && !(sentence && c == msg)) *c = (char)(*c - 'A' + 'a');
    }
  }
  drawWrapped(g, MonoM, msg, r, 12);
}

/** Icon on the left, big value + small caption stacked on the right. */
static void iconWithText(Adafruit_GFX &g, const FaceRect &r, WxIcon icon,
                         const char *big, const char *small) {
  const int16_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  const int16_t gap = 14;
  Ink ib = measure(g, MonoL, big), is = measure(g, MonoS, small);
  int16_t colW = ib.w > is.w ? ib.w : is.w;
  int16_t iconS = r.h - 20 < 76 ? r.h - 20 : 76;
  int16_t maxIcon = r.w - 2 * kInset - gap - colW;
  if (iconS > maxIcon) iconS = maxIcon;
  if (iconS < 24) iconS = 24;

  int16_t x0 = cx - (iconS + gap + colW) / 2;
  faceDrawIcon(g, icon, x0 + iconS / 2, cy, iconS);

  const int16_t colX = x0 + iconS + gap;
  const int16_t capL = capHeight(g, MonoL), capS = capHeight(g, MonoS);
  int16_t y = cy - (capL + 10 + capS) / 2 + capL;
  textAt(g, MonoL, big, colX - ib.x1, y);
  y += 10 + capS;
  textAt(g, MonoS, small, colX - is.x1, y);
}

void Face::drawWeather(Adafruit_GFX &g, const FaceRect &r, uint8_t variant,
                       const WeatherSnapshot &wx, const struct tm *now) {
  const int16_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  if (!wx.valid) {
    Line lines[] = {{MonoS, "NO WEATHER"}, {MonoS, "SET LOCATION"}};
    drawBlock(g, lines, 2, cx, cy, 10);
    return;
  }

  char a[32], b[32];
  const WxIcon iconNow = wxIconForCode(wx.codeNow);
  const int16_t capS = capHeight(g, MonoS);

  switch ((WeatherVariant)variant) {
    case WeatherVariant::Now:
      snprintf(a, sizeof(a), "%d" FACE_DEG, wx.tempNow);
      snprintf(b, sizeof(b), "H %d" FACE_DEG " L %d" FACE_DEG, wx.days[0].tMax,
               wx.days[0].tMin);
      iconWithText(g, r, iconNow, a, b);
      break;

    case WeatherVariant::Day: {
      char day[10];
      snprintf(a, sizeof(a), "%d" FACE_DEG, wx.tempNow);
      iconWithText(g, r, iconNow, a,
                   now ? P(day, sizeof(day), kDayNames[now->tm_wday % 7])
                       : wx.days[0].weekday);
      break;
    }

    case WeatherVariant::Description: {
      char desc[17];
      snprintf(a, sizeof(a), "%d" FACE_DEG "C", wx.tempNow);
      Line lines[] = {{MonoS, wxDescription(wx.codeNow, desc, sizeof(desc))},
                      {MonoL, a}};
      drawBlock(g, lines, 2, cx, cy, 14);
      break;
    }

    case WeatherVariant::Details: {
      const WxIcon icons[3] = {WxIcon::Cloud, WxIcon::Drop, WxIcon::Waves};
      const uint8_t vals[3] = {wx.cloudNow, wx.days[0].precipProb,
                               wx.humidityNow};
      int16_t iconS = (int16_t)(r.h * 0.42f);
      if (iconS > 44) iconS = 44;
      const int16_t colW = (r.w - 2 * kInset) / 3;
      const int16_t top = cy - (iconS + 12 + capS) / 2;
      for (int i = 0; i < 3; i++) {
        int16_t ccx = r.x + kInset + colW * i + colW / 2;
        faceDrawIcon(g, icons[i], ccx, top + iconS / 2, iconS);
        snprintf(a, sizeof(a), "%u%%", vals[i]);
        textCentered(g, MonoS, a, ccx, top + iconS + 12 + capS);
      }
      break;
    }

    case WeatherVariant::Big: {
      int16_t iconS = r.h - 12;
      if (iconS > 150) iconS = 150;
      faceDrawIcon(g, iconNow, cx, cy, iconS);
      break;
    }

    case WeatherVariant::Days3:
    default: {
      int16_t iconS = r.h - 2 * capS - 36;
      if (iconS > 52) iconS = 52;
      if (iconS < 20) iconS = 20;
      const int16_t colW = (r.w - 2 * kInset) / 3;
      const int16_t blockH = capS + 10 + iconS + 10 + capS;
      const int16_t top = cy - blockH / 2;
      for (int i = 0; i < 3; i++) {
        const WeatherDay &d = wx.days[i];
        int16_t ccx = r.x + kInset + colW * i + colW / 2;
        textCentered(g, MonoS, d.weekday, ccx, top + capS);
        faceDrawIcon(g, wxIconForCode(d.code), ccx, top + capS + 10 + iconS / 2,
                     iconS);
        snprintf(a, sizeof(a), "%d/%d", d.tMax, d.tMin);
        textCentered(g, MonoS, a, ccx, top + blockH);
      }
      break;
    }
  }
}
