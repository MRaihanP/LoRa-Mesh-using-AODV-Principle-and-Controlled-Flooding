#include <Arduino.h>
#include "RP_MultiHop.h"
#include "rtc.h"
// Sink biasanya tidak pakai PB soft power, jadi tidak di-include pb.h di sini
// (kalau perlu, kamu bisa tambah sendiri)

// ID device yang bisa diakses subsistem lain (di luar library jaringan)
uint32_t gDeviceId24 = 0;   // hash24
String   gDeviceIdHex6;     // "ABC123" (6 hex)

// -----------------------------------------------------------------------------
// Mapping pin hardware untuk SINK
// -----------------------------------------------------------------------------
static BoardPins makeSinkPins() {
  BoardPins p;

  // LoRa (SINK)
  p.lora.M0  = 7;
  p.lora.M1  = 6;
  p.lora.AUX = 10;
  p.lora.RX  = 4;   // LoRa TX
  p.lora.TX  = 5;   // LoRa RX

  // LED status (kalau tidak ada LED, boleh diubah ke -1)
  p.statusLed = 0;

  // RTC I2C
  p.rtcSda = 8;
  p.rtcScl = 9;

  return p;
}

// -----------------------------------------------------------------------------
// Instansiasi device SINK
// -----------------------------------------------------------------------------
BoardPins pins = makeSinkPins();
MeshSink device(pins);

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
  { StaticRoute::ID_S,   0                 },
  { StaticRoute::ID_R1,  StaticRoute::ID_S },
  { StaticRoute::ID_R2,  StaticRoute::ID_R1},
  { StaticRoute::ID_N1,  StaticRoute::ID_R2},
  { StaticRoute::ID_N2,  StaticRoute::ID_N1},
  { StaticRoute::ID_N3,  StaticRoute::ID_N2},
};

// 3) Data slot (index 1..5)
static const DataSlot DATA_SLOTS[] = {
  // slotIndex, ownerId,          startSec, durationSec
  { 1, StaticRoute::ID_R1,  0,  4 },
  { 2, StaticRoute::ID_R2,  4, 10 },
  { 3, StaticRoute::ID_N1, 10, 20 },
  { 4, StaticRoute::ID_N2, 20, 30 },
  { 5, StaticRoute::ID_N3, 30, 40 },
};

// 4) HELLO slot
static const HelloSlot HELLO_SLOTS[] = {
  // id,                     slotIndex (0..5)
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

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== START MultiHop Sink ===");

  // RTC backend
  RtcTime::begin(pins.rtcSda, pins.rtcScl);

  // ID device
  Id::begin();
  gDeviceId24   = Id::id24();
  gDeviceIdHex6 = Id::idHex6();

  Serial.print("[BOOT] This ID = ");
  Serial.println(gDeviceIdHex6);

  device.begin();
  NetRuntime::applyConfig(NET_CFG);
}

void loop() {
  // Tidak ada PB di sink (kalau butuh, bisa ditambahkan sendiri)
  device.loop();
}
