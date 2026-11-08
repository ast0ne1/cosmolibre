#pragma once
#include <stddef.h>
#include <stdint.h>

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  size_t print(const char *s) {
    size_t n = 0;
    while (s && *s) n += write((uint8_t)*s++);
    return n;
  }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(const class __FlashStringHelper *s) {
    return print(reinterpret_cast<const char *>(s));
  }
};
