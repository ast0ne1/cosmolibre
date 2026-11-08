#include "hal/FaceBitmap.h"

#include <string.h>

namespace {

constexpr int16_t kBandRows = 32;

int16_t rowStride(int16_t w) { return (int16_t)(((w + 31) / 32) * 4); }

/** Adafruit_GFX target that only keeps rows [y0, y0 + kBandRows). */
class BandCanvas : public Adafruit_GFX {
 public:
  BandCanvas(int16_t w, int16_t h, uint8_t *buf)
      : Adafruit_GFX(w, h), stride_(rowStride(w)), buf_(buf) {}

  void setBand(int16_t y0) {
    y0_ = y0;
    memset(buf_, 0xFF, (size_t)stride_ * kBandRows);  // white
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    y -= y0_;
    if (y < 0 || y >= kBandRows || x < 0 || x >= WIDTH) {
      return;
    }
    uint8_t &b = buf_[y * stride_ + (x >> 3)];
    const uint8_t mask = 0x80 >> (x & 7);
    if (color == GxEPD_BLACK) {
      b &= ~mask;
    } else {
      b |= mask;
    }
  }

  void fillScreen(uint16_t color) override {
    memset(buf_, color == GxEPD_BLACK ? 0x00 : 0xFF,
           (size_t)stride_ * kBandRows);
  }

 private:
  int16_t stride_;
  uint8_t *buf_;
  int16_t y0_ = 0;
};

void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

}  // namespace

size_t faceBmpSize(int16_t w, int16_t h) {
  return 62 + (size_t)rowStride(w) * h;
}

void renderBmp(int16_t w, int16_t h, BmpDrawFn draw, void *drawCtx,
               FaceBmpSink sink, void *ctx, void (*onBand)()) {
  const int16_t stride = rowStride(w);

  uint8_t hdr[62] = {0};
  hdr[0] = 'B';
  hdr[1] = 'M';
  put32(hdr + 2, (uint32_t)faceBmpSize(w, h));
  put32(hdr + 10, 62);                       // pixel data offset
  put32(hdr + 14, 40);                       // BITMAPINFOHEADER
  put32(hdr + 18, (uint32_t)w);
  put32(hdr + 22, (uint32_t)(-(int32_t)h));  // negative = top-down rows
  put16(hdr + 26, 1);                        // planes
  put16(hdr + 28, 1);                        // 1 bpp
  put32(hdr + 34, (uint32_t)stride * h);
  put32(hdr + 46, 2);                        // palette entries
  // palette[0] = black (already 0), palette[1] = white
  hdr[58] = hdr[59] = hdr[60] = 0xFF;
  sink(ctx, hdr, sizeof(hdr));

  uint8_t *buf = new uint8_t[(size_t)stride * kBandRows];
  BandCanvas canvas(w, h, buf);
  for (int16_t y0 = 0; y0 < h; y0 += kBandRows) {
    canvas.setBand(y0);
    draw(canvas, drawCtx);
    const int16_t rows = (h - y0) < kBandRows ? (h - y0) : kBandRows;
    sink(ctx, buf, (size_t)stride * rows);
    if (onBand) onBand();
  }
  delete[] buf;
}

namespace {
struct FaceDraw {
  Face *face;
  int16_t w, h;
  const FaceInput *in;
};
void drawFace(Adafruit_GFX &gfx, void *ctx) {
  FaceDraw *d = static_cast<FaceDraw *>(ctx);
  d->face->drawFull(gfx, d->w, d->h, *d->in);
}
}  // namespace

void renderFaceBmp(Face &face, int16_t w, int16_t h, const FaceInput &in,
                   FaceBmpSink sink, void *ctx, void (*onBand)()) {
  FaceDraw d{&face, w, h, &in};
  renderBmp(w, h, drawFace, &d, sink, ctx, onBand);
}
