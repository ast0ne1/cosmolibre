// EEPROM stand-in for desktop tests (preview/tests). Starts erased (0xFF), like flash.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct EEPROMStub {
  uint8_t data[4096];
  size_t size = 0;
  bool dirty = false;
  int commits = 0;
  EEPROMStub() { memset(data, 0xFF, sizeof(data)); }
  void begin(size_t n) { size = n; }
  template <typename T> T &get(int a, T &t) {
    memcpy(&t, data + a, sizeof(T));
    return t;
  }
  template <typename T> const T &put(int a, const T &t) {
    if (memcmp(data + a, &t, sizeof(T))) {
      memcpy(data + a, &t, sizeof(T));
      dirty = true;
    }
    return t;
  }
  bool commit() {
    if (dirty) commits++;
    dirty = false;
    return true;
  }
};
inline EEPROMStub EEPROM;
