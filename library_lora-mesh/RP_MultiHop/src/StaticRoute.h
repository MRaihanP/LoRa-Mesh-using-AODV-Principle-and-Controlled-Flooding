#pragma once
#include <Arduino.h>
#include "NetRuntimeConfig.h"

namespace StaticRoute {

using Id24 = uint32_t;

// -----------------------------------------------------------------------------
// 1) Fallback ID & tree statis (layout lama)
// -----------------------------------------------------------------------------
// Kamu boleh tetap memakai konstanta ini untuk awal,
// tapi idealnya ID real dimasukkan via NetworkConfig dari sketch.
//
//   Sink   = 93B751  (S)
//   Relay1 = DE5C6F  (R1)
//   Relay2 = DA782A  (R2)
//   Node1  = E21FB5  (N1)
//   Node2  = 1D462D  (N2)
//   Node3  = 87BC16  (N3)
static constexpr Id24 ID_S   = 0x73BE56; 
static constexpr Id24 ID_R1  = 0xDE5C6F;
static constexpr Id24 ID_R2  = 0xDA782A;
static constexpr Id24 ID_N1  = 0xE21FB5;
static constexpr Id24 ID_N2  = 0x1D462D;
static constexpr Id24 ID_N3  = 0x87BC16;

// Tabel routing fallback: child -> parent
struct RouteEntry {
  Id24 id;
  Id24 parent;
};

// Pohon fallback (5 hop):
//   N3 -> N2 -> N1 -> R2 -> R1 -> S -> (root)
static constexpr RouteEntry ROUTES[] = {
  { ID_S,   0      },
  { ID_R1,  ID_S   },
  { ID_R2,  ID_R1  },
  { ID_N1,  ID_R2  },
  { ID_N2,  ID_N1  },
  { ID_N3,  ID_N2  },
};

// -----------------------------------------------------------------------------
// 2) Helper hex24 / parseHex24
// -----------------------------------------------------------------------------
inline String hex24(Id24 v) {
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (unsigned)(v & 0xFFFFFFu));
  return String(buf);
}

inline Id24 parseHex24(const String& s) {
  const char* c = s.c_str();
  return (Id24)strtoul(c, nullptr, 16) & 0xFFFFFFu;
}

// -----------------------------------------------------------------------------
// 3) Parent lookup (runtime config > fallback)
// -----------------------------------------------------------------------------
inline Id24 findParent(Id24 id) {
  // 1) Coba dari NetworkConfig (runtime)
  if (NetRuntime::hasConfig()) {
    if (const NetRuntime::StaticRouteEntry* r =
            NetRuntime::findRouteByChild(id))
    {
      return r->parent;
    }
  }

  // 2) Fallback ke tabel statis lama
  for (auto &e : ROUTES) {
    if (e.id == id) return e.parent;
  }
  return 0;
}

inline bool isDirectChild(Id24 parentId, Id24 childId) {
  // Runtime dulu
  if (NetRuntime::hasConfig() && NetRuntime::current().routeCount > 0) {
    if (const NetRuntime::StaticRouteEntry* r =
            NetRuntime::findRouteByChild(childId))
    {
      return (r->parent == parentId);
    }
  }

  // Fallback
  for (auto &e : ROUTES) {
    if (e.id == childId) return (e.parent == parentId);
  }
  return false;
}

// -----------------------------------------------------------------------------
// 4) Alias <-> ID (runtime config > fallback)
// -----------------------------------------------------------------------------
inline const char* aliasForId(Id24 id) {
  // 1) Runtime Config
  if (NetRuntime::hasConfig()) {
    if (const char* a = NetRuntime::aliasForId(id)) {
      return a;
    }
  }

  // 2) Fallback ke mapping statis
  if (id == ID_S)   return "S";
  if (id == ID_R1)  return "R1";
  if (id == ID_R2)  return "R2";
  if (id == ID_N1)  return "N1";
  if (id == ID_N2)  return "N2";
  if (id == ID_N3)  return "N3";
  return "UNK";
}

inline uint8_t staticHopToSink(Id24 id) {
  // 1) Runtime Config (kalau ada tree runtime)
  if (NetRuntime::hasConfig() && NetRuntime::current().routeCount > 0) {
    return NetRuntime::staticHopToSink(id);
  }

  // 2) Fallback tree statis lama:
  //   N3 -> N2 -> N1 -> R2 -> R1 -> S
  if (id == ID_S)   return 0;
  if (id == ID_R1)  return 1;
  if (id == ID_R2)  return 2;
  if (id == ID_N1)  return 3;
  if (id == ID_N2)  return 4;
  if (id == ID_N3)  return 5;
  return 255; // unknown
}

inline Id24 idForAlias(const String& alias) {
  // 1) Runtime Config
  if (NetRuntime::hasConfig()) {
    Id24 id = NetRuntime::idForAlias(alias);
    if (id != 0) return id;
  }

  // 2) Fallback mapping statis
  if (alias == "S")   return ID_S;
  if (alias == "R1")  return ID_R1;
  if (alias == "R2")  return ID_R2;
  if (alias == "N1")  return ID_N1;
  if (alias == "N2")  return ID_N2;
  if (alias == "N3")  return ID_N3;
  return 0;
}

inline const char* aliasForHex(const String& hex) {
  Id24 id = parseHex24(hex);
  return aliasForId(id);
}

} // namespace StaticRoute
