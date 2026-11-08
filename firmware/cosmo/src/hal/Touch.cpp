#include "hal/Touch.h"

void Touch::begin() {
  pinMode(kGpio2, INPUT_PULLUP);
  delay(2);
  gpio2Raw_ = digitalRead(kGpio2) ? 1 : 0;
  gpio2Stable_ = gpio2Raw_;
  gpio2EdgeMs_ = millis();
  active_ = false;
  hasEvent_ = false;
  quietSince_ = 0;
}

const char *Touch::padName(TouchPad p) {
  switch (p) {
    case TouchPad::Left:
      return "LEFT";
    case TouchPad::Middle:
      return "MIDDLE";
    case TouchPad::Right:
      return "RIGHT";
    default:
      return "NONE";
  }
}

TouchPad Touch::classify(int minA0, bool sawGpio2) const {
  if (minA0 <= kMiddleA0Max) {
    return TouchPad::Middle;
  }
  if (minA0 >= kLeftA0Min && minA0 <= kLeftA0Max) {
    return TouchPad::Left;
  }
  if (sawGpio2 && minA0 >= kIdleA0 - 40) {
    return TouchPad::Right;
  }
  if (sawGpio2 && minA0 < kIdleA0 - 40) {
    return TouchPad::Left;
  }
  if (sawGpio2) {
    return TouchPad::Right;
  }
  return TouchPad::None;
}

void Touch::update() {
  const uint32_t now = millis();
  const int a0 = analogRead(A0);

  uint8_t g2 = digitalRead(kGpio2) ? 1 : 0;
  if (g2 != gpio2Raw_) {
    gpio2Raw_ = g2;
    gpio2EdgeMs_ = now;
  } else if ((now - gpio2EdgeMs_) >= kDebounceMs && g2 != gpio2Stable_) {
    gpio2Stable_ = g2;
  }
  const bool g2Low = (gpio2Stable_ == 0);

  const bool a0Active = (a0 < kIdleA0 - 50);
  const bool digActive = g2Low;

  if (!active_ && (a0Active || digActive)) {
    active_ = true;
    holdSent_ = false;
    activeSince_ = now;
    quietSince_ = 0;
    minA0_ = a0;
    sawGpio2_ = g2Low;
  }

  if (!active_) {
    return;
  }

  if (a0 < minA0_) {
    minA0_ = a0;
  }
  if (g2Low) {
    sawGpio2_ = true;
  }

  const uint32_t dur = now - activeSince_;
  if (!holdSent_ && dur >= kHoldMs && (a0Active || digActive)) {
    TouchPad pad = classify(minA0_, sawGpio2_);
    event_ = {pad, TouchEvent::Hold, (uint16_t)minA0_, dur, sawGpio2_};
    hasEvent_ = true;
    holdSent_ = true;
  }

  if (!a0Active && !digActive) {
    if (quietSince_ == 0) {
      quietSince_ = now;
    }
    if (now - quietSince_ >= kDebounceMs) {
      // A hold already fired its own event; don't also report it as a tap.
      if (!holdSent_) {
        TouchPad pad = classify(minA0_, sawGpio2_);
        event_ = {pad, TouchEvent::Release, (uint16_t)minA0_,
                  now - activeSince_, sawGpio2_};
        hasEvent_ = true;
      }
      active_ = false;
      quietSince_ = 0;
    }
  } else {
    quietSince_ = 0;
  }
}

bool Touch::poll(TouchSample &out) {
  if (!hasEvent_) {
    return false;
  }
  out = event_;
  hasEvent_ = false;
  return true;
}
