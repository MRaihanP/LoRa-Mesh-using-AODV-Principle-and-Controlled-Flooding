#pragma once
#include <Arduino.h>
#include "RP_MultiHop.h"

// Semua pin + config jaringan dipusatkan di sini
namespace AppCfg {

  // GPS UART
  static constexpr int GPS_RX = 10;   // MCU RX  <- GPS TX
  static constexpr int GPS_TX = 9;    // MCU TX  -> GPS RX
  static constexpr uint32_t GPS_BAUD = 9600;   // Ublox Neo-6M

  // RS485 UART
  // Waveshare ESP32-S3-Zero: TX=GPIO43, RX=GPIO44 (default UART0)
  static constexpr int RS485_TX = 43;     // MCU TX -> DI MAX485
  static constexpr int RS485_RX = 44;     // MCU RX <- RO MAX485
  static constexpr int RS485_DE_RE = 13;  // arah TX/RX MAX485
  static constexpr uint32_t RS485_BAUD = 115200;

  // ---------------------------------------------------------------------------
  // PIN MAPPING SINK (Waveshare ESP32-S3-Zero + E220 + DS3231)
  // ---------------------------------------------------------------------------
  static inline BoardPins makeSinkPins() {
    BoardPins p;

    // LoRa E220 (UART2 internal library)
    p.lora.M0  = 1;   // GPIO2
    p.lora.M1  = 2;   // GPIO1
    p.lora.AUX = 5;  // GPIO5
    p.lora.RX  = 4;   // MCU RX  <- E220 TX
    p.lora.TX  = 3;   // MCU TX  -> E220 RX

    // Status LED bawaan library (tidak dipakai untuk RGB; boleh tetap -1 kalau kamu mau)
    // Tapi Base::begin() memanggil pinMode(statusLed, OUTPUT), jadi jangan -1.
    // Pakai pin dummy yang aman kalau tidak digunakan (mis. GPIO47 di beberapa board),
    // namun agar portable: kita set ke GPIO21 juga (tidak masalah karena kita kontrol sendiri).
    p.statusLed = 21;

    // RTC I2C (DS3231)
    p.rtcSda = 7;
    p.rtcScl = 8;

    return p;
  }

  static const BoardPins PINS = makeSinkPins();

  // ---------------------------------------------------------------------------
  // RTC sync policy (tetap, bisa disesuaikan)
  // ---------------------------------------------------------------------------
  static constexpr int32_t  RTC_SYNC_THRESHOLD_SEC = 2;
  static constexpr uint32_t RTC_SYNC_INTERVAL_MS   = 24UL * 3600UL * 1000UL;

  // ---------------------------------------------------------------------------
  // NETWORK CONFIG (copy dari Firmware_Sink lama: DEVICES/ROUTES/SLOTS/FRAME/ROUTING)
  // ---------------------------------------------------------------------------
  using NetRuntime::DeviceEntry;
  using NetRuntime::StaticRouteEntry;
  using NetRuntime::DataSlot;
  using NetRuntime::HelloSlot;
  using NetRuntime::FrameConfig;
  using NetRuntime::RoutingConfig;
  using NetRuntime::NetworkConfig;
  using NetRuntime::DeviceRole;

  static const DeviceEntry DEVICES[] = {
    { StaticRoute::ID_S,  "S",  DeviceRole::SINK  },
    { StaticRoute::ID_R1, "R1", DeviceRole::RELAY },
    { StaticRoute::ID_R2, "R2", DeviceRole::RELAY },
    { StaticRoute::ID_N1, "N1", DeviceRole::NODE  },
    { StaticRoute::ID_N2, "N2", DeviceRole::NODE  },
    { StaticRoute::ID_N3, "N3", DeviceRole::NODE  },
  };

  static const StaticRouteEntry ROUTES[] = {
    { StaticRoute::ID_S,   0                  },
    { StaticRoute::ID_R1,  StaticRoute::ID_S  },
    { StaticRoute::ID_R2,  StaticRoute::ID_R1 },
    { StaticRoute::ID_N1,  StaticRoute::ID_R2 },
    { StaticRoute::ID_N2,  StaticRoute::ID_N1 },
    { StaticRoute::ID_N3,  StaticRoute::ID_N2 },
  };

  static const DataSlot DATA_SLOTS[] = {
    { 1, StaticRoute::ID_R1,  0,  4 },
    { 2, StaticRoute::ID_R2,  4, 10 },
    { 3, StaticRoute::ID_N1, 10, 20 },
    { 4, StaticRoute::ID_N2, 20, 30 },
    { 5, StaticRoute::ID_N3, 30, 40 },
  };

  static const HelloSlot HELLO_SLOTS[] = {
    { StaticRoute::ID_S,  5 },
    { StaticRoute::ID_R1, 4 },
    { StaticRoute::ID_R2, 3 },
    { StaticRoute::ID_N1, 2 },
    { StaticRoute::ID_N2, 1 },
    { StaticRoute::ID_N3, 0 },
  };

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

  static const NetworkConfig NET_CFG = {
    .devices        = DEVICES,
    .deviceCount    = sizeof(DEVICES) / sizeof(DEVICES[0]),
    .routes         = ROUTES,
    .routeCount     = sizeof(ROUTES) / sizeof(ROUTES[0]),
    .dataSlots      = DATA_SLOTS,
    .dataSlotCount  = sizeof(DATA_SLOTS) / sizeof(DATA_SLOTS[0]),
    .helloSlots     = HELLO_SLOTS,
    .helloSlotCount = sizeof(HELLO_SLOTS) / sizeof(HELLO_SLOTS[0]),
    .frame          = FRAME_CFG,
    .routing        = ROUTE_CFG,
    .lastDataSlotId = 5,
  };

} // namespace AppCfg