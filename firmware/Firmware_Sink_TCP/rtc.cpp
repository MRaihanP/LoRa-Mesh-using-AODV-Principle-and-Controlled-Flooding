#include "rtc.h"

bool       RtcTime::_begun             = false;
bool       RtcTime::_ok                = false;
RTC_DS3231 RtcTime::_rtc;
uint32_t   RtcTime::_fallbackStartUnix = 0;
uint32_t   RtcTime::_fallbackStartMs   = 0;

// Default policy: beda >= 2 detik, maksimal 12 jam sekali
uint32_t   RtcTime::_syncThresholdSec  = 2;
uint32_t   RtcTime::_syncIntervalMs    = 12UL * 3600UL * 1000UL;
uint32_t   RtcTime::_lastSyncMs        = 0;
bool       RtcTime::_everSynced        = false;

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

void RtcTime::setSyncPolicy(uint32_t thresholdSec, uint32_t intervalMs) {
  _syncThresholdSec = (thresholdSec == 0) ? 1 : thresholdSec;
  _syncIntervalMs   = intervalMs;
}

static uint32_t u32_absdiff(uint32_t a, uint32_t b) {
  return (a >= b) ? (a - b) : (b - a);
}

bool RtcTime::tickGpsSync(bool gpsLocked, uint32_t gpsUnix, uint32_t nowMs) {
  if (!_begun) return false;          // begin() harus dipanggil dulu
  if (!gpsLocked) return false;       // hanya sync saat GPS locked
  if (gpsUnix == 0) return false;     // abaikan data invalid

  // Cek beda waktu
  const uint32_t rtcUnix = RtcTime::now();
  const uint32_t diffSec = u32_absdiff(gpsUnix, rtcUnix);

  if (diffSec < _syncThresholdSec) return false;

  // Rate limit: pertama kali boleh langsung, berikutnya tunggu interval
  if (_everSynced) {
    const uint32_t elapsed = nowMs - _lastSyncMs; // aman overflow millis
    if (elapsed < _syncIntervalMs) return false;
  }

  // Koreksi RTC jika hardware RTC OK, kalau tidak OK update fallback baseline
  if (_ok) {
    _rtc.adjust(DateTime((uint32_t)gpsUnix));
  } else {
    // reset baseline fallback supaya now() ikut benar
    _fallbackStartUnix = gpsUnix;
    _fallbackStartMs   = millis();
  }

  _everSynced = true;
  _lastSyncMs = nowMs;
  return true;
}

// Implementasi fungsi global yang dipakai layer jaringan
uint32_t nowUnixLike() {
  return RtcTime::now();
}
