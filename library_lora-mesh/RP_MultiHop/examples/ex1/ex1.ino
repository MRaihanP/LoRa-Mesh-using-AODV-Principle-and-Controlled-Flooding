#include <Arduino.h>
#include "RP_MultiHop.h"
#include "rtc.h"
#include "pb.h"

// Pilih peran di sini (tepat satu saja yang aktif)
// #define ROLE_NODE 
// #define ROLE_RELAY
#define ROLE_SINK 

// ID device yang bisa diakses subsistem lain (di luar library jaringan)
uint32_t gDeviceId24 = 0;   // hash24
String   gDeviceIdHex6;     // "ABC123" (6 hex)

// -----------------------------------------------------------------------------
// Mapping pin hardware DIPINDAHKAN ke sketch (bukan lagi di library).
// Tiga helper ini adalah pengganti BoardPins::makeNodePins/makeRelayPins/makeSinkPins,
// tapi sekarang hidup di .ino, sehingga library core bersih dari konfigurasi board.
// -----------------------------------------------------------------------------
static BoardPins makeNodePins() {
  BoardPins p;

  // LoRa (NODE)
  p.lora.M0  = 4;
  p.lora.M1  = 5;
  p.lora.AUX = 15;
  p.lora.RX  = 7;
  p.lora.TX  = 6;

  // LED status
  p.statusLed = 2;

  // RTC I2C
  p.rtcSda = 39;
  p.rtcScl = 40;

  return p;
}

static BoardPins makeRelayPins() {
  BoardPins p;

  // LoRa (RELAY)
  p.lora.M0  = 6;
  p.lora.M1  = 7;
  p.lora.AUX = 17;
  p.lora.RX  = 16;
  p.lora.TX  = 15;

  // LED status
  p.statusLed = 2;

  // RTC I2C
  p.rtcSda = 39;
  p.rtcScl = 40;

  return p;
}

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

#if defined(ROLE_NODE)
BoardPins pins = makeNodePins();
MeshNode  device(pins);
#elif defined(ROLE_RELAY)
BoardPins pins = makeRelayPins();
MeshRelay device(pins);
#elif defined(ROLE_SINK)
BoardPins pins = makeSinkPins();
MeshSink  device(pins);
#else
#error "Define salah satu: ROLE_NODE / ROLE_RELAY / ROLE_SINK"
#endif

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

// 2) Pohon static: child -> parent (sama dengan ROUTES lama)
static const StaticRouteEntry ROUTES[] = {
  { StaticRoute::ID_S,   0                 },
  { StaticRoute::ID_R1,  StaticRoute::ID_S },
  { StaticRoute::ID_R2,  StaticRoute::ID_R1},
  { StaticRoute::ID_N1,  StaticRoute::ID_R2},
  { StaticRoute::ID_N2,  StaticRoute::ID_N1},
  { StaticRoute::ID_N3,  StaticRoute::ID_N2},
};

// 3) Data slot (index 1..5, detik sama seperti fallback di SlotPlan)
static const DataSlot DATA_SLOTS[] = {
  // slotIndex, ownerId,          startSec, durationSec
  { 1, StaticRoute::ID_R1,  0,  4 },  // N1  : 0–10
  { 2, StaticRoute::ID_R2,  4, 10 },  // N3  : 10–20
  { 3, StaticRoute::ID_N1, 10, 20 },  // N2  : 20–30
  { 4, StaticRoute::ID_N2, 20, 30 },  // R2  : 30–36
  { 5, StaticRoute::ID_N3, 30, 40 },  // R1  : 36–40
};

// 4) HELLO slot (urutan sama dengan HELLO_IDS lama)
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
  .frameLenSec       = 40,  // sama dengan FRAME_LEN_SEC fallback
  .helloPeriodFrames = 2,   // DATA, HELLO, DATA, HELLO, ...
  .helloSlotSec      = 2,   // sama dengan HELLO_SLOT_SEC fallback
};

// Nilai ini sementara bisa dicopy dari konstanta di Node/Relay (nanti kita wiring ke sini)
static const RoutingConfig ROUTE_CFG = {
  .dynamicRoutingEnabled = true,   // untuk saat ini, tetap ON
  .rssiMinParent         = -100,   // sama seperti RSSI_MIN_PARENT di Relay/Node
  .rssiBadParent         = -105,   // sama seperti RSSI_BAD_PARENT
  .rssiSwitchMarginDb    = 6,      // sama seperti RSSI_SWITCH_MARGIN_DB
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
  .lastDataSlotId   = 5,   // sama dengan NetTopology::LAST_DATA_SLOT_ID sekarang
};

String buildNodePayload(uint32_t unixNow, uint16_t seq) {
  // Di sini kamu bebas: baca GPS, sensor, batt%, dll
  // Contoh sederhana: masih pakai format lama

  float lat   = -6.7f;
  float lon   = 110.0f;
  float msl   = 150.0f;
  float tempC = 27.5f;
  float humid = 65.0f;
  int   sos   = 0;
  int   batt  = 87;

  String p;
  p.reserve(64);
  p  = String(lat, 6);   p += '|';
  p += String(lon, 6);   p += '|';
  p += String(msl, 1);   p += '|';
  p += String(tempC, 1); p += '|';
  p += String(humid, 1); p += '|';
  p += String(sos);      p += '|';
  p += String(batt);
  return p;
}

String buildRelayPayload(uint32_t unixNow, uint16_t seq) {
  float lat  = -6.8f;
  float lon  = 110.05f;
  int   batt = 95;

  String p;
  p.reserve(32);
  p  = String(lat, 6); p += '|';
  p += String(lon, 6); p += '|';
  p += String(batt);
  return p;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== START MultiHop Test ===");

  // --- Soft power untuk Node / Relay ---
#if defined(ROLE_NODE)
  PB::begin(16, 17);   // MY_POWER=16, Switch=17
  device.setPayloadBuilder(buildNodePayload);
#elif defined(ROLE_RELAY)
  PB::begin(4, 5);     // MY_POWER=4,  Switch=5
  device.setPayloadBuilder(buildRelayPayload);
#endif

  // RTC backend: tetap di level sketch, konfigurasi pakai pins.rtcSda/rtcScl
  RtcTime::begin(pins.rtcSda, pins.rtcScl);

  // ID device (hash24) → simpan ke variabel global untuk dipakai subsistem lain
  Id::begin();
  gDeviceId24  = Id::id24();
  gDeviceIdHex6 = Id::idHex6();

  Serial.print("[BOOT] This ID = ");
  Serial.println(gDeviceIdHex6);

  device.begin();
  NetRuntime::applyConfig(NET_CFG);
}

void loop() {
#if defined(ROLE_NODE) || defined(ROLE_RELAY)
  PB::tick();
#endif
  device.loop();
}
