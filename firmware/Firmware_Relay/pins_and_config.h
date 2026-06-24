#pragma once
#include <Arduino.h>
#include "RP_MultiHop.h"

// ===== Runtime global (didefinisikan di RP_relay1.ino) =====
extern int gBattSoc;

// runtime globals dari RP_relay1.ino
extern int   gBattSoc;
extern bool  gGpsLocked;
extern float gGpsLat;
extern float gGpsLon;
extern float gGpsMsl;

// Semua config aplikasi Relay kita taruh di namespace AppCfg
namespace AppCfg {

  // ---------------- PIN RELAY ----------------
  static const BoardPins PINS = []() {
    BoardPins p;

    // LoRa (RELAY)
    p.lora.M0  = 6;
    p.lora.M1  = 7;
    p.lora.AUX = 17;
    p.lora.RX  = 16;
    p.lora.TX  = 15;

    // LED status
    p.statusLed = 1;

    // RTC I2C
    p.rtcSda = 39;
    p.rtcScl = 40;

    return p;
  }();

  // ---------------- NET CONFIG ----------------
  using NetRuntime::DeviceEntry;
  using NetRuntime::StaticRouteEntry;
  using NetRuntime::DataSlot;
  using NetRuntime::HelloSlot;
  using NetRuntime::FrameConfig;
  using NetRuntime::RoutingConfig;
  using NetRuntime::NetworkConfig;
  using NetRuntime::DeviceRole;

  // 1) Daftar device (ID & alias & role)
  static const DeviceEntry DEVICES[] = {
    { StaticRoute::ID_S,  "S",  DeviceRole::SINK  },
    { StaticRoute::ID_R1, "R1", DeviceRole::RELAY },
    { StaticRoute::ID_R2, "R2", DeviceRole::RELAY },
    { StaticRoute::ID_N1, "N1", DeviceRole::NODE  },
    { StaticRoute::ID_N2, "N2", DeviceRole::NODE  },
    { StaticRoute::ID_N3, "N3", DeviceRole::NODE  },
  };

  // 2) Pohon static: child -> parent
  static const StaticRouteEntry ROUTES[] = {
    { StaticRoute::ID_S,   0                  },
    { StaticRoute::ID_R1,  StaticRoute::ID_S  },
    { StaticRoute::ID_R2,  StaticRoute::ID_R1 },
    { StaticRoute::ID_N1,  StaticRoute::ID_R2 },
    { StaticRoute::ID_N2,  StaticRoute::ID_N1 },
    { StaticRoute::ID_N3,  StaticRoute::ID_N2 },
  };

  // 3) Data slot
  static const DataSlot DATA_SLOTS[] = {
    { 1, StaticRoute::ID_R1,  0,  4 },
    { 2, StaticRoute::ID_R2,  4, 10 },
    { 3, StaticRoute::ID_N1, 10, 20 },
    { 4, StaticRoute::ID_N2, 20, 30 },
    { 5, StaticRoute::ID_N3, 30, 40 },
  };

  // 4) HELLO slot
  static const HelloSlot HELLO_SLOTS[] = {
    { StaticRoute::ID_S,  5 },
    { StaticRoute::ID_R1, 4 },
    { StaticRoute::ID_R2, 3 },
    { StaticRoute::ID_N1, 2 },
    { StaticRoute::ID_N2, 1 },
    { StaticRoute::ID_N3, 0 },
  };

  // 5) Frame & routing config
  static const FrameConfig FRAME_CFG = {
    .frameLenSec       = 40,
    .helloPeriodFrames = 2,
    .helloSlotSec      = 2,
    .helloLenSec       = 20,
  };

  static const RoutingConfig ROUTE_CFG = {
    .dynamicRoutingEnabled = true,
    .rssiMinParent         = -100,
    .rssiBadParent         = -105,
    .rssiSwitchMarginDb    = 6,
  };

  // 6) NetworkConfig global
  static const NetworkConfig NET_CFG = {
    .devices          = DEVICES,
    .deviceCount      = sizeof(DEVICES) / sizeof(DEVICES[0]),
    .routes           = ROUTES,
    .routeCount       = sizeof(ROUTES) / sizeof(ROUTES[0]),
    .dataSlots        = DATA_SLOTS,
    .dataSlotCount    = sizeof(DATA_SLOTS) / sizeof(DATA_SLOTS[0]),
    .helloSlots       = HELLO_SLOTS,
    .helloSlotCount   = sizeof(HELLO_SLOTS) / sizeof(HELLO_SLOTS[0]),
    .frame            = FRAME_CFG,
    .routing          = ROUTE_CFG,
    .lastDataSlotId   = 5,
  };

  // ---------------- TUNING DEFAULT ----------------
  static inline NetRuntime::RuntimeTuning makeTuning() {
    NetRuntime::RuntimeTuning t;
    t.ackTimeoutMs     = 2500;
    t.maxRetry         = 1;
    t.txGapMs          = 50;
    t.forwardGuardMs   = 150;
    t.backlogTtlFrames = 10;
    t.backlogTailSec   = 4;

    t.lastDataSlotId   = 5;
    t.rxSensitivityDbm = -124.0f;

    // untuk Relay biasanya tidak print, tapi tetep set aja
    t.sinkPrintMode    = NetRuntime::SinkPrintMode::ANALYZED_ONLY;
    return t;
  }

  // ---------------- PAYLOAD RELAY ----------------
  static inline String buildRelayPayload(uint32_t unixNow, uint16_t seq) {
    (void)unixNow;
    (void)seq;

    float lat = 0.0f;
    float lon = 0.0f;
    float msl = 0.0f;

    if (gGpsLocked) {
      lat = gGpsLat;
      lon = gGpsLon;
      msl = gGpsMsl;
    }

    float tempC = 27.5f;
    float humid = 65.0f;
    int   sos   = 0;

    int batt = (gBattSoc >= 0) ? gBattSoc : 0;
    if (batt > 100) batt = 100;

    String p;
    p.reserve(72);
    p  = String(lat, 6);   p += ',';
    p += String(lon, 6);   p += ',';
    p += String(msl, 1);   p += ',';
    p += String(tempC, 1); p += ',';
    p += String(humid, 1); p += ',';
    p += String(sos);      p += ',';
    p += String(batt);
    return p;
  }
} // namespace AppCfg
