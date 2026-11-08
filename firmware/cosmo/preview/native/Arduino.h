// Minimal Arduino shim so Adafruit_GFX + Face build on the desktop (preview only).
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARDUINO
#define ARDUINO 100
#endif
#define PROGMEM
#define PSTR(s) (s)
#define PGM_P const char *
#define strncpy_P strncpy
#define memcpy_P memcpy
#define snprintf_P snprintf
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
// Pointer-width: Adafruit_GFX only uses this for pgm_read_pointer (64-bit host).
#define pgm_read_dword(addr) (*(const uintptr_t *)(addr))

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886
#endif
#define radians(deg) ((deg) * DEG_TO_RAD)

typedef bool boolean;
typedef uint8_t byte;

class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))

class String {
 public:
  String(const char *s = "") : s_(s ? s : "") {}
  const char *c_str() const { return s_; }
  size_t length() const { return strlen(s_); }

 private:
  const char *s_;
};

#include "Print.h"

// Serial stand-in: firmware logging is dropped on the desktop.
struct SerialStub {
  void println(const __FlashStringHelper *) {}
  void println(const char *) {}
  void printf(const char *, ...) {}
};
inline SerialStub Serial;
