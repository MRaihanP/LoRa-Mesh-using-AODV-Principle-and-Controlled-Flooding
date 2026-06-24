#pragma once
#include <Arduino.h>

// Backend waktu global berbasis DS3231 (UTC)
// - Dipakai jaringan (nowUnixLike)
// - Dipakai UI (Page_ClockDS3231) untuk baca/tulis RTC
class RtcTime {
public:
  // Panggil sekali di setup (level sketch / backend) setelah OS/AppOS
  // memanggil Wire.begin(...) dengan SDA/SCL dari PinsAndConfig.
  static void begin();

  // Unix time (detik sejak 1970-01-01 00:00:00 UTC).
  // Jika RTC gagal, fallback ke pseudo-epoch berbasis millis()
  // supaya jaringan tetap punya waktu yang monoton naik.
  static uint32_t now();

  // Set DS3231 dari epoch UTC.
  // Dipakai ketika sync dari NTP di Page_ClockDS3231.
  static void setFromEpoch(uint32_t epoch);

private:
  static bool     _begun;
  static bool     _ok;
  static uint32_t _fallbackStartUnix;
  static uint32_t _fallbackStartMs;
};

// Fungsi global untuk layer jaringan (RP_MultiHop)
uint32_t nowUnixLike();
