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

  // ---------------------------
  // GPS -> RTC correction API
  // ---------------------------

  // Opsional: ubah policy default (threshold=2s, interval=12 jam)
  static void setSyncPolicy(uint32_t thresholdSec, uint32_t intervalMs);

  // Panggil rutin di loop() setelah kamu update/parse GPS.
  // - gpsLocked: true jika fix valid (GPS locked)
  // - gpsUnix : waktu GPS dalam unix detik (UTC)
  // - nowMs   : millis() saat ini
  // Return true jika RTC barusan dikoreksi.
  static bool tickGpsSync(bool gpsLocked, uint32_t gpsUnix, uint32_t nowMs);

private:
  static bool       _begun;
  static bool       _ok;
  static RTC_DS3231 _rtc;

  // Untuk fallback ketika RTC gagal / tidak ada
  static uint32_t   _fallbackStartUnix;
  static uint32_t   _fallbackStartMs;

  // Sync policy/state
  static uint32_t   _syncThresholdSec;   // default 2
  static uint32_t   _syncIntervalMs;     // default 12 jam
  static uint32_t   _lastSyncMs;
  static bool       _everSynced;
};

// Implementasi fungsi global yang dipakai layer jaringan
uint32_t nowUnixLike();
