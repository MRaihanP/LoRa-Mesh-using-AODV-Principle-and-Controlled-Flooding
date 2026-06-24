#include "Sink.h"
extern "C" void __attribute__((weak)) sinkRawLineMirror(const char* line) {
  (void)line;
}
#include "Packet.h"
#include "SlotPlan.h"   // <-- untuk frameIdx & slot HELLO
#include "NetRuntimeConfig.h"

using namespace StaticRoute;
using namespace DataWire;

// Ambil konfigurasi dari NetConfig.h
using NetTopology::RX_SENSITIVITY_DBM;
using NetTopology::LAST_DATA_SLOT_ID;
using NetTopology::MAX_NODES_TRACK;
using NetPrint::SinkPrintMode;
using NetPrint::SINK_PRINT_MODE;

// ----------------------------------------------------------
// Alias type lokal (biar tidak pakai MeshSink::Id24 yang private)
// ----------------------------------------------------------
using Id24 = StaticRoute::Id24;

// ----------------------------------------------------------
// Tracking "ever seen" node (per source ID, lintas frame)
// ----------------------------------------------------------

struct EverSeenNode {
  bool     used;
  Id24     id;
  uint32_t pkts;   // total paket diterima (lintas frame)
  uint32_t lost;   // total frame di mana node ini "expected" tapi tidak kirim
};

static EverSeenNode gEverSeen[MAX_NODES_TRACK];

// ----------------------------------------------------------
// Packet loss tracker per ID (berdasarkan gap sequence)
//   -> dipakai untuk field CSV: PACKET_LOSS
// ----------------------------------------------------------
struct PktLossTracker {
  bool     used;
  Id24     id;
  uint16_t lastSeq;
  uint32_t lost;
};

static PktLossTracker gLossTrackers[MAX_NODES_TRACK];

// ----------------------------------------------------------
// Statistik per-frame per-node (untuk summary ANALYZED_ONLY)
// Disimpan "last metrics" di frame tsb.
// ----------------------------------------------------------
struct PerNodeFrameStats {
  bool     used;
  Id24     id;
  uint32_t frameIdx;
  uint32_t count;

  uint8_t  lastHopCount;
  uint32_t lastE2ELat;
  float    lastAvgLatHop;
  float    lastAvgRssi;
  float    lastAvgSnr;
  float    lastThroughput;
  float    lastLinkMargin;
  int      lastBattPct;
  int      lastPktSize;
  float    lastDistanceM;

  String   lastPath;
};

static PerNodeFrameStats gFrameStats[MAX_NODES_TRACK];
static bool     gHaveFrame       = false;
static uint32_t gCurrentFrameIdx = 0;

// ----------------------------------------------------------
// Helper: RTC "Unix-like" time (UTC)
//   -> IMPLEMENTASINYA ADA DI MODUL RTC KAMU
// ----------------------------------------------------------
extern uint32_t nowUnixLike();

// ----------------------------------------------------------
// Helper: EverSeen & LossTracker & FrameStats
// ----------------------------------------------------------
static EverSeenNode* getEverNode(Id24 id) {
  if (id == 0) return nullptr;
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (gEverSeen[i].used && gEverSeen[i].id == id) {
      return &gEverSeen[i];
    }
  }
  return nullptr;
}

static EverSeenNode* ensureEverSeen(Id24 id) {
  if (id == 0) return nullptr;
  if (EverSeenNode* e = getEverNode(id)) return e;

  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (!gEverSeen[i].used) {
      gEverSeen[i].used = true;
      gEverSeen[i].id   = id;
      gEverSeen[i].pkts = 0;
      gEverSeen[i].lost = 0;
      return &gEverSeen[i];
    }
  }
  return nullptr; // tidak ada slot kosong
}

static PktLossTracker* ensureLossTracker(Id24 id) {
  if (id == 0) return nullptr;
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (gLossTrackers[i].used && gLossTrackers[i].id == id) {
      return &gLossTrackers[i];
    }
  }
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (!gLossTrackers[i].used) {
      gLossTrackers[i].used    = true;
      gLossTrackers[i].id      = id;
      gLossTrackers[i].lastSeq = 0;
      gLossTrackers[i].lost    = 0;
      return &gLossTrackers[i];
    }
  }
  return nullptr;
}

static void clearFrameStats() {
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    gFrameStats[i].used      = false;
    gFrameStats[i].id        = 0;
    gFrameStats[i].frameIdx  = 0;
    gFrameStats[i].count     = 0;
    gFrameStats[i].lastHopCount   = 0;
    gFrameStats[i].lastE2ELat     = 0;
    gFrameStats[i].lastAvgLatHop  = 0.0f;
    gFrameStats[i].lastAvgRssi    = 0.0f;
    gFrameStats[i].lastAvgSnr     = 0.0f;
    gFrameStats[i].lastThroughput = 0.0f;
    gFrameStats[i].lastLinkMargin = 0.0f;
    gFrameStats[i].lastBattPct    = 0;
    gFrameStats[i].lastPktSize    = 0;
    gFrameStats[i].lastDistanceM  = 0.0f;
    gFrameStats[i].lastPath       = "";
  }
}

static PerNodeFrameStats* getFrameStatsFor(Id24 id) {
  if (id == 0) return nullptr;
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (gFrameStats[i].used && gFrameStats[i].id == id) {
      return &gFrameStats[i];
    }
  }
  return nullptr;
}

static PerNodeFrameStats* ensureFrameStats(Id24 id, uint32_t frameIdx) {
  if (id == 0) return nullptr;

  // Cari existing
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (gFrameStats[i].used && gFrameStats[i].id == id) {
      if (gFrameStats[i].frameIdx != frameIdx) {
        // frame berubah -> reset slot untuk frame baru
        gFrameStats[i].frameIdx       = frameIdx;
        gFrameStats[i].count          = 0;
        gFrameStats[i].lastHopCount   = 0;
        gFrameStats[i].lastE2ELat     = 0;
        gFrameStats[i].lastAvgLatHop  = 0.0f;
        gFrameStats[i].lastAvgRssi    = 0.0f;
        gFrameStats[i].lastAvgSnr     = 0.0f;
        gFrameStats[i].lastThroughput = 0.0f;
        gFrameStats[i].lastLinkMargin = 0.0f;
        gFrameStats[i].lastBattPct    = 0;
        gFrameStats[i].lastPktSize    = 0;
        gFrameStats[i].lastDistanceM  = 0.0f;
        gFrameStats[i].lastPath       = "";
      }
      return &gFrameStats[i];
    }
  }

  // Cari slot kosong
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (!gFrameStats[i].used) {
      gFrameStats[i].used           = true;
      gFrameStats[i].id             = id;
      gFrameStats[i].frameIdx       = frameIdx;
      gFrameStats[i].count          = 0;
      gFrameStats[i].lastHopCount   = 0;
      gFrameStats[i].lastE2ELat     = 0;
      gFrameStats[i].lastAvgLatHop  = 0.0f;
      gFrameStats[i].lastAvgRssi    = 0.0f;
      gFrameStats[i].lastAvgSnr     = 0.0f;
      gFrameStats[i].lastThroughput = 0.0f;
      gFrameStats[i].lastLinkMargin = 0.0f;
      gFrameStats[i].lastBattPct    = 0;
      gFrameStats[i].lastPktSize    = 0;
      gFrameStats[i].lastDistanceM  = 0.0f;
      gFrameStats[i].lastPath       = "";
      return &gFrameStats[i];
    }
  }

  return nullptr;
}

// ----------------------------------------------------------
// Helper: format waktu lokal (UTC+7) dari epoch UTC
// ----------------------------------------------------------
static void formatLocalTimeUTC7(uint32_t epochUtc, char* buf, size_t n) {
  uint32_t t = epochUtc + 7U * 3600U;  // shift ke UTC+7
  uint32_t secOfDay = t % 86400U;
  uint32_t h = secOfDay / 3600U;
  uint32_t m = (secOfDay % 3600U) / 60U;
  uint32_t s = secOfDay % 60U;
  snprintf(buf, n, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// ----------------------------------------------------------
// Summary per frame (dipanggil setelah frame selesai)
// frameTimeUtc = nowTs (epoch UTC) tepat sebelum dicetak
// ----------------------------------------------------------
static void flushFrameSummary(uint32_t frameIdx, uint32_t frameTimeUtc) {
  bool anySeen = false;
  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (gEverSeen[i].used) { anySeen = true; break; }
  }
  if (!anySeen) return;

  char timeBuf[16];
  formatLocalTimeUTC7(frameTimeUtc, timeBuf, sizeof(timeBuf));

  Serial.print("=== NETWORK SUMMARY FRAME: ");
  Serial.print(frameIdx);
  Serial.print(" Time: ");
  Serial.print(timeBuf);
  Serial.println(" (UTC+7) ===");

  Serial.println("Alias  ID       pkts  lost  loss%   HOP  avgRSSI   avgSNR   E2ELat(s) avgLat  Batt%      THR     LM Size  Dist(m)  path");

  for (int i = 0; i < MAX_NODES_TRACK; ++i) {
    if (!gEverSeen[i].used) continue;

    EverSeenNode* e = &gEverSeen[i];
    Id24 id = e->id;

    const char* alias = StaticRoute::aliasForId(id);
    if (!alias) alias = "UNK";

    const PerNodeFrameStats* st = getFrameStatsFor(id);
    bool hasThisFrame = (st && st->frameIdx == frameIdx && st->count > 0);

    // Node pernah ada, tapi tidak muncul di frame ini -> dianggap lost 1 hop
    if (!hasThisFrame) {
      e->lost += 1;
    }

    uint32_t pkts  = e->pkts;
    uint32_t lost  = e->lost;
    uint32_t total = pkts + lost;
    float    lossPct = (total > 0)
                       ? (100.0f * ((float)lost / (float)total))
                       : 0.0f;

    uint8_t  hop  = 0;
    uint32_t e2e  = 0;
    float    avgH = 0.0f;
    float    rssi = 0.0f;
    float    snr  = 0.0f;
    float    thr  = 0.0f;
    float    lm   = 0.0f;
    int      batt = 0;
    int      size = 0;
    float    dist = 0.0f;
    const char* pathOut = "-";

    if (hasThisFrame && st) {
      hop  = st->lastHopCount;
      e2e  = st->lastE2ELat;
      avgH = st->lastAvgLatHop;
      rssi = st->lastAvgRssi;
      snr  = st->lastAvgSnr;
      thr  = st->lastThroughput;
      lm   = st->lastLinkMargin;
      batt = st->lastBattPct;
      size = st->lastPktSize;
      dist = st->lastDistanceM;
      if (st->lastPath.length() > 0) {
        pathOut = st->lastPath.c_str();
      }
    }

    char idHex[7];
    snprintf(idHex, sizeof(idHex), "%06X", (unsigned)id);

    // Header:
    // Alias  ID       pkts  lost  loss%   HOP  avgRSSI   avgSNR   E2ELat(s) avgLat  Batt%     THR     LM Size  path
    Serial.printf("%-5s %-8s %5lu %5lu %6.1f %5u %8.1f %8.1f %10lu %7.3f %6d %8.1f %6.1f %4d %8.1f  %-s\n",
                  alias,
                  idHex,
                  (unsigned long)pkts,
                  (unsigned long)lost,
                  lossPct,
                  (unsigned)hop,
                  rssi,
                  snr,
                  (unsigned long)e2e,
                  avgH,
                  batt,
                  thr,
                  lm,
                  size,
                  dist,
                  pathOut);
  }

  Serial.println("========================");
  Serial.println();
}

// ----------------------------------------------------------
// Implementasi MeshSink
// ----------------------------------------------------------

// Helper alias <-> id: sekarang delegasi ke StaticRoute (runtime-aware)
const char* MeshSink::aliasForId(Id24 id) {
  // Delegasikan ke StaticRoute (yang sudah runtime-aware via NetRuntimeConfig)
  return StaticRoute::aliasForId(id);
}

MeshSink::Id24 MeshSink::idForAlias(const String& alias) {
  // Delegasikan ke StaticRoute (yang sudah runtime-aware via NetRuntimeConfig)
  return StaticRoute::idForAlias(alias);
}

// path: "N2-N1-R2-R1-S" atau tanpa "S"
// Child terakhir utk Sink = elemen sebelum "S" (kalau ada), jika tidak ada, ambil terakhir
bool MeshSink::extractChildAliasFromPath(const String& path, String& outChildAlias) {
  if (!path.length()) return false;

  const int MAX_PARTS = 10;
  String parts[MAX_PARTS];
  int count = 0;
  int start = 0;
  for (int i = 0; i <= path.length(); ++i) {
    if (i == path.length() || path[i] == '-') {
      if (count < MAX_PARTS) {
        parts[count++] = path.substring(start, i);
      }
      start = i + 1;
    }
  }
  if (count == 0) return false;

  if (parts[count - 1] == "S" && count >= 2) {
    outChildAlias = parts[count - 2];
    return true;
  }

  outChildAlias = parts[count - 1];
  return true;
}

MeshSink::MeshSink(const BoardPins& pins)
: MeshDeviceBase("SINK", pins),
  _id24(0),
  _idHex6(""),
  _lastHelloFrameIdx(0),
  _helloSentThisFrame(false)
{
  // gEverSeen / gFrameStats / gLossTrackers sudah 0-in oleh BSS
}

// =====================================================
// HELLO topology graph helpers (Sink)
// =====================================================

void MeshSink::clearHelloGraph() {
  for (int i = 0; i < MAX_DEVICES; ++i) {
    _devices[i].id             = 0;
    _devices[i].alias          = "";
    _devices[i].hopSelf        = 255;
    _devices[i].battPct        = 0;
    _devices[i].lastHelloFrame = 0;
    _devices[i].used           = false;
  }
  for (int i = 0; i < MAX_LINKS; ++i) {
    _links[i].srcId            = 0;
    _links[i].dstId            = 0;
    _links[i].lastRssi         = 0;
    _links[i].hopDst           = 255;
    _links[i].lastHelloFrame   = 0;
    _links[i].used             = false;
  }
  _lastHelloSummaryFrameIdx = 0;
}

void MeshSink::updateDeviceFromHello(Id24 id,
                                     const String& alias,
                                     uint8_t hopSelf,
                                     uint8_t battPct,
                                     uint32_t frameIdx)
{
  if (id == 0) return;

  int freeIdx  = -1;
  int foundIdx = -1;

  for (int i = 0; i < MAX_DEVICES; ++i) {
    if (_devices[i].used) {
      if (_devices[i].id == id) {
        foundIdx = i;
        break;
      }
    } else if (freeIdx < 0) {
      freeIdx = i;
    }
  }

  int idx = foundIdx;
  if (idx < 0) {
    if (freeIdx >= 0) {
      idx = freeIdx;
    } else {
      // kalau penuh: ganti yang lastHello paling lama
      uint32_t oldest = _devices[0].lastHelloFrame;
      idx = 0;
      for (int i = 1; i < MAX_DEVICES; ++i) {
        if (_devices[i].lastHelloFrame < oldest) {
          oldest = _devices[i].lastHelloFrame;
          idx = i;
        }
      }
    }
  }

  _devices[idx].used           = true;
  _devices[idx].id             = id;
  _devices[idx].alias          = alias;
  _devices[idx].hopSelf        = hopSelf;
  _devices[idx].battPct        = battPct;
  _devices[idx].lastHelloFrame = frameIdx;
}

void MeshSink::updateLinkFromHello(Id24 srcId,
                                   Id24 dstId,
                                   uint8_t hopDst,
                                   int16_t rssi,
                                   uint32_t frameIdx)
{
  if (srcId == 0 || dstId == 0 || srcId == dstId) return;

  int freeIdx  = -1;
  int foundIdx = -1;

  for (int i = 0; i < MAX_LINKS; ++i) {
    if (_links[i].used) {
      if (_links[i].srcId == srcId && _links[i].dstId == dstId) {
        foundIdx = i;
        break;
      }
    } else if (freeIdx < 0) {
      freeIdx = i;
    }
  }

  int idx = foundIdx;
  if (idx < 0) {
    if (freeIdx >= 0) {
      idx = freeIdx;
    } else {
      // kalau penuh: ganti yang lastHello paling lama
      uint32_t oldest = _links[0].lastHelloFrame;
      idx = 0;
      for (int i = 1; i < MAX_LINKS; ++i) {
        if (_links[i].lastHelloFrame < oldest) {
          oldest = _links[i].lastHelloFrame;
          idx = i;
        }
      }
    }
  }

  _links[idx].used            = true;
  _links[idx].srcId           = srcId;
  _links[idx].dstId           = dstId;
  _links[idx].lastRssi        = rssi;
  _links[idx].hopDst          = hopDst;
  _links[idx].lastHelloFrame  = frameIdx;
}

void MeshSink::printHelloSummary(uint32_t frameIdx) const {
  Serial.println("=== HELLO TOPOLOGY SUMMARY ===");
  Serial.print("FrameIdx=");
  Serial.println(frameIdx);

  Serial.println("[Devices]");
  for (int i = 0; i < MAX_DEVICES; ++i) {
    if (!_devices[i].used) continue;
    Serial.print("  ");
    Serial.print(_devices[i].alias);
    Serial.print(" (id=0x");
    Serial.printf("%06X", (unsigned int)_devices[i].id);
    Serial.print(") hopSelf=");
    Serial.print(_devices[i].hopSelf);
    Serial.print(" batt=");
    Serial.print((int)_devices[i].battPct);
    Serial.print(" lastHello=");
    Serial.println(_devices[i].lastHelloFrame);
  }

  Serial.println("[Links]");
  for (int i = 0; i < MAX_LINKS; ++i) {
    if (!_links[i].used) continue;
    // hanya tampilkan link yang terakhir di frame yang sama, supaya lebih fresh
    if (_links[i].lastHelloFrame != frameIdx) continue;

    const char* srcAlias = aliasForId(_links[i].srcId);
    const char* dstAlias = aliasForId(_links[i].dstId);

    Serial.print("  ");
    Serial.print(srcAlias ? srcAlias : "?");
    Serial.print(" -> ");
    Serial.print(dstAlias ? dstAlias : "?");
    Serial.print(" rssi=");
    Serial.print(_links[i].lastRssi);
    Serial.print(" hopDst=");
    Serial.print(_links[i].hopDst);
    Serial.print(" lastHello=");
    Serial.println(_links[i].lastHelloFrame);
  }
  Serial.println("==============================");
}

void MeshSink::begin() {
  MeshDeviceBase::begin();
  initLoraUart();

  _id24   = Id::id24();
  _idHex6 = Id::idHex6();

  clearHelloGraph();

  if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
    Serial.printf("[SINK] ID=%s\n", _idHex6.c_str());
  }
}

void MeshSink::loop() {
  static String rxBuf;

  // ------------------------------------------------------
  // 0) FRAME / SLOT kalkulasi untuk HELLO BEACON Sink
  // ------------------------------------------------------
  uint32_t unixNow  = nowUnixLike(); // epoch UTC dari RTC
  uint32_t frameIdx = 0;
  uint8_t  posSec   = 0;
  bool     isHelloFrameNow = false;

  // superframe = DATA 40 detik + HELLO 20 detik
  uint32_t superframeLenSec = SlotPlan::superframeLenSec();

  if (superframeLenSec > 0) {
    frameIdx        = unixNow / superframeLenSec;
    posSec          = SlotPlan::posInCycleSec(unixNow);
    isHelloFrameNow = SlotPlan::isHelloPhase(unixNow);
  }

  // Reset flag HELLO setiap ganti frame
  if (frameIdx != _lastHelloFrameIdx) {
    _lastHelloFrameIdx  = frameIdx;
    _helloSentThisFrame = false;

    // HOOK: Frame advance (frame berubah)
    if (_hkFrameAdv) _hkFrameAdv(_hkUser);
  }

  // Kirim BEACON / ROUTE_HELLO singkat dari Sink di frame HELLO,
  // hanya di slot 2 detik milik Sink (lihat SlotPlan::HELLO_IDS).
  if (isHelloFrameNow &&
      !_helloSentThisFrame &&
      SlotPlan::isInMyHelloSlot(_id24, SlotPlan::posInHelloSec(unixNow)))
  {
    // Layout HELLO:
    //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
    //  4=parentAlias, 5=hopToSinkSelf, 6=frameIdx, 7=posInFrame,
    //  8=battPct, 9=neighCount
    // Layout HELLO (konsisten dengan Node):
    //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
    //  4=parentAlias, 5=hopToSinkSelf, 6=frameIdx, 7=posInFrame,
    //  8=battPct, 9=role, 10=neighCount
    String pkt;
    pkt.reserve(64);
    pkt  = MeshProto::ROUTE_HELLO;           // "10"
    pkt += ",";
    pkt += String((int)DataWire::ROUTE_VER); // routeVer
    pkt += ",";
    pkt += _idHex6;                          // srcIdHex (Sink ID)
    pkt += ",";
    // aliasSelf diambil dari StaticRoute/NetRuntimeConfig kalau ada
    {
      const char* selfAliasC = StaticRoute::aliasForId(_id24);
      pkt += (selfAliasC && selfAliasC[0]) ? selfAliasC : "S";
    }
    pkt += ",";
    pkt += "-";                              // parentAlias (tidak relevan untuk Sink)
    pkt += ",";
    pkt += "0";                              // hopToSinkSelf = 0
    pkt += ",";
    pkt += String((unsigned long)frameIdx);  // frameIdx
    pkt += ",";
    pkt += String((int)posSec);              // posInCycle superframe
    pkt += ",";
    pkt += "0";                              // battPct (tidak dipakai)
    pkt += ",";
    pkt += String((int)NetRuntime::DeviceRole::SINK); // role = SINK (index 9)
    pkt += ",";
    pkt += "0";                              // neighCount = 0 (Sink tidak gossip)

    // HOOK: TX HELLO Sink
    if (_hkTxHello) _hkTxHello(_hkUser);

    Lora.print(pkt);
    Lora.print('\n');

    // if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
    //   Serial.print("[SINK HELLO TX] ");
    //   Serial.println(pkt);
    // }

    _helloSentThisFrame = true;
    blinkStatus();
  }

  // ------------------------------------------------------
  // 0B) ANALYZED_ONLY: cetak HELLO summary setelah window
  //     slot HELLO di frame HELLO ini selesai.
  //     - Menggunakan konfigurasi runtime (helloSlotSec, helloSlotCount)
  //       jika ada.
  //     - Jika tidak ada config, fallback ke SlotPlan::HELLO_FRAME_USED_SEC.
  // ------------------------------------------------------
  if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY && isHelloFrameNow) {
    uint8_t helloUsedSec = 0;

    if (NetRuntime::hasConfig()) {
      const auto& cfg = NetRuntime::current();
      if (cfg.frame.helloSlotSec > 0 && cfg.helloSlotCount > 0) {
        uint32_t usedSec =
          (uint32_t)cfg.frame.helloSlotSec * (uint32_t)cfg.helloSlotCount;
        if (usedSec > 255U) usedSec = 255U;
        helloUsedSec = (uint8_t)usedSec;
      }
    }

    // Fallback: pakai konstanta default dari SlotPlan
    if (helloUsedSec == 0) {
      helloUsedSec = SlotPlan::HELLO_FRAME_USED_SEC;
    }

    // Pastikan hanya sekali per frame
    static uint32_t lastHelloSummaryPrintedFrameIdx = 0;

    // Syarat:
    // - posSec sudah lewat window HELLO
    // - frameIdx ini belum pernah kita cetak summary HELLO-nya
    // - _lastHelloSummaryFrameIdx adalah frame HELLO yang memang punya data
    if (helloUsedSec > 0 &&
        posSec >= helloUsedSec &&
        frameIdx != 0 &&
        frameIdx != lastHelloSummaryPrintedFrameIdx &&
        _lastHelloSummaryFrameIdx == frameIdx)
    {
      printHelloSummary(_lastHelloSummaryFrameIdx);
      lastHelloSummaryPrintedFrameIdx = frameIdx;
    }
  }

  // ------------------------------------------------------
  // 1) RX: kumpulkan semua byte dari LoRa UART
  // ------------------------------------------------------
  while (Lora.available()) {
    char c = (char)Lora.read();
    rxBuf += c;
  }

  // ------------------------------------------------------
  // 2) Proses paket "<payload>\n" + "<RSSI_BYTE>"
  // ------------------------------------------------------
  while (true) {
    int nlIdx = rxBuf.indexOf('\n');
    if (nlIdx < 0) break;                 // belum ada '\n'
    if (nlIdx + 1 >= rxBuf.length()) break; // byte RSSI belum masuk

    uint8_t rssiByte = (uint8_t)rxBuf[nlIdx + 1];
    String  line     = rxBuf.substring(0, nlIdx);
    rxBuf.remove(0, nlIdx + 2);  // buang payload + '\n' + RSSI

    line.trim();
    if (!line.length()) continue;

    uint32_t nowTs  = nowUnixLike();

    int rssi_dBm = (int)rssiByte - 256;

    // 2A) Filter ACK
    if (line.startsWith("ACK,")) {
      // Jika ingin debug ACK yang terdengar di sink, bisa print di sini
      // Serial.print("[SINK RX ACK RAW] ");
      // Serial.println(line);
      continue;
    }

    // 2B) ROUTE_HELLO: frame khusus routing / gosip topologi
    {
      int firstComma = line.indexOf(',');
      String proto = (firstComma < 0) ? line : line.substring(0, firstComma);
      proto.trim();
      if (proto == MeshProto::ROUTE_HELLO) {
        const int MAXF_H = 40;
        String fh[MAXF_H];
        int fch = 0, stH = 0;
        for (int i = 0; i <= line.length(); ++i) {
          if (i == line.length() || line[i] == ',') {
            if (fch < MAXF_H) fh[fch++] = line.substring(stH, i);
            stH = i + 1;
          }
        }
        if (fch < 10) {
          // if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
          //   Serial.print("[SINK HELLO] Drop: field < 10 | line=");
          //   Serial.println(line);
          // }
          continue;
        }

        // HOOK: RX HELLO (HELLO valid)
        if (_hkRxHello) _hkRxHello(_hkUser, line.c_str());

        //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
        //  4=parentAlias, 5=hopToSinkSelf, 6=frameIdx, 7=posInFrame,
        //  8=battPct, 9=neighCount, 10.. = triplet neighbors
        //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
        //  4=parentAlias, 5=hopToSinkSelf, 6=frameIdx, 7=posInFrame,
        //  8=battPct, 9=role, 10=neighCount, 11.. = triplet neighbors
        String srcIdHex     = fh[2]; srcIdHex.trim();
        String aliasSelf    = fh[3]; aliasSelf.trim();
        String parentAlias  = fh[4]; parentAlias.trim();
        uint8_t hopSelf     = (uint8_t)fh[5].toInt();
        uint32_t frameIdxH  = (uint32_t)fh[6].toInt();
        uint8_t battPct     = (uint8_t)fh[8].toInt();
        // fh[9] = role (dibaca Node, Sink tidak perlu menggunakannya)

        uint8_t neighCount  = 0;
        if (fch >= 11) {
          neighCount = (uint8_t)fh[10].toInt();
        }

        // Map alias ke Id24 (via StaticRoute/NetRuntimeConfig)
        Id24 srcId = idForAlias(aliasSelf);

        // Update device info di graf Sink
        updateDeviceFromHello(srcId, aliasSelf, hopSelf, battPct, frameIdxH);

        // Parse neighbors: alias, hop, rssi (triplet mulai index 11)
        int offset = 11;
        for (int i = 0; i < neighCount; ++i) {
          if (offset + 2 >= fch) break;

          String nAlias = fh[offset++]; nAlias.trim();
          uint8_t nHop  = (uint8_t)fh[offset++].toInt();
          int nRssi     = fh[offset++].toInt();

          Id24 dstId = idForAlias(nAlias);
          updateLinkFromHello(srcId, dstId, nHop, (int16_t)nRssi, frameIdxH);

          // if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
          //   Serial.print("[SINK NEIGH] ...");
          // }
        }

        if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
          // Simpan frame HELLO terakhir yang terlihat.
          // Nanti dipakai sebagai label saat mencetak summary topology.
          _lastHelloSummaryFrameIdx = frameIdxH;
        }

        continue; // jangan diproses sebagai DATA
      }
    }

    // 2C) Data CSV
    const int MAXF = 40;   // buffer cukup besar, NUM_FIELDS <= 40
    String f[MAXF];
    int fc = 0;
    int start = 0;
    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        if (fc < MAXF) {
          f[fc++] = line.substring(start, i);
        }
        start = i + 1;
      }
    }

    // Minimal header yang wajib untuk analitik di Sink:
    // - sampai POS_IN_FRAME (IDX_POS_IN_FRAME)
    //   (proto, routeVer, srcId, parentAlias, path, destAlias, seq,
    //    tsOrigin, slotId, lat/lon/.../battPct, hopCount, netFlags,
    //    retryCount, backLogCount, frameIdx, posInFrame)
    const int MIN_FIELDS_SINK = IDX_POS_IN_FRAME + 1;
    if (fc < 8) {
      // if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
      //   Serial.println("[SINK] Drop: field count < 8");
      // }
      continue;
    }

    // Pastikan array String f[] memiliki panjang NUM_FIELDS.
    // Kalau paket yang diterima lebih pendek dari NUM_FIELDS,
    // field sisanya diisi "0". Ini menjaga kompatibilitas dengan
    // kode yang masih mengakses indeks lebih tinggi (footer analitik).
    if (fc < NUM_FIELDS) {
      for (int i = fc; i < NUM_FIELDS; ++i) {
        f[i] = "0";
      }
      fc = NUM_FIELDS;
    }

    // 3) Ambil field penting (pakai index dari Packet.h)
    String proto       = f[IDX_PROTO];        proto.trim();
    if (_hkRxData) _hkRxData(_hkUser, line.c_str());
    String routeVerS   = f[IDX_ROUTE_VER];    routeVerS.trim();
    String srcIdHex    = f[IDX_SRC_ID];       srcIdHex.trim();
    String parentAl    = f[IDX_PARENT_ALIAS]; parentAl.trim();
    String path        = f[IDX_PATH];         path.trim();
    String destAl      = f[IDX_DEST_ALIAS];   destAl.trim();
    String seqStr      = f[IDX_SEQ];          seqStr.trim();
    String tsOriginS   = f[IDX_TS_ORIGIN];    tsOriginS.trim();
    String slotIdS     = f[IDX_SLOT_ID];      slotIdS.trim();

    // ⬇️ payload sekarang 1 string bebas (bisa "lat|lon|msl|temp|humid|sos|batt")
    //    jaringan tidak meng-parse konten ini, hanya Sink yang opsional baca
    String payloadS    = f[IDX_PAYLOAD];      payloadS.trim();

    String hopCntS     = f[IDX_HOP_COUNT];    hopCntS.trim();
    String hopRssiS    = f[IDX_HOP_RSSI_SUM]; hopRssiS.trim();
    String hopSnrS     = f[IDX_HOP_SNR_SUM];  hopSnrS.trim();
    String frameIdxS   = f[IDX_FRAME_IDX];    frameIdxS.trim();
    String posInFrameS = f[IDX_POS_IN_FRAME]; posInFrameS.trim();
    String pktSizeS    = f[IDX_PACKET_SIZE];  pktSizeS.trim();

    Id24     srcId        = StaticRoute::parseHex24(srcIdHex);
    Id24     myId         = _id24;
    uint32_t tsOrigin     = (uint32_t)strtoul(tsOriginS.c_str(),   nullptr, 10);
    uint16_t seq          = (uint16_t)strtoul(seqStr.c_str(),      nullptr, 16);
    uint32_t frameIdxPkt  = (uint32_t)strtoul(frameIdxS.c_str(),   nullptr, 10);
    uint32_t posInFrame   = (uint32_t)strtoul(posInFrameS.c_str(), nullptr, 10);
    uint8_t  slotId       = (uint8_t)slotIdS.toInt();

    // ---------------------------------------------
    // Batt% dari payload (tidak diubah)
    // ---------------------------------------------
    int battPct = 0;
    int lastSep = payloadS.lastIndexOf('|');
    if (lastSep >= 0 && lastSep + 1 < payloadS.length()) {
      String battStr = payloadS.substring(lastSep + 1);
      battStr.trim();
      if (battStr.length() > 0) {
        battPct = battStr.toInt();
      }
    }

    // Distance(m) dari payload lat/lon (pakai sink position yang di-lock NetRuntime)
    float lat = 0.0f;
    float lon = 0.0f;

    // ambil token 0 dan 1 dari payloadS berbasis '|'
    int p0 = payloadS.indexOf('|');
    if (p0 > 0) {
      String latStr = payloadS.substring(0, p0);
      latStr.trim();
      lat = latStr.toFloat();

      int p1 = payloadS.indexOf('|', p0 + 1);
      if (p1 > p0 + 1) {
        String lonStr = payloadS.substring(p0 + 1, p1);
        lonStr.trim();
        lon = lonStr.toFloat();
      }
    }

    float distanceM = 0.0f;
    if (lat != 0.0f || lon != 0.0f) {
      distanceM = NetRuntime::distanceFromSinkMeters(lat, lon);
    }
    String distanceOut = String(distanceM, 1);

    // Waktu "sekarang" di Sink dari RTC (epoch UTC)
    // uint32_t nowTs  = nowUnixLike();

    // Alias Sink (diambil dari StaticRoute/NetRuntime)
    const char* myAliasC = StaticRoute::aliasForId(myId);
    String myAlias = myAliasC ? String(myAliasC) : String("S");

    // Sink hanya menerima paket jika destAlias menunjuk ke Sink
    if (destAl != myAlias) {
      continue;
    }

    // Opsional: cek bahwa parentAlias juga menunjuk ke Sink
    if (parentAl != myAlias) {
      continue;
    }

    // 4) Ambil alias child terakhir dari PATH (hop sebelum Sink)
    String childAlias;
    if (!extractChildAliasFromPath(path, childAlias)) {
      Serial.println("[SINK] Drop: path invalid");
      continue;
    }
    Id24 childId = idForAlias(childAlias);

    if (childId == 0) {
      Serial.println("[SINK] Drop: childId=0");
      continue;
    }

    // 5) Kirim ACK ke child
    {
      String dstIdHex = StaticRoute::hex24(childId);
      String ack = "ACK," + dstIdHex + "," + seqStr + "," + _idHex6;
      Lora.print(ack);
      Lora.print('\n');
    }

    // 6) e2eLatency dan avgLatHop
    // ---------------------------------------------
    // Pertama: coba hitung dari tsOrigin (epoch UTC) langsung
    uint32_t e2eLat = 0;
    if (tsOrigin > 0 && nowTs >= tsOrigin) {
      e2eLat = nowTs - tsOrigin;

      // Kalau terlalu besar, anggap invalid → reset ke 0 dulu
      const uint32_t MAX_REASONABLE_E2E = SlotPlan::superframeLenSec() * 5; // misal 5 superframe
      if (SlotPlan::superframeLenSec() > 0 && e2eLat > MAX_REASONABLE_E2E) {
        e2eLat = 0;
      }
    }

    // 7) hopCount + hopRssiSum + hopSnrSum
    // ---------------------------------------------
    // hopCount yang datang dari upstream (SEBELUM hop Sink ditambahkan)
    uint8_t hopCount       = (uint8_t)hopCntS.toInt();
    uint8_t hopCountUpstream = hopCount;

    // Kalau e2eLat masih 0 (RTC sinkron banget, kirim & terima di detik yang sama),
    // coba hitung lagi berdasarkan kombinasi (frameIdx, posInFrame) yang
    // berasal dari panjang frame (runtime config bila ada).
    if (e2eLat == 0) {
      uint32_t frameLenSec2 = SlotPlan::superframeLenSec();

      if (frameLenSec2 > 0) {
        uint32_t frameIdxNow = nowTs / frameLenSec2;
        uint8_t  posNowSec   = (uint8_t)(nowTs % frameLenSec2);

        uint32_t originAbs = frameIdxPkt * frameLenSec2
                           + (uint32_t)posInFrame;
        uint32_t nowAbs    = frameIdxNow * frameLenSec2
                           + (uint32_t)posNowSec;

        if (nowAbs >= originAbs) {
          e2eLat = nowAbs - originAbs;
        }
      }
    }

    // Kalau masih 0 juga, paksa minimal = jumlah hop (kasar),
    // supaya tidak pernah tercetak 0 detik.
    if (e2eLat == 0) {
      uint8_t hopsFinal = hopCountUpstream + 1;  // +1 karena hop ke Sink
      if (hopsFinal == 0) hopsFinal = 1;
      e2eLat = (uint32_t)hopsFinal;
    }

    // Sekarang lanjutkan akumulasi hop seperti biasa
    int32_t  hopRssiSum = (int32_t)strtol(hopRssiS.c_str(), nullptr, 10);
    float    hopSnrSum  = hopSnrS.toFloat();

    // mulai dari nilai upstream, lalu tambahkan hop Sink
    hopCount = hopCountUpstream;
    hopCount++;
    hopRssiSum += rssi_dBm;

    float avgLatHop = 0.0f;
    if (hopCount > 0 && e2eLat > 0) {
      avgLatHop = (float)e2eLat / (float)hopCount;
    }

    // 8) avgRssi, SNR, link margin 
    float avgRssi = 0.0f;
    if (hopCount > 0) {
      avgRssi = (float)hopRssiSum / (float)hopCount;
    } else {
      avgRssi = (float)rssi_dBm;
    }

    float snrHopLast = (float)rssi_dBm - DataWire::NOISE_FLOOR_DBM;
    hopSnrSum += snrHopLast;

    float avgSnr = 0.0f;
    if (hopCount > 0) {
      avgSnr = hopSnrSum / (float)hopCount;
    }

    float linkMargin = avgRssi - RX_SENSITIVITY_DBM;

    // 9) packet size + throughput
    int packetSizeBytes = line.length();
    float throughput = 0.0f;
    if (e2eLat > 0) {
      throughput = (float)(packetSizeBytes * 8) / (float)e2eLat; // bit/s
    }

    // 10) packet loss (berdasarkan gap sequence) -> field CSV PACKET_LOSS
    uint32_t packetLoss = 0;
    {
      PktLossTracker* lt = ensureLossTracker(srcId);
      if (lt) {
        if (lt->lastSeq != 0) {
          uint16_t expected = lt->lastSeq + 1;
          if (seq != expected) {
            if (seq > lt->lastSeq) {
              lt->lost += (uint32_t)(seq - lt->lastSeq - 1);
            }
          }
        }
        lt->lastSeq = seq;
        packetLoss  = lt->lost;
      }
    }

    // 11) Update field footer CSV
    f[IDX_HOP_COUNT]     = String((unsigned)hopCount);
    // netFlags, retryCount, backLogCount tetap dari upstream
    f[IDX_E2E_LATENCY]   = String((unsigned long)e2eLat);
    f[IDX_AVG_LAT_HOP]   = String(avgLatHop, 3);
    f[IDX_HOP_RSSI_SUM]  = String((long)hopRssiSum);
    f[IDX_AVG_RSSI]      = String(avgRssi, 1);
    f[IDX_HOP_SNR_SUM]   = String(hopSnrSum, 2);
    f[IDX_AVG_SNR]       = String(avgSnr, 1);
    f[IDX_THROUGHPUT]    = String(throughput, 1);
    f[IDX_LINK_MARGIN]   = String(linkMargin, 1);
    f[IDX_PACKET_LOSS]   = String((unsigned long)packetLoss);
    f[IDX_PACKET_SIZE]   = String(packetSizeBytes);

    // 12) Bangun string RAW dengan format:
    // <proto>,<routeVer>,<srcId>,<parentAlias>,<path>,<destAlias>,<seq>,
    // <tsOrigin>,<slotId>,<payload>,<hopCount>,<netFlags>,<retryCount>,
    // <backlogCount>,<frameIdx>,<posInFrame>,<e2eLatency>,<avgLatHop>,
    // <hopRssiSum>,<avgRssi>,<hopSnrSum>,<avgSnr>,<throughput>,<linkMargin>,
    // <packetLoss>,<packetSize>
    //
    // payload diambil dari IDX_PAYLOAD, tapi semua '|' diganti spasi
    // supaya tidak ada karakter '|' di output.
    String protoOut      = f[IDX_PROTO];        protoOut.trim();
    String routeVerOut   = f[IDX_ROUTE_VER];    routeVerOut.trim();
    String srcIdOut      = f[IDX_SRC_ID];       srcIdOut.trim();
    String parentAlOut   = f[IDX_PARENT_ALIAS]; parentAlOut.trim();
    String pathOut       = f[IDX_PATH];         pathOut.trim();
    String destAlOut     = f[IDX_DEST_ALIAS];   destAlOut.trim();
    String seqOut        = f[IDX_SEQ];          seqOut.trim();
    String tsOriginOut   = f[IDX_TS_ORIGIN];    tsOriginOut.trim();
    String slotIdOut     = f[IDX_SLOT_ID];      slotIdOut.trim();

    String payloadOut    = payloadS;
    payloadOut.replace('|', ',');  // HAPUS semua '|' di output raw

    String hopCountOut     = f[IDX_HOP_COUNT];     hopCountOut.trim();
    String netFlagsOut     = f[IDX_NET_FLAGS];     netFlagsOut.trim();
    String retryCountOut   = f[IDX_RETRY_COUNT];   retryCountOut.trim();
    String backlogCountOut = f[IDX_BACKLOG_COUNT]; backlogCountOut.trim();
    String frameIdxOut     = f[IDX_FRAME_IDX];     frameIdxOut.trim();
    String posInFrameOut   = f[IDX_POS_IN_FRAME];  posInFrameOut.trim();
    String e2eLatOut       = f[IDX_E2E_LATENCY];   e2eLatOut.trim();
    String avgLatHopOut    = f[IDX_AVG_LAT_HOP];   avgLatHopOut.trim();
    String hopRssiSumOut   = f[IDX_HOP_RSSI_SUM];  hopRssiSumOut.trim();
    String avgRssiOut      = f[IDX_AVG_RSSI];      avgRssiOut.trim();
    String hopSnrSumOut    = f[IDX_HOP_SNR_SUM];   hopSnrSumOut.trim();
    String avgSnrOut       = f[IDX_AVG_SNR];       avgSnrOut.trim();
    String throughputOut   = f[IDX_THROUGHPUT];    throughputOut.trim();
    String linkMarginOut   = f[IDX_LINK_MARGIN];   linkMarginOut.trim();
    String packetLossOut   = f[IDX_PACKET_LOSS];   packetLossOut.trim();
    String packetSizeOut   = f[IDX_PACKET_SIZE];   packetSizeOut.trim();

    String rawLine;
    rawLine.reserve(160);

    rawLine  = protoOut;
    rawLine += ','; rawLine += routeVerOut;
    rawLine += ','; rawLine += srcIdOut;
    rawLine += ','; rawLine += parentAlOut;
    rawLine += ','; rawLine += pathOut;
    rawLine += ','; rawLine += destAlOut;
    rawLine += ','; rawLine += seqOut;
    rawLine += ','; rawLine += tsOriginOut;
    rawLine += ','; rawLine += slotIdOut;
    rawLine += ','; rawLine += payloadOut;
    rawLine += ','; rawLine += distanceOut;
    rawLine += ','; rawLine += hopCountOut;
    rawLine += ','; rawLine += netFlagsOut;
    rawLine += ','; rawLine += retryCountOut;
    rawLine += ','; rawLine += backlogCountOut;
    rawLine += ','; rawLine += frameIdxOut;
    rawLine += ','; rawLine += posInFrameOut;
    rawLine += ','; rawLine += e2eLatOut;
    rawLine += ','; rawLine += avgLatHopOut;
    rawLine += ','; rawLine += hopRssiSumOut;
    rawLine += ','; rawLine += avgRssiOut;
    rawLine += ','; rawLine += hopSnrSumOut;
    rawLine += ','; rawLine += avgSnrOut;
    rawLine += ','; rawLine += throughputOut;
    rawLine += ','; rawLine += linkMarginOut;
    rawLine += ','; rawLine += packetLossOut;
    rawLine += ','; rawLine += packetSizeOut;

    // 13) RAW_ONLY: cuma print string final, tanpa debug lain
    if (SINK_PRINT_MODE == SinkPrintMode::RAW_ONLY) {
      Serial.print('@');
      Serial.print(rawLine);
      Serial.println('#');

      String framed = "@";
      framed += rawLine;
      framed += "#";
      sinkRawLineMirror(framed.c_str());
    }
    
    // 14) ANALYZED_ONLY: tracking per-frame untuk summary DATA
    //     - Summary DATA dicetak:
    //         * Saat ganti frame (frameIdxPkt > gCurrentFrameIdx), ATAU
    //         * Saat menerima paket dari slot data terakhir pada frame ini.
    //     - Summary HELLO tidak lagi dicetak di sini, tapi di-schedule
    //       terpisah di awal loop() setelah window HELLO selesai.
    if (SINK_PRINT_MODE == SinkPrintMode::ANALYZED_ONLY) {
      // Frame boundary:
      if (!gHaveFrame) {
        gHaveFrame       = true;
        gCurrentFrameIdx = frameIdxPkt;
        clearFrameStats();
      }
      else if (frameIdxPkt > gCurrentFrameIdx) {
        // Frame lama selesai, flush summary DATA
        flushFrameSummary(gCurrentFrameIdx, nowTs);
        clearFrameStats();
        gCurrentFrameIdx = frameIdxPkt;
      }
      else if (frameIdxPkt < gCurrentFrameIdx) {
        // Paket dari frame lama → optional: boleh tetap proses
        // tapi JANGAN mengubah gCurrentFrameIdx dan JANGAN clearFrameStats.
        // Untuk sementara kita biarkan saja supaya metrik tetap konsisten.
      }

      // Ever seen & frame stats untuk srcId
      if (EverSeenNode* e = ensureEverSeen(srcId)) {
        e->pkts += 1;
      }

      if (PerNodeFrameStats* st = ensureFrameStats(srcId, frameIdxPkt)) {
        st->count          += 1;
        st->lastHopCount    = hopCount;
        st->lastE2ELat      = e2eLat;
        st->lastAvgLatHop   = avgLatHop;
        st->lastAvgRssi     = avgRssi;
        st->lastAvgSnr      = avgSnr;
        st->lastThroughput  = throughput;
        st->lastLinkMargin  = linkMargin;
        st->lastBattPct     = battPct;
        st->lastPktSize     = packetSizeBytes;
        st->lastDistanceM   = distanceM;
        st->lastPath        = path;
      }

      // Kalau paket ini datang dari slot terakhir frame → saatnya flush summary DATA
      // Kita ambil lastSlotId dari runtime config kalau ada, kalau tidak pakai
      // konstanta LAST_DATA_SLOT_ID dari NetConfig.h
      uint8_t lastSlotId = NetTopology::LAST_DATA_SLOT_ID;
      if (NetRuntime::hasConfig()) {
        const auto& cfg = NetRuntime::current();
        if (cfg.lastDataSlotId != 0) {
          lastSlotId = cfg.lastDataSlotId;
        }
      }

      if (slotId == lastSlotId &&
          gHaveFrame &&
          frameIdxPkt == gCurrentFrameIdx)
      {
        // Flush summary DATA frame
        flushFrameSummary(gCurrentFrameIdx, nowTs);

        // Reset state frame summary DATA
        clearFrameStats();
        gHaveFrame = false;
      }
    }
    blinkStatus();
  }
}
