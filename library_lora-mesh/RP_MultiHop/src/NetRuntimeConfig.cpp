#include "NetRuntimeConfig.h"

namespace NetRuntime {

static NetworkConfig gCfg;
static bool          gHasConfig = false;

static RuntimeTuning  gTune;  // default-initialized

// -----------------------------------------------------------------------------
// 0) Runtime tuning
// -----------------------------------------------------------------------------
void applyTuning(const RuntimeTuning& t) {
  gTune = t; // copy
}

const RuntimeTuning& tuning() {
  return gTune;
}

uint32_t ackTimeoutMs()     { return gTune.ackTimeoutMs; }
uint8_t  maxRetry()         { return gTune.maxRetry; }
uint32_t txGapMs()          { return gTune.txGapMs; }
uint32_t forwardGuardMs()   { return gTune.forwardGuardMs; }

uint8_t  backlogTtlFrames() { return gTune.backlogTtlFrames; }
uint8_t  backlogTailSec()   { return gTune.backlogTailSec; }

uint8_t  lastDataSlotId()   { return gTune.lastDataSlotId; }
float    rxSensitivityDbm() { return gTune.rxSensitivityDbm; }

SinkPrintMode sinkPrintMode() { return gTune.sinkPrintMode; }

// -----------------------------------------------------------------------------
// 1) applyConfig / current / hasConfig
// -----------------------------------------------------------------------------
void applyConfig(const NetworkConfig& cfg) {
  gCfg       = cfg;      // shallow copy (pointer + count)
  gHasConfig = (cfg.deviceCount > 0);
}

const NetworkConfig& current() {
  return gCfg;
}

bool hasConfig() {
  return gHasConfig;
}

// -----------------------------------------------------------------------------
// 2) Device & route lookup
// -----------------------------------------------------------------------------
const DeviceEntry* findDeviceById(Id24 id) {
  if (!gCfg.devices || gCfg.deviceCount == 0) return nullptr;
  for (size_t i = 0; i < gCfg.deviceCount; ++i) {
    if (gCfg.devices[i].id24 == id) {
      return &gCfg.devices[i];
    }
  }
  return nullptr;
}

const DeviceEntry* findDeviceByAlias(const String& alias) {
  if (!gCfg.devices || gCfg.deviceCount == 0) return nullptr;
  for (size_t i = 0; i < gCfg.deviceCount; ++i) {
    const char* a = gCfg.devices[i].alias;
    if (!a) continue;
    if (alias.equalsIgnoreCase(a)) {
      return &gCfg.devices[i];
    }
  }
  return nullptr;
}

const StaticRouteEntry* findRouteByChild(Id24 child) {
  if (!gCfg.routes || gCfg.routeCount == 0) return nullptr;
  for (size_t i = 0; i < gCfg.routeCount; ++i) {
    if (gCfg.routes[i].child == child) {
      return &gCfg.routes[i];
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// 3) Hop statis ke Sink dari tree (child -> parent)
// -----------------------------------------------------------------------------
uint8_t staticHopToSink(Id24 id) {
  if (!gCfg.routes || gCfg.routeCount == 0) {
    return 255;  // tidak ada tree runtime
  }

  if (id == 0) return 0;

  uint8_t hops = 0;
  Id24    cur  = id;

  // Hindari loop dengan batas hops max
  const uint8_t MAX_HOPS = 32;

  while (true) {
    const StaticRouteEntry* r = findRouteByChild(cur);
    if (!r) {
      // tidak tahu parent; kalau ini Sink (parent=0) boleh dianggap 0
      return (hops == 0) ? 0 : 255;
    }
    if (r->parent == 0) {
      // Root/Sink
      return hops;
    }
    cur = r->parent;
    hops++;
    if (hops >= MAX_HOPS) {
      // kemungkinan loop
      return 255;
    }
  }
}

// -----------------------------------------------------------------------------
// 4) DATA slot helper
// -----------------------------------------------------------------------------
uint8_t dataSlotIdFor(Id24 id) {
  if (!gCfg.dataSlots || gCfg.dataSlotCount == 0) return 0;
  for (size_t i = 0; i < gCfg.dataSlotCount; ++i) {
    if (gCfg.dataSlots[i].ownerId == id) {
      return gCfg.dataSlots[i].slotIndex;
    }
  }
  return 0;
}

bool isInDataWindow(Id24 id, uint32_t unixNow, uint32_t& frameIdxOut) {
  uint32_t dataLen = gCfg.frame.frameLenSec;
  if (dataLen == 0 || !gCfg.dataSlots || gCfg.dataSlotCount == 0) {
    frameIdxOut = 0;
    return false;
  }

  uint32_t helloLen = gCfg.frame.helloLenSec;
  if (helloLen == 0) helloLen = 20;

  uint32_t cycleLen = dataLen + helloLen;
  if (cycleLen == 0) {
    frameIdxOut = 0;
    return false;
  }

  frameIdxOut = unixNow / cycleLen;
  uint32_t posInCycle = unixNow % cycleLen;

  // DATA phase hanya 0..dataLen-1
  if (posInCycle >= dataLen) {
    return false;
  }

  uint32_t posInData = posInCycle;

  for (size_t i = 0; i < gCfg.dataSlotCount; ++i) {
    const DataSlot& ds = gCfg.dataSlots[i];
    if (ds.ownerId != id) continue;

    uint32_t start = ds.startSec;
    uint32_t end   = ds.startSec + ds.durationSec;
    if (posInData >= start && posInData < end) {
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// 5) HELLO helper
// -----------------------------------------------------------------------------
bool isHelloFrame(uint32_t frameIdx) {
  (void)frameIdx;
  return false;
}

static inline void getSuperframeTime(uint32_t unixNow,
                                     uint32_t& superIdx,
                                     uint32_t& posInCycle,
                                     uint32_t& dataLen,
                                     uint32_t& helloLen) {
  dataLen  = gCfg.frame.frameLenSec;
  helloLen = gCfg.frame.helloLenSec;
  if (dataLen == 0) dataLen = 40;
  if (helloLen == 0) helloLen = 20;

  uint32_t cycleLen = dataLen + helloLen;
  if (cycleLen == 0) {
    superIdx = 0;
    posInCycle = 0;
    return;
  }

  superIdx = unixNow / cycleLen;
  posInCycle = unixNow % cycleLen;
}

bool isHelloPhase(uint32_t unixNow) {
  uint32_t superIdx, pos, d, h;
  getSuperframeTime(unixNow, superIdx, pos, d, h);
  (void)superIdx;
  return (pos >= d);
}

uint32_t posInHelloSec(uint32_t unixNow) {
  uint32_t superIdx, pos, d, h;
  getSuperframeTime(unixNow, superIdx, pos, d, h);
  (void)superIdx;
  if (pos < d) return 0;
  uint32_t ph = pos - d;     // 0..helloLen-1
  if (ph >= h) ph = h ? (h - 1) : 0;
  return ph;
}

bool isInHelloSlotSuperframe(Id24 id, uint32_t posHelloSec) {
  if (!gCfg.helloSlots || gCfg.helloSlotCount == 0) return false;

  uint32_t helloLen = gCfg.frame.helloLenSec;
  if (helloLen == 0) helloLen = 20;
  if (posHelloSec >= helloLen) return false;

  uint8_t slotSec = gCfg.frame.helloSlotSec;
  if (slotSec == 0) return false;

  // total detik yang benar-benar dipakai untuk slot hello di dalam hello phase
  uint32_t usedSec = (uint32_t)slotSec * (uint32_t)gCfg.helloSlotCount;
  if (usedSec > helloLen) usedSec = helloLen;
  if (posHelloSec >= usedSec) return false;

  // Cari slotIndex milik id
  int16_t slotIndex = -1;
  for (size_t i = 0; i < gCfg.helloSlotCount; ++i) {
    if (gCfg.helloSlots[i].id == id) {
      slotIndex = (int16_t)gCfg.helloSlots[i].slotIndex;
      break;
    }
  }
  if (slotIndex < 0) return false;

  uint32_t myStart = (uint32_t)slotIndex * (uint32_t)slotSec;
  uint32_t myEnd   = myStart + (uint32_t)slotSec;
  return (posHelloSec >= myStart && posHelloSec < myEnd);
}

bool isInHelloSlot(Id24 id, uint8_t posInHelloSec) {
  if (!gCfg.helloSlots || gCfg.helloSlotCount == 0) return false;

  uint32_t helloLen = gCfg.frame.helloLenSec;
  if (helloLen == 0) helloLen = 20;
  if (posInHelloSec >= helloLen) return false;

  uint8_t slotSec = gCfg.frame.helloSlotSec;
  if (slotSec == 0) return false;

  uint32_t usedSec = (uint32_t)slotSec * (uint32_t)gCfg.helloSlotCount;
  if (usedSec > helloLen) usedSec = helloLen;
  if (posInHelloSec >= usedSec) {
    return false;
  }

  int16_t slotIndex = -1;
  for (size_t i = 0; i < gCfg.helloSlotCount; ++i) {
    if (gCfg.helloSlots[i].id == id) {
      slotIndex = (int16_t)gCfg.helloSlots[i].slotIndex;
      break;
    }
  }
  if (slotIndex < 0) return false;

  uint8_t myStart = (uint8_t)(slotIndex * slotSec);
  uint8_t myEnd   = (uint8_t)(myStart + slotSec);
  return (posInHelloSec >= myStart && posInHelloSec < myEnd);
}

// -----------------------------------------------------------------------------
// 6) Routing config
// -----------------------------------------------------------------------------
bool isDynamicRoutingEnabled()  {return gCfg.routing.dynamicRoutingEnabled;}
int16_t rssiMinParent()         { return gCfg.routing.rssiMinParent; }
int16_t rssiBadParent()         { return gCfg.routing.rssiBadParent; }
int8_t rssiSwitchMarginDb()     { return gCfg.routing.rssiSwitchMarginDb; }

// -----------------------------------------------------------------------------
// 7) Alias helper
// -----------------------------------------------------------------------------
const char* aliasForId(Id24 id) {
  const DeviceEntry* d = findDeviceById(id);
  if (d && d->alias) return d->alias;
  return nullptr;
}

Id24 idForAlias(const String& alias) {
  const DeviceEntry* d = findDeviceByAlias(alias);
  if (d) return d->id24;
  return 0;
}

// -----------------------------------------------------------------------------
// 8) Distance Calculator
// -----------------------------------------------------------------------------

static bool   s_sinkLocked = false;
static double s_sinkLat = 0.0;
static double s_sinkLon = 0.0;

static inline double deg2rad(double d) { return d * 0.017453292519943295; }

// Haversine, output meter
static float haversine_m(double lat1, double lon1, double lat2, double lon2) {
const double R = 6371000.0; // meter
const double p1 = deg2rad(lat1);
const double p2 = deg2rad(lat2);
const double dP = deg2rad(lat2 - lat1);
const double dL = deg2rad(lon2 - lon1);

const double a = sin(dP * 0.5) * sin(dP * 0.5) +
                    cos(p1) * cos(p2) * sin(dL * 0.5) * sin(dL * 0.5);
const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
return (float)(R * c);
}

void setSinkPosition(bool locked, double lat, double lon) {
s_sinkLocked = locked;
if (locked) {
    s_sinkLat = lat;
    s_sinkLon = lon;
}
}

bool isSinkLocked() { return s_sinkLocked; }

float distanceFromSinkMeters(double lat, double lon) {
if (!s_sinkLocked) return 0.0f;
// basic validity
if (lat == 0.0 && lon == 0.0) return 0.0f;
return haversine_m(s_sinkLat, s_sinkLon, lat, lon);
}

} // namespace NetRuntime
