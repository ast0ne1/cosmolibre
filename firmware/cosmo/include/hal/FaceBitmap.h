#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hal/Face.h"

/**
 * Render the face to a 1‑bit BMP, streamed in horizontal bands so the
 * ESP8266 never needs a full 17 KB frame in RAM. Same pixels as the panel.
 */
using FaceBmpSink = void (*)(void *ctx, const uint8_t *data, size_t len);

/** Draws one complete screen; called once per band (output is clipped). */
using BmpDrawFn = void (*)(Adafruit_GFX &gfx, void *drawCtx);

size_t faceBmpSize(int16_t w, int16_t h);

/** Any screen (face, menu, …) → 1-bit BMP. */
void renderBmp(int16_t w, int16_t h, BmpDrawFn draw, void *drawCtx,
               FaceBmpSink sink, void *ctx, void (*onBand)() = nullptr);

void renderFaceBmp(Face &face, int16_t w, int16_t h, const FaceInput &in,
                   FaceBmpSink sink, void *ctx, void (*onBand)() = nullptr);
