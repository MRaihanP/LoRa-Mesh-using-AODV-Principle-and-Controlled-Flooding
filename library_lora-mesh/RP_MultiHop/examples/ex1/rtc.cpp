#include "rtc.h"

bool       RtcTime::_begun             = false;
bool       RtcTime::_ok                = false;
RTC_DS3231 RtcTime::_rtc;
uint32_t   RtcTime::_fallbackStartUnix = 0;
uint32_t   RtcTime::_fallbackStartMs   = 0;

void RtcTime::begin(int sda, int scl) {
  if (_begun) return;
  _begun = true;

  Wire.begin(sda, scl);

  _ok = _rtc.begin();
  if (!_ok) {
    // Fallback: gunakan millis() sebagai basis waktu pseudo-epoch.
    _fallbackStartUnix = 0;          // boleh diganti konstanta lain kalau mau
    _fallbackStartMs   = millis();
  }
}

uint32_t RtcTime::now() {
  if (_ok) {
    DateTime t = _rtc.now();
    return (uint32_t)t.unixtime();
  }

  // Fallback tanpa RTC / RTC gagal: kembalikan waktu pseudo berbasis millis.
  if (!_begun) {
    // Kalau begin() belum pernah dipanggil, minimal tetap berikan sesuatu.
    return millis() / 1000;
  }

  uint32_t elapsedMs = millis() - _fallbackStartMs;
  return _fallbackStartUnix + (elapsedMs / 1000);
}

// Implementasi fungsi global yang dipakai layer jaringan
uint32_t nowUnixLike() {
  return RtcTime::now();
}
