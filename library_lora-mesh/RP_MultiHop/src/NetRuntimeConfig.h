#pragma once
#include <Arduino.h>

namespace NetRuntime {

// ID 24-bit diperlakukan sebagai uint32_t (mask 0xFFFFFF kalau perlu)
using Id24 = uint32_t;

// -----------------------------------------------------------------------------
// 0) Runtime tuning (di-set dari sketch)
// -----------------------------------------------------------------------------
enum class SinkPrintMode : uint8_t {
  RAW_ONLY,
  ANALYZED_ONLY
};

struct RuntimeTuning {
  // Reliability (disamakan untuk origin/forward/relay sesuai requirement)
  uint32_t ackTimeoutMs     = 2500;
  uint8_t  maxRetry         = 1;
  uint32_t txGapMs          = 50;
  uint32_t forwardGuardMs   = 150;

  // Backlog policy
  uint8_t  backlogTtlFrames = 10;
  uint8_t  backlogTailSec   = 4;

  // Sink analytics knobs
  uint8_t  lastDataSlotId   = 5;        // dipakai Sink utk flush summary saat slot terakhir
  float    rxSensitivityDbm = -124.0f;  // utk hitung linkMargin

  // Sink output mode
  SinkPrintMode sinkPrintMode = SinkPrintMode::ANALYZED_ONLY;
};

void applyTuning(const RuntimeTuning& t);
const RuntimeTuning& tuning();

// Accessor ringkas (biar call-site tidak pegang struct)
uint32_t ackTimeoutMs();
uint8_t  maxRetry();
uint32_t txGapMs();
uint32_t forwardGuardMs();

uint8_t  backlogTtlFrames();
uint8_t  backlogTailSec();

uint8_t  lastDataSlotId();
float    rxSensitivityDbm();

SinkPrintMode sinkPrintMode();

// -----------------------------------------------------------------------------
// 1) Device & role
// -----------------------------------------------------------------------------
enum class DeviceRole : uint8_t {
  NODE,
  RELAY,
  SINK
};

struct DeviceEntry {
  Id24        id24;    // ID 24-bit (hasil Id, dimasukkan dari sketch)
  const char* alias;   // "N1", "R1", "S", dll (string literal)
  DeviceRole  role;
};

// -----------------------------------------------------------------------------
// 2) Static routing tree (child -> parent)
// -----------------------------------------------------------------------------
struct StaticRouteEntry {
  Id24 child;
  Id24 parent;  // 0 = root
};

// -----------------------------------------------------------------------------
// 3) DATA slot per frame (index-based, waktu dalam detik)
// -----------------------------------------------------------------------------
struct DataSlot {
  uint8_t slotIndex;    // 1..N (IDX_SLOT_ID, LAST_DATA_SLOT_ID, dll)
  Id24    ownerId;      // device pemilik slot
  uint8_t startSec;     // detik mulai (inklusif) dalam frame
  uint8_t durationSec;  // durasi dalam detik
};

// -----------------------------------------------------------------------------
// 4) HELLO slot per frame HELLO
// -----------------------------------------------------------------------------
struct HelloSlot {
  Id24    id;          // device
  uint8_t slotIndex;   // posisi slot (0..N-1)
};

// -----------------------------------------------------------------------------
// 5) Shape frame & routing config
// -----------------------------------------------------------------------------
struct FrameConfig {
  uint32_t frameLenSec;        // DATA phase (default 40)
  uint32_t helloPeriodFrames;  // legacy, dipertahankan agar kompatibel
  uint8_t  helloSlotSec;       // durasi slot HELLO per device
  uint32_t helloLenSec;        // HELLO phase (default 20)
};

struct RoutingConfig {
  bool     dynamicRoutingEnabled;  // ON/OFF dynamic routing
  int16_t  rssiMinParent;          // threshold minimal parent
  int16_t  rssiBadParent;          // parent dianggap buruk
  int8_t   rssiSwitchMarginDb;     // hysteresis switching parent
};

// -----------------------------------------------------------------------------
// 6) NetworkConfig global
// -----------------------------------------------------------------------------
struct NetworkConfig {
  // Device list
  const DeviceEntry*      devices      = nullptr;
  size_t                  deviceCount  = 0;

  // Static tree
  const StaticRouteEntry* routes       = nullptr;
  size_t                  routeCount   = 0;

  // Data slots
  const DataSlot*         dataSlots    = nullptr;
  size_t                  dataSlotCount= 0;

  // Hello slots
  const HelloSlot*        helloSlots   = nullptr;
  size_t                  helloSlotCount = 0;

  // Frame & routing
  FrameConfig             frame        = {};
  RoutingConfig           routing      = {};

  // Catatan opsional: bisa diisi sama dengan NetTopology::LAST_DATA_SLOT_ID,
  // tapi library core tidak wajib memakainya (bisa dipakai Sink nanti).
  uint8_t                 lastDataSlotId = 0;
};

// -----------------------------------------------------------------------------
// 7) API konfigurasi
// -----------------------------------------------------------------------------
void applyConfig(const NetworkConfig& cfg);
const NetworkConfig& current();
bool hasConfig();

// -----------------------------------------------------------------------------
// 8) Helper lookup
// -----------------------------------------------------------------------------
const DeviceEntry*       findDeviceById(Id24 id);
const DeviceEntry*       findDeviceByAlias(const String& alias);
const StaticRouteEntry*  findRouteByChild(Id24 child);

// hitung hop statis ke Sink dari tree (child -> parent)
uint8_t staticHopToSink(Id24 id);

// DATA slot helper
uint8_t dataSlotIdFor(Id24 id);
bool    isInDataWindow(Id24 id, uint32_t unixNow, uint32_t& frameIdxOut);

// HELLO helper
bool    isHelloFrame(uint32_t frameIdx);
bool    isInHelloSlot(Id24 id, uint8_t posInFrameSec);

// Routing config
bool    isDynamicRoutingEnabled();
int16_t rssiMinParent();
int16_t rssiBadParent();
int8_t  rssiSwitchMarginDb();

// Alias helper
const char* aliasForId(Id24 id);
Id24        idForAlias(const String& alias);

void setSinkPosition(bool locked, double lat, double lon);
bool isSinkLocked();
float distanceFromSinkMeters(double lat, double lon); // 0 kalau belum lock / invalid

} // namespace NetRuntime
