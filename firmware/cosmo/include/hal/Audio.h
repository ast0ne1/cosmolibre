#pragma once

#include <Arduino.h>

/** MAX98357A via I2S — BCLK=15 LRCK=2 DOUT=3 (see README › Hardware). */
class Audio {
 public:
  void begin();
  /** Blocking short beep; pauses touch-friendly restore of GPIO2 / EPD CS. */
  void playBeep();
  /** 0 (silent) … 10 (loud). */
  void setVolume(uint8_t v) { volume_ = v > 10 ? 10 : v; }
  uint8_t volume() const { return volume_; }

 private:
  static constexpr int kBclk = 15;
  static constexpr int kLrck = 2;
  static constexpr int kDout = 3;
  uint8_t volume_ = 6;
};
