#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>   // Library "RTClib" (Adafruit)

class RtcTime {
public:
  // Panggil sekali di setup() (level sketch), sebelum jaringan dipakai.
  static void begin(int sda, int scl);

  // Unix time (detik). Jika RTC gagal, fallback ke pseudo-millis.
  static uint32_t now();

private:
  static bool       _begun;
  static bool       _ok;
  static RTC_DS3231 _rtc;

  // Untuk fallback ketika RTC gagal / tidak ada
  static uint32_t   _fallbackStartUnix;
  static uint32_t   _fallbackStartMs;
};
