#pragma once
#include <Arduino.h>
#include "pins_and_config.h"
#include "rtc.h"

// ----------------------------------------------------------------------------
// TimeSync: koreksi RTC DS3231 berbasis GNSS time (UTC)
// Kebijakan:
// - Koreksi hanya jika |GNSS - RTC| >= 2 detik
// - Koreksi saat boot (sekali) begitu GNSS time valid + fix OK
// - Koreksi periodik tiap 12 jam jika masih selisih >= 2 detik
// ----------------------------------------------------------------------------
class TimeSync {
public:
  static void begin() {
    _begun = true;
    _bootSynced = false;
    _lastSyncEpoch = 0;
    _lastAttemptMs = 0;
  }

  static void tick(uint32_t nowMs) {
    if (!_begun) begin();

    using G = PinsAndConfig::GNSS;

    // GNSS time harus valid + fresh
    if (!G::timeValid) return;
    if (G::lastTimeMs == 0) return;
    if (nowMs - G::lastTimeMs > 2000) return; // stale

    // Sync hanya kalau GNSS fix OK (time benar dari sat)
    if (!G::gnssFixOK || !G::hasFix) return;

    const uint32_t gnssEpoch = G::epochUtc;
    const uint32_t rtcEpoch  = RtcTime::now();

    int32_t diff = (int32_t)gnssEpoch - (int32_t)rtcEpoch;
    if (diff < 0) diff = -diff;

    if (diff < 2) return; // sudah cukup dekat

    const bool needBootSync = !_bootSynced;
    const bool needPeriodic = (_lastSyncEpoch == 0) || ((gnssEpoch >= _lastSyncEpoch) && (gnssEpoch - _lastSyncEpoch >= 12UL * 3600UL));
    if (!needBootSync && !needPeriodic) return;

    // throttle supaya tidak spam set time
    if (nowMs - _lastAttemptMs < 3000) return;
    _lastAttemptMs = nowMs;

    RtcTime::setFromEpoch(gnssEpoch);
    _lastSyncEpoch = gnssEpoch;
    _bootSynced = true;
  }

private:
  static bool     _begun;
  static bool     _bootSynced;
  static uint32_t _lastSyncEpoch;
  static uint32_t _lastAttemptMs;
};

inline bool     TimeSync::_begun = false;
inline bool     TimeSync::_bootSynced = false;
inline uint32_t TimeSync::_lastSyncEpoch = 0;
inline uint32_t TimeSync::_lastAttemptMs = 0;
