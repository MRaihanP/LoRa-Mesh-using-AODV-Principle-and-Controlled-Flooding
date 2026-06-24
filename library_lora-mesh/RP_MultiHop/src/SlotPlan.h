#pragma once
#include <Arduino.h>
#include "StaticRoute.h"
#include "NetRuntimeConfig.h"

namespace SlotPlan {

using Id24 = StaticRoute::Id24;

// -----------------------------------------------------------------------------
// 1) DATA phase
// -----------------------------------------------------------------------------
static constexpr uint32_t FRAME_LEN_SEC = 40;   // DATA phase
static constexpr uint32_t HELLO_LEN_SEC = 20;   // HELLO phase
static constexpr uint32_t SUPERFRAME_LEN_SEC = FRAME_LEN_SEC + HELLO_LEN_SEC; // 60 s

// Panjang DATA phase runtime:
// - kalau ada NetRuntimeConfig → pakai frameLenSec dari config
// - kalau tidak ada → fallback 40
inline uint32_t frameLenSec() {
  if (NetRuntime::hasConfig()) {
    const auto& cfg = NetRuntime::current().frame;
    if (cfg.frameLenSec > 0) {
      return cfg.frameLenSec;
    }
  }
  return FRAME_LEN_SEC;
}

// Panjang HELLO phase runtime:
// - kalau config punya helloLenSec → pakai itu
// - kalau tidak ada → fallback 20
inline uint32_t helloLenSec() {
  if (NetRuntime::hasConfig()) {
    const auto& cfg = NetRuntime::current().frame;
    if (cfg.helloLenSec > 0) {
      return cfg.helloLenSec;
    }
  }
  return HELLO_LEN_SEC;
}

// Panjang total superframe runtime
inline uint32_t superframeLenSec() {
  return frameLenSec() + helloLenSec();
}

// Posisi detik di dalam superframe: 0..59
inline uint8_t posInCycleSec(uint32_t unixNow) {
  uint32_t sflen = superframeLenSec();
  if (sflen == 0) return 0;
  return (uint8_t)(unixNow % sflen);
}

// Fase DATA: 0..39
inline bool isDataPhase(uint32_t unixNow) {
  return posInCycleSec(unixNow) < frameLenSec();
}

// Fase HELLO: 40..59
inline bool isHelloPhase(uint32_t unixNow) {
  return posInCycleSec(unixNow) >= frameLenSec();
}

// Posisi detik di dalam HELLO phase: 0..19
inline uint8_t posInHelloSec(uint32_t unixNow) {
  uint8_t pos = posInCycleSec(unixNow);
  uint32_t dlen = frameLenSec();
  if (pos < dlen) return 0;
  return (uint8_t)(pos - dlen);
}

// -----------------------------------------------------------------------------
// 2) Fallback DATA slot
// -----------------------------------------------------------------------------
struct SlotInfo {
  Id24    id;
  uint8_t startSec;  // inklusif
  uint8_t endSec;    // eksklusif
};

static constexpr SlotInfo SLOTS[] = {
  { StaticRoute::ID_R1,  0,  4 },
  { StaticRoute::ID_R2,  4, 10 },
  { StaticRoute::ID_N1, 10, 20 },
  { StaticRoute::ID_N2, 20, 30 },
  { StaticRoute::ID_N3, 30, 40 }
};

inline const SlotInfo* findSlotFallback(Id24 myId) {
  for (size_t i = 0; i < sizeof(SLOTS) / sizeof(SLOTS[0]); ++i) {
    if (SLOTS[i].id == myId) {
      return &SLOTS[i];
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// 3) DATA slot window
// -----------------------------------------------------------------------------
inline bool isInMyTxWindow(Id24 myId, uint32_t unixNow, uint32_t &frameIdxOut) {
  // 1) Runtime Config
  if (NetRuntime::hasConfig() && NetRuntime::current().dataSlotCount > 0) {
    return NetRuntime::isInDataWindow(myId, unixNow, frameIdxOut);
  }

  // 2) Fallback superframe
  uint32_t sflen = superframeLenSec();
  if (sflen == 0) {
    frameIdxOut = 0;
    return false;
  }

  frameIdxOut = unixNow / sflen;

  // DATA hanya aktif pada phase DATA
  if (!isDataPhase(unixNow)) {
    return false;
  }

  uint32_t posInData = posInCycleSec(unixNow); // 0..39

  const SlotInfo* si = findSlotFallback(myId);
  if (!si) return false;

  return (posInData >= si->startSec && posInData < si->endSec);
}

inline uint8_t slotIdFor(Id24 myId) {
  if (NetRuntime::hasConfig() && NetRuntime::current().dataSlotCount > 0) {
    return NetRuntime::dataSlotIdFor(myId);
  }

  for (size_t i = 0; i < sizeof(SLOTS) / sizeof(SLOTS[0]); ++i) {
    if (SLOTS[i].id == myId) {
      return (uint8_t)(i + 1);
    }
  }
  return 0;
}

// -----------------------------------------------------------------------------
// 4) HELLO slot
// -----------------------------------------------------------------------------
static constexpr uint32_t HELLO_PERIOD_FRAMES = 2; // legacy only

static constexpr Id24 HELLO_IDS[] = {
  StaticRoute::ID_S,
  StaticRoute::ID_R1,
  StaticRoute::ID_R2,
  StaticRoute::ID_N1,
  StaticRoute::ID_N2,
  StaticRoute::ID_N3
};

static constexpr uint8_t HELLO_SLOT_SEC      = 2;
static constexpr uint8_t HELLO_DEVICE_COUNT  =
    (uint8_t)(sizeof(HELLO_IDS) / sizeof(HELLO_IDS[0]));
static constexpr uint8_t HELLO_FRAME_USED_SEC =
    (uint8_t)(HELLO_SLOT_SEC * HELLO_DEVICE_COUNT);

// Legacy helper dipertahankan agar kode lama tetap compile,
// tetapi keputusan HELLO sekarang dilakukan dengan isHelloPhase(unixNow).
inline bool isHelloFrame(uint32_t frameIdx) {
  (void)frameIdx;
  return false;
}

// Parameter diperlakukan sebagai posisi di dalam HELLO phase (0..19)
inline bool isInMyHelloSlot(Id24 myId, uint8_t posInHelloSecVal) {
  if (NetRuntime::hasConfig() && NetRuntime::current().helloSlotCount > 0) {
    return NetRuntime::isInHelloSlot(myId, posInHelloSecVal);
  }

  if (posInHelloSecVal >= helloLenSec()) {
    return false;
  }

  int slotIndex = -1;
  for (size_t i = 0; i < sizeof(HELLO_IDS) / sizeof(HELLO_IDS[0]); ++i) {
    if (HELLO_IDS[i] == myId) {
      slotIndex = (int)i;
      break;
    }
  }
  if (slotIndex < 0) return false;

  uint8_t myStart = (uint8_t)(slotIndex * HELLO_SLOT_SEC);
  uint8_t myEnd   = (uint8_t)(myStart + HELLO_SLOT_SEC);

  return (posInHelloSecVal >= myStart && posInHelloSecVal < myEnd);
}

} // namespace SlotPlan