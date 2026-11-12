#pragma once

#include <Arduino.h>

enum class TouchPad : uint8_t { None = 0, Left, Middle, Right };

enum class TouchEvent : uint8_t { None = 0, Press, Release, Hold };

struct TouchSample {
  TouchPad pad;
  TouchEvent event;
  uint16_t minA0;
  uint32_t durationMs;
  bool gpio2Low;
};

/** Capacitive top pads: A0 ladder + GPIO2 (see README › Hardware). */
class Touch {
 public:
  void begin();
  /** Poll ADC/GPIO; call frequently from loop(). */
  void update();
  /** True if an edge event is available; fills out and clears it. */
  bool poll(TouchSample &out);

  static const char *padName(TouchPad p);

 private:
  static constexpr uint8_t kGpio2 = 2;
  static constexpr int kIdleA0 = 980;
  static constexpr int kLeftA0Min = 80;
  static constexpr int kLeftA0Max = 400;
  static constexpr int kMiddleA0Max = 50;
  static constexpr uint32_t kDebounceMs = 30;
  static constexpr uint32_t kHoldMs = 800;
  // Read the pads 100 times a second. The ESP8266's ADC shares hardware with
  // the radio: reading it flat out (every loop pass, thousands of times a
  // second) makes Wi-Fi drop its connection and fail to re-join.
  static constexpr uint32_t kSampleMs = 10;

  TouchPad classify(int minA0, bool sawGpio2) const;

  bool active_ = false;
  bool holdSent_ = false;
  int minA0_ = 1024;
  bool sawGpio2_ = false;
  uint32_t activeSince_ = 0;
  uint32_t quietSince_ = 0;
  uint32_t gpio2EdgeMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint8_t gpio2Stable_ = 1;
  uint8_t gpio2Raw_ = 1;

  bool hasEvent_ = false;
  TouchSample event_{};
};
