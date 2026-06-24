#include <Arduino.h>

#include "RP_MultiHop.h"
#include "rtc.h"
#include "pb.h"
#include "pins_and_config.h"
#include "BuzzerTone.h"

// Battery
#include "RP_batteryVD.h"

// GPS
#include <TinyGPSPlus.h>

// ID device yang bisa diakses subsistem lain (di luar library jaringan)
uint32_t gDeviceId24 = 0;   // hash24
String   gDeviceIdHex6;     // "ABC123" (6 hex)

// ---------- Battery globals (dipakai payload) ----------
RP_batteryVD   gBatt;
RPBVD_Reading  gBattRd;

uint32_t gBattLastTickMs = 0;
int      gBattSoc = 0;      // % stabil untuk payload (0..100). Fallback 0 jika belum ada.
int      gBattVbat_mV = 0;  // opsional

// ---------- GPS globals (dipakai payload) ----------
static const int GPS_RX = 18;     // gpsRX=GPIO8
static const int GPS_TX = 8;    // gpsTX=GPIO18
static const uint32_t GPS_BAUD = 9600;

TinyGPSPlus gGps;
HardwareSerial GPS_SERIAL(1);

uint32_t gGpsLastPrintMs = 0;
bool     gGpsLocked = false;
float    gGpsLat = 0.0f;
float    gGpsLon = 0.0f;
float    gGpsMsl = 0.0f;     // meters

// Device Relay (pakai pins dari header)
MeshRelay device(AppCfg::PINS);

// =====================================================
//  Helper compat API (SFINAE) ada di header agar Arduino IDE tidak merusak template
// =====================================================
#include "CompatApi.h"

static uint8_t getNeighborCountSafe() {
  uint8_t n = tryNeighborCount1(device, 0);
  if (n == 0) n = tryNeighborCount2(device, 0);
  return n;
}

// ------------------ Battery ------------------
static void battBegin() {
  RPBVD_Config cfg;

  cfg.pin_adc   = 10;
  cfg.pin_enable= 11;
  cfg.enable_active_high = true;

  cfg.adc_resolution_bits = 12;
  cfg.adc_ref_mV          = 3300;

  cfg.divider_gain    = 2.0f;
  cfg.gain_corr       = 1.0f;
  cfg.offset_corr_mV  = 0;

  cfg.mode             = RPBVD_SocMode::LINEAR;
  cfg.vmin_display_mV  = 3000;
  cfg.vmax_display_mV  = 4000;

  cfg.ema_alpha_v    = 0.25f;
  cfg.ema_alpha_soc  = 0.20f;
  cfg.display_gamma  = 0.78f;
  cfg.hysteresis_pct = 0.4f;
  cfg.min_step_ms    = 4000;

  cfg.full_capacity_mAh = 22000;

  gBatt.begin(cfg);
  gBatt.firstMeasure(gBattRd);

  gBattVbat_mV = (int)gBattRd.vbat_mV;
  gBattSoc     = (int)gBattRd.soc_disp_int;

  Serial.printf("[BATT] INIT: Vbat=%dmV SoC=%d%% Rem~%.0fmAh\n",
                gBattRd.vbat_mV, gBattRd.soc_disp_int, gBattRd.remain_mAh);
}

static void battTick(uint32_t nowMs) {
  static uint32_t lastTickMs = 0;
  if (nowMs - lastTickMs < 1000) return;
  lastTickMs = nowMs;

  gBatt.sample(gBattRd);
  gBattVbat_mV = (int)gBattRd.vbat_mV;
  gBattSoc     = (int)gBattRd.soc_disp_int;
}

// ------------------ GPS ------------------
static void gpsBegin() {
  // ESP32-S3: UART1 bisa dipetakan bebas
  GPS_SERIAL.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.printf("[GPS] UART1 begin baud=%lu RX=%d TX=%d\n",
                (unsigned long)GPS_BAUD, GPS_RX, GPS_TX);
}

// definisi locked: lokasi valid + umur data < 2 detik (biar nggak pakai stale)
// (kalau modul belum output GGA/RMC stabil, tetap aman: payload akan 0)
static bool gpsIsLockedNow() {
  if (!gGps.location.isValid()) return false;
  const uint32_t age = gGps.location.age();
  if (age > 2000) return false;
  if (gGps.satellites.isValid() && gGps.satellites.value() == 0) return false;
  return true;
}


static bool gpsGetUnix(uint32_t &outUnix) {
  // butuh date + time valid
  if (!gGps.date.isValid() || !gGps.time.isValid()) return false;

  int y = (int)gGps.date.year();
  int m = (int)gGps.date.month();
  int d = (int)gGps.date.day();
  int hh = (int)gGps.time.hour();
  int mm = (int)gGps.time.minute();
  int ss = (int)gGps.time.second();

  if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31) return false;

  // Konversi kalender UTC -> epoch (detik sejak 1970-01-01)
  auto days_from_civil = [](int y, unsigned m, unsigned d) -> int64_t {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
  };

  int64_t days = days_from_civil(y, (unsigned)m, (unsigned)d);
  int64_t unixSec = days * 86400LL + (int64_t)hh * 3600 + (int64_t)mm * 60 + (int64_t)ss;

  if (unixSec <= 0) return false;

  outUnix = (uint32_t)unixSec;
  return true;
}

static void gpsTick(uint32_t nowMs) {
  while (GPS_SERIAL.available() > 0) {
    gGps.encode((char)GPS_SERIAL.read());
  }

  // Update globals kalau ada data baru
  if (gGps.location.isUpdated()) {
    gGpsLat = (float)gGps.location.lat();
    gGpsLon = (float)gGps.location.lng();
  }
  if (gGps.altitude.isUpdated()) {
    gGpsMsl = (float)gGps.altitude.meters();
  }

  // Edge-detect GPS lock/unlock agar buzzer tidak spam
  static bool     sPrevLock = false;
  static uint32_t sLastEdgeMs = 0;
  const bool lockedNow = gpsIsLockedNow();
  gGpsLocked = lockedNow;

  // Debounce 3000 ms untuk fluktuasi lock
  if (lockedNow != sPrevLock && (nowMs - sLastEdgeMs) > 3000) {
    sLastEdgeMs = nowMs;
    sPrevLock = lockedNow;
    if (lockedNow) BuzzerTone::toneGpsLocked();
    else          BuzzerTone::toneGpsLoss();
  }

  // debug ringan tiap 5 detik (boleh dihapus kalau ingin silent)
  if (nowMs - gGpsLastPrintMs >= 5000) {
    gGpsLastPrintMs = nowMs;
    // Serial.printf("[GPS] lock=%d lat=%.6f lon=%.6f msl=%.1f sat=%d hdop=%.1f\n",
    //               (int)gGpsLocked,
    //               gGpsLat, gGpsLon, gGpsMsl,
    //               gGps.satellites.isValid() ? (int)gGps.satellites.value() : -1,
    //               gGps.hdop.isValid() ? (gGps.hdop.hdop() / 100.0f) : -1.0f);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== START Firmware_Relay ===");

  BuzzerTone::begin();
  BuzzerTone::toneBoot();

  PB::begin(4, 5);

  battBegin();
  gpsBegin();

  device.setPayloadBuilder(AppCfg::buildRelayPayload);

  // Hook buzzer untuk TX + ACK (jika didukung library)
  trySetTxCallback(device, [](uint16_t seq, bool isRetry) {
    (void)seq;
    // Jika ingin bunyi hanya attempt pertama: aktifkan filter ini
    // if (isRetry) return;
    (void)isRetry;
    BuzzerTone::toneTx();
  }, 0);

  trySetAckCallback(device, [](uint16_t seq) {
    (void)seq;
    BuzzerTone::toneAck();
  }, 0);

  // Hook buzzer untuk paket masuk yang akan diteruskan (opsional)
  trySetForwardRxCallback(device, [](uint32_t fromId24, uint16_t seq) {
    (void)fromId24; (void)seq;
    BuzzerTone::toneForwardRx();
  }, 0);

  RtcTime::begin(AppCfg::PINS.rtcSda, AppCfg::PINS.rtcScl);

  Id::begin();
  gDeviceId24   = Id::id24();
  gDeviceIdHex6 = Id::idHex6();

  Serial.print("[BOOT] This ID = ");
  Serial.println(gDeviceIdHex6);

  NetRuntime::applyConfig(AppCfg::NET_CFG);
  NetRuntime::applyTuning(AppCfg::makeTuning());

  pinMode(AppCfg::PINS.lora.M0, OUTPUT);
  pinMode(AppCfg::PINS.lora.M1, OUTPUT);
  digitalWrite(AppCfg::PINS.lora.M0, LOW);
  digitalWrite(AppCfg::PINS.lora.M1, LOW);
  delay(50);

  pinMode(AppCfg::PINS.lora.AUX, INPUT);   // AUX harus input
  delay(50);

  Serial2.begin(9600, SERIAL_8N1, AppCfg::PINS.lora.RX, AppCfg::PINS.lora.TX);

  device.begin();
}

void loop() {
  const uint32_t nowMs = millis();

  PB::tick();
  battTick(nowMs);
  gpsTick(nowMs);

  // ---- RTC sync dari GPS ----
  // - cek 1x saat pertama kali GPS lock
  // - lalu cek ulang tiap 24 jam
  static bool     sDidFirstRtcCheck = false;
  static uint32_t sNextRtcCheckMs   = 0;

  uint32_t gpsUnix = 0;
  if (gGpsLocked && gpsGetUnix(gpsUnix)) {

    // // 1) cek pertama: segera ketika GPS pertama kali lock
    // if (!sDidFirstRtcCheck) {
    //   if (RtcTime::tickGpsSync(true, gpsUnix, 0)) {
    //     Serial.printf("[RTC] First sync by GPS: gps=%lu rtc_now=%lu\n",
    //                   (unsigned long)gpsUnix, (unsigned long)RtcTime::now());
    //   } else {
    //     Serial.println("[RTC] First check (no correction)");
    //   }

    //   sDidFirstRtcCheck = true;
    //   sNextRtcCheckMs   = nowMs + (24UL * 60UL * 60UL * 1000UL);
    // }

    // 2) cek berikutnya: tiap 24 jam
    if (sDidFirstRtcCheck && (int32_t)(nowMs - sNextRtcCheckMs) >= 0) {
      if (RtcTime::tickGpsSync(true, gpsUnix, 0)) {
        Serial.printf("[RTC] Daily sync by GPS: gps=%lu rtc_now=%lu\n",
                      (unsigned long)gpsUnix, (unsigned long)RtcTime::now());
      } else {
        Serial.println("[RTC] Daily check (no correction)");
      }

      sNextRtcCheckMs = nowMs + (24UL * 60UL * 60UL * 1000UL);
    }
  }

  // ---- NET PAIRED (heuristik): neighborCount 0 -> >0 ----
  static bool sPairedBeeped = false;
  static uint32_t sLastPairCheckMs = 0;
  if (!sPairedBeeped && (nowMs - sLastPairCheckMs) > 1000) {
    sLastPairCheckMs = nowMs;
    const uint8_t n = getNeighborCountSafe();
    if (n > 0) {
      sPairedBeeped = true;
      BuzzerTone::tonePairOk();
    }
  }

  device.loop();
}
