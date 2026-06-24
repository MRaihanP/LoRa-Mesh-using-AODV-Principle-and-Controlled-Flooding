#include "rtc.h"

#include <Wire.h>
#include <time.h>
#include "pins_and_config.h"   // PinsAndConfig::RTC::ADDR

bool     RtcTime::_begun             = false;
bool     RtcTime::_ok                = false;
uint32_t RtcTime::_fallbackStartUnix = 0;
uint32_t RtcTime::_fallbackStartMs   = 0;

// -----------------------------
// Helper BCD dan DS3231 low-level
// -----------------------------
static uint8_t toBCD(uint8_t v){ return (uint8_t)(((v/10)<<4) | (v%10)); }
static uint8_t fromBCD(uint8_t b){ return (uint8_t)(10*((b>>4)&0x0F) + (b&0x0F)); }

// Baca DS3231 sebagai UTC ke struct tm
static bool ds3231ReadUTC(struct tm& out){
  using RtcCfg = PinsAndConfig::RTC;

  Wire.beginTransmission(RtcCfg::ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(RtcCfg::ADDR, (uint8_t)7) != 7){
    return false;
  }

  uint8_t secB  = Wire.read();
  uint8_t minB  = Wire.read();
  uint8_t hrB   = Wire.read();
  uint8_t dowB  = Wire.read(); (void)dowB;
  uint8_t dateB = Wire.read();
  uint8_t monB  = Wire.read();
  uint8_t yrB   = Wire.read();

  struct tm t{};
  t.tm_sec  = fromBCD(secB & 0x7F);
  t.tm_min  = fromBCD(minB & 0x7F);
  t.tm_hour = fromBCD(hrB  & 0x3F);
  t.tm_mday = fromBCD(dateB & 0x3F);
  t.tm_mon  = fromBCD(monB & 0x1F) - 1;
  t.tm_year = fromBCD(yrB) + 100;   // 2000+
  t.tm_isdst= 0;

  out = t;
  return true;
}

// Tulis DS3231 dari struct tm (UTC)
static void ds3231WriteUTC(const struct tm& t){
  using RtcCfg = PinsAndConfig::RTC;

  Wire.beginTransmission(RtcCfg::ADDR);
  Wire.write(0x00); // mulai dari register detik

  uint8_t sec = toBCD((uint8_t)t.tm_sec) & 0x7F; // CH=0
  uint8_t min = toBCD((uint8_t)t.tm_min);
  uint8_t hour= toBCD((uint8_t)t.tm_hour);       // 24h
  uint8_t dow = (t.tm_wday == 0) ? 7 : t.tm_wday;
  uint8_t date= toBCD((uint8_t)t.tm_mday);
  uint8_t mon = toBCD((uint8_t)(t.tm_mon + 1)) & 0x1F;
  uint8_t yr  = toBCD((uint8_t)(t.tm_year - 100)); // sejak 2000

  Wire.write(sec);
  Wire.write(min);
  Wire.write(hour);
  Wire.write(dow);
  Wire.write(date);
  Wire.write(mon);
  Wire.write(yr);
  Wire.endTransmission();
}

// Konversi tm (UTC) → epoch (UTC).
static time_t tmToEpochUTC(struct tm& t){
  // Di ESP32, kalau TZ tidak di-set, mktime() treat tm sebagai local.
  // Kalau kamu perlakukan sistem sebagai UTC, biarkan TZ=UTC (default)
  return mktime(&t);
}

// -----------------------------
// RtcTime implementation
// -----------------------------
void RtcTime::begin() {
  if (_begun) return;
  _begun = true;

  // Asumsi: Wire.begin(SDA,SCL) SUDAH dipanggil di AppOS::begin()
  // Di sini hanya cek apakah RTC hidup, dan siapkan baseline fallback.
  struct tm t{};
  if (ds3231ReadUTC(t)){
    _ok = true;
    time_t epoch = tmToEpochUTC(t);
    if (epoch <= 0) epoch = 0;
    _fallbackStartUnix = (uint32_t)epoch;
    _fallbackStartMs   = millis();
  } else {
    _ok = false;
    _fallbackStartUnix = 0;
    _fallbackStartMs   = millis();
  }
}

uint32_t RtcTime::now() {
  // Kalau RTC OK, baca langsung DS3231 tiap kali.
  if (_ok){
    struct tm t{};
    if (ds3231ReadUTC(t)){
      time_t epoch = tmToEpochUTC(t);
      if (epoch > 0){
        return (uint32_t)epoch;
      }
    }
    // kalau baca gagal sesekali, jatuh ke fallback di bawah
  }

  // Fallback: pseudo-epoch berbasis millis()
  if (!_begun){
    return millis() / 1000;
  }
  uint32_t elapsedMs = millis() - _fallbackStartMs;
  return _fallbackStartUnix + (elapsedMs / 1000);
}

void RtcTime::setFromEpoch(uint32_t epoch){
  // Set DS3231 dari epoch UTC
  time_t e = (time_t)epoch;
  struct tm t{};
  gmtime_r(&e, &t);
  ds3231WriteUTC(t);

  _ok                = true;
  _fallbackStartUnix = epoch;
  _fallbackStartMs   = millis();
}

// Fungsi global untuk layer jaringan
uint32_t nowUnixLike() {
  return RtcTime::now();
}
