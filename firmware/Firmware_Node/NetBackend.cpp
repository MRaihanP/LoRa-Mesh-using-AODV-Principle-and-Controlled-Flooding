#include <Arduino.h>
#include "RP_MultiHop.h"
#include "NetBackend.h"
#include "pins_and_config.h"
#include "BuzzerTone.h"

using PinsAndConfig::NetLoRa;
using PinsAndConfig::Power;
using PinsAndConfig::NetMultiHop;

bool PinsAndConfig::Power::sosActive = false;
uint32_t PinsAndConfig::NetMultiHop::idHash24 = 0;
char     PinsAndConfig::NetMultiHop::idHex6[7] = "000000";
uint8_t PinsAndConfig::NetMultiHop::neighborCount = 0;

// ==========================
// 1) BoardPins persis node.ino
// ==========================
static BoardPins makeNodePins() {
    BoardPins p;

    // LoRa (NODE) – ambil dari PinsAndConfig::NetLoRa
    p.lora.M0  = NetLoRa::M0;
    p.lora.M1  = NetLoRa::M1;
    p.lora.AUX = NetLoRa::AUX;
    p.lora.RX  = NetLoRa::RX;
    p.lora.TX  = NetLoRa::TX;

    // LED status: pakai LED power global dari struct Power
    p.statusLed = Power::PIN_LED;

    return p;
}

static BoardPins pins = makeNodePins();
static MeshNode  device(pins);
static NetBackend::TelemetryHookFn gTelemetryHook = nullptr;
static void* gTelemetryHookUser = nullptr;

void NetBackend::setTelemetryHook(TelemetryHookFn fn, void* user){
  gTelemetryHook = fn;
  gTelemetryHookUser = user;
}

// ==========================
// 2) Payload dummy (copy dari node.ino)
// ==========================
String buildNodePayload(uint32_t unixNow, uint16_t seq) {
    (void)unixNow;

    // Bunyi saat akan kirim pesan (TX event)
    if (PinsAndConfig::NetMultiHop::neighborCount > 0) {
        BuzzerTone::beep(2100, 25);   // TX ping pendek
    }
    
    (void)seq;

    using namespace PinsAndConfig;

    if (gTelemetryHook){
        gTelemetryHook(millis(), gTelemetryHookUser);
    }

    // =========================
    // 1) Ambil snapshot GNSS
    // =========================
    float lat   = NAN;
    float lon   = NAN;
    float msl   = NAN;

    if (GNSS::hasFix && GNSS::gnssFixOK &&
        !isnan(GNSS::latDeg) && !isnan(GNSS::lonDeg))
    {
        lat = GNSS::latDeg;
        lon = GNSS::lonDeg;

        // Ketinggian: utamakan GNSS altMSL_m,
        // kalau NaN, fallback ke BMP280::altMSL_m kalau valid.
        if (BMP280::altValid && !isnan(BMP280::altMSL_m)) {
            msl = BMP280::altMSL_m;
        } else if (!isnan(GNSS::altMSL_m)) {
            msl = GNSS::altMSL_m;
        } else {
            msl = 0.0f;
        }
    } else {
        // Tidak ada fix GNSS → pakai 0 atau fallback BMP saja
        lat = 0.0f;
        lon = 0.0f;
        if (BMP280::altValid && !isnan(BMP280::altMSL_m)) {
            msl = BMP280::altMSL_m;
        } else {
            msl = 0.0f;
        }
    }

    // =========================
    // 2) Ambil snapshot SHT30
    // =========================
    float tempC = 0.0f;
    float humid = 0.0f;

    if (SHT30::tempValid && !isnan(SHT30::tempC)) {
        tempC = SHT30::tempC;
    } else {
        tempC = 0.0f;
    }

    if (SHT30::humValid && !isnan(SHT30::humPct)) {
        humid = SHT30::humPct;
    } else {
        humid = 0.0f;
    }

    // =========================
    // 3) Batt % (sementara dummy)
    // =========================
    int batt = PinsAndConfig::BatteryVD::soc_display_pct;
    if (batt < 0)   batt = 0;
    if (batt > 100) batt = 100;

    // =========================
    // 4) Build payload string
    // Format: lat|lon|msl|tempC|humid|sos|batt
    // Untuk sekarang sos = 0
    // =========================
    int sos = PinsAndConfig::Power::sosActive ? 1 : 0;

    String p;
    p.reserve(64);

    p  = String(lat,   6); p += ',';
    p += String(lon,   6); p += ',';
    p += String(msl,   1); p += ',';
    p += String(tempC, 1); p += ',';
    p += String(humid, 1); p += ',';
    p += String(sos);      p += ',';
    p += String(batt);

    return p;
}

// ==========================
// 3) BEGIN
// ==========================
void NetBackend::begin() {
  Serial.println();
  Serial.println("=== NetBackend BEGIN (Navigator) ===");

  // --- Device ID (class sekarang bernama Id di library) ---
  Id::begin();
  uint32_t id24 = Id::id24();
  String   hex6 = Id::idHex6();
  NetMultiHop::idHash24 = id24;

  // Pastikan selalu 6 hex + null terminator
  // (ambil dari string library, tapi disalin ke buffer statik)
  snprintf(NetMultiHop::idHex6, sizeof(NetMultiHop::idHex6), "%s", hex6.c_str());

  Serial.print("[NetBackend] This ID = ");
  Serial.println(hex6);

  // --- Payload builder & init device ---
  device.setPayloadBuilder(buildNodePayload);

  // >>> ADDED: buzzer ping hooks for TX + ACK
  device.setTxCallback([](uint16_t seq, bool isRetry) {
    (void)seq;
    if (PinsAndConfig::Power::sosActive) return;

    // Mau bunyi juga saat retry? kalau iya: hapus if ini
    // Kalau mau bunyi hanya attempt pertama: biarkan
    // if (isRetry) return;

    BuzzerTone::beep(2100, 25); // TX ping
  });

  device.setAckCallback([](uint16_t seq) {
    (void)seq;
    if (PinsAndConfig::Power::sosActive) return;

    BuzzerTone::beep(2600, 40); // ACK ping (confirmed)
  });

  device.begin();
  NetRuntime::applyConfig(NetMultiHop::NET);
}

// ==========================
// 6) TICK
// ==========================
void NetBackend::tick() {
    device.loop();

    int n = (int)device.neighborCount();
    if (n < 0) n = 0;
    if (n > 255) n = 255;

    // ===== Pair OK (mesh join heuristic): neighborCount naik dari 0 ke >0 =====
    static uint8_t prevN = 0;
    if (prevN == 0 && n > 0) {
        BuzzerTone::tonePairOk();
    }
    prevN = (uint8_t)n;

    NetMultiHop::neighborCount = (uint8_t)n;
}
