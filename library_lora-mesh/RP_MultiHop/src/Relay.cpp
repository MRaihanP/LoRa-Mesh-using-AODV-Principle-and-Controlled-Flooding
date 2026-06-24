// ======================================================
// 1. INCLUDE SECTION
// ======================================================
#include "Relay.h"
#include "Packet.h"
#include "NetTime.h"
#include "NetRuntimeConfig.h"
#include "FramePlan.h"

using namespace StaticRoute;
using namespace DataWire;

// ======================================================
// 2. LOCAL STATIC HELPERS (PURE FUNCTIONS)
// ======================================================

// Check if alias exists inside path list separated by '-'
static bool aliasInPath(const String& path, const String& alias) {
  if (!path.length() || !alias.length()) return false;

  int start = 0;
  const int len = path.length();

  for (int i = 0; i <= len; ++i) {
    if (i == len || path[i] == '-') {
      String part = path.substring(start, i);
      part.trim();
      if (part.length() && part == alias) return true;
      start = i + 1;
    }
  }
  return false;
}

// Data window lookup (runtime config → fallback static slots)
static bool findDataWindowForSource(StaticRoute::Id24 srcId,
                                    uint8_t& startSecOut,
                                    uint8_t& endSecOut) {
  if (NetRuntime::hasConfig() && NetRuntime::current().dataSlotCount > 0) {
    const auto& cfg = NetRuntime::current();
    for (size_t i = 0; i < cfg.dataSlotCount; ++i) {
      const auto& ds = cfg.dataSlots[i];
      if (ds.ownerId == srcId) {
        startSecOut = ds.startSec;
        endSecOut   = (uint8_t)((ds.startSec + ds.durationSec) % SlotPlan::frameLenSec());
        return true;
      }
    }
  } else {
    const SlotPlan::SlotInfo* si = SlotPlan::findSlotFallback(srcId);
    if (si) {
      startSecOut = si->startSec;
      endSecOut   = si->endSec;
      return true;
    }
  }
  return false;
}

// Check if current position is inside tail region of source data window
static bool isInTailOfDataWindow(StaticRoute::Id24 srcId,
                                 uint32_t unixNow,
                                 uint8_t tailSec) {
  if (tailSec == 0) return false;

  uint32_t frameLen = SlotPlan::frameLenSec();
  if (frameLen == 0) return false;

  uint8_t pos      = (uint8_t)(unixNow % frameLen);
  uint8_t startSec = 0;
  uint8_t endSec   = 0;

  if (!findDataWindowForSource(srcId, startSec, endSec)) return false;

  bool wrapped = endSec <= startSec;
  uint8_t tailStart;

  if (!wrapped) {
    if (pos < startSec || pos >= endSec) return false;
    tailStart = (uint8_t)(endSec - tailSec);
    if (tailStart < startSec) tailStart = startSec;
  } else {
    bool inWindow = (pos >= startSec || pos < endSec);
    if (!inWindow) return false;
    if (pos >= startSec) {
      tailStart = (uint8_t)((endSec + frameLen - tailSec) % frameLen);
    } else {
      tailStart = (uint8_t)(endSec - tailSec);
    }
  }

  return pos >= tailStart;
}

// ======================================================
// 3. CONSTRUCTOR
// ======================================================

MeshRelay::MeshRelay(const BoardPins& pins)
: MeshDeviceBase("RELAY", pins),

  // IDENTITY & ROUTING STATE
  _id24(0),
  _parentId24(0),
  _hopToSink(255),
  _alias(""),
  _parentAlias(""),
  _slotIndex(-1),

  // PARENT RELIABILITY STATE
  _parentAckFailStreak(0),
  _parentLastRssi(0),
  _parentDegradeScore(0.0f),
  _parentConfirmed(false),
  _failedParentCount(0),
  _lastRouteEvalFrameIdx(0),
  _routeDirty(false),

  // FAST SELF-HEALING STATE
  _parentSuspected(false),
  _brokenParentPending(false),
  _healingFromParent(0),
  _brokenParentId24(0),
  _brokenParentUntilFrameIdx(0),

  // TX CONTROL
  _txBusy(false),
  _lastTxStartMs(0),
  _lastTxMs(0),

  // ORIGIN SELF TX
  _localSeq(1),
  _selfSentThisFrame(false),
  _selfLastFrameIdx(0),
  _selfLastSeqHex(""),

  // FORWARD QUEUE
  _head(0),
  _tail(0),
  _count(0),
  _inFlight(false),
  _retry(0),
  _lastFwdTxMs(0),
  _nextForwardAllowedMs(0),

  // BACKLOG QUEUE
  _seenFwdPos(0),
  _bHead(0),
  _bTail(0),
  _bCount(0),
  _backlogInFlight(false),
  _backlogRetry(0),
  _backlogAckStartMs(0),
  _backlogFrameIdx(0),
  _nextBacklogFrameIdxAllowed(0),
  _backlogSentThisFrame(false),
  _backlogSeqInFlight(""),

  // HELLO / ROUTE DISCOVERY
  _helloSentThisFrame(false),
  _helloSeq(0),
  _lastHelloFrameIdx(0),
  _wasInHelloPhase(false),
  _neighborCollectionFrameIdx(0),

  // CALLBACKS
  _payloadBuilder(nullptr),
  _txCb(nullptr),
  _ackCb(nullptr),
  _fwdRxCb(nullptr)
{}

// ======================================================
// 4. LIFECYCLE CORE
// ======================================================

void MeshRelay::begin() {
  MeshDeviceBase::begin();
  initLoraUart();

  Id::begin();
  _id24   = Id::id24();
  _idHex6 = Id::idHex6();

  const char* aliasC = StaticRoute::aliasForId(_id24);
  _alias = aliasC ? String(aliasC) : String("UNK");
  _slotIndex = SlotPlan::slotIdFor(_id24);

  // Routing bootstrap – always start dynamic
  _parentId24   = 0;
  _parentHex6   = "";
  _parentAlias  = "";

  _parentConfirmed  = false;
  _hopToSink        = 255;
  _failedParentCount = 0;

  clearNeighbors();

  _wasInHelloPhase             = false;
  _neighborCollectionFrameIdx  = 0;
  _nextForwardAllowedMs        = millis();
  _nextBacklogFrameIdxAllowed  = 0;
  _backlogInFlight             = false;
  _backlogRetry                = 0;

  Serial.printf("[RELAY] ID=%s, dynamic bootstrap\n", _idHex6.c_str());
}

// ======================================================
// 5. TX CONTROL LAYER
// ======================================================

bool MeshRelay::isDataTxPhase() const {
  uint32_t unixNow = nowUnixLike();
  uint32_t frameIdxDummy = 0;

  // block DATA during HELLO phase
  if (SlotPlan::isHelloPhase(unixNow))
    return false;

  // allow only within assigned TX window
  return SlotPlan::isInMyTxWindow(_id24, unixNow, frameIdxDummy);
}

bool MeshRelay::canTransmitNow() const {
  if (millis() - _lastTxMs < MIN_TX_GAP_MS) return false;
  return true;
}

bool MeshRelay::requestTx() {
  uint32_t now = millis();
  if (_txBusy) {
    if (now - _lastTxStartMs < TX_BUSY_TIMEOUT_MS) return false;
    _txBusy = false;
  }
  _txBusy = true;
  _lastTxStartMs = now;
  return true;
}

void MeshRelay::releaseTx() {
  _txBusy = false;
}

// ======================================================
// 6. MAIN LOOP
// ======================================================

void MeshRelay::loop() {
  static uint32_t _lastAnyTxGuardMs = 0;
  static constexpr uint32_t TX_GLOBAL_GAP_MS = 120;

  static const uint32_t ACK_TIMEOUT_MS_RELAY = NetReliability::RELAY_ACK_TIMEOUT_MS;
  static const uint8_t  MAX_RETRY_RELAY      = NetReliability::RELAY_MAX_RETRY;

  static const uint32_t ACK_TIMEOUT_MS_FWD   = NetReliability::FWD_ACK_TIMEOUT_MS;
  static const uint8_t  MAX_RETRY_FWD        = NetReliability::FWD_MAX_RETRY;
  static const uint32_t TX_GAP_MS_FWD        = NetReliability::FWD_TX_GAP_MS;
  static const uint32_t FORWARD_GUARD_MS     = NetReliability::RELAY_FORWARD_GUARD_MS;

  static String rxBuf;
  uint32_t nowMs = millis();

  // =========================
  // 1) RX: kumpulkan byte dari LoRa UART
  // =========================
  while (Lora.available()) {
    char c = (char)Lora.read();
    rxBuf += c;
    if (rxBuf.length() > 512) {
      Serial.print("[RXBUF] HARD overflow len=");
      Serial.println(rxBuf.length());
      rxBuf = "";
    }
  }

  // Proses per paket: "<payload>\n" + "<RSSI>"
  while (true) {
    int nlIdx = rxBuf.indexOf('\n');
    if (nlIdx < 0) break;
    if (nlIdx + 1 >= (int)rxBuf.length()) break;

    uint8_t rssiByte = (uint8_t)rxBuf[nlIdx + 1];
    String  line     = rxBuf.substring(0, nlIdx);
    rxBuf.remove(0, nlIdx + 2);

    line.trim();
    if (!line.length()) continue;

    int rssi_dBm;
    if (rssiByte == 0 || rssiByte == 255) {
      rssi_dBm = 0;
    } else {
      rssi_dBm = (int)rssiByte - 256;
    }

    // =========================
    // 1A) ACK frame
    // =========================
    if (line.startsWith("ACK,")) {
      const int MAXF_ACK = 8;
      String fAck[MAXF_ACK];
      int fcAck = 0, s = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          if (fcAck < MAXF_ACK) fAck[fcAck++] = line.substring(s, i);
          s = i + 1;
        }
      }
      if (fcAck < 4) continue;

      String dst    = fAck[1]; dst.trim();
      String seqStr = fAck[2]; seqStr.trim();
      String fromId = fAck[3]; fromId.trim();

      // =====================================
      // FORWARD QUEUE ACK HANDLER
      // =====================================
      if (_inFlight && _count > 0) {
        QueueEntry &E = _q[_head];

        bool fromCurrentParent =
          (_parentId24 != 0) &&
          fromId.equalsIgnoreCase(_parentHex6);

        bool ownershipValid = false;
        {
          const int MAXF_VERIFY = NUM_FIELDS;
          String fv[MAXF_VERIFY];
          int fcv = 0, stv = 0;
          for (int i = 0; i <= E.frame.length(); ++i) {
            if (i == E.frame.length() || E.frame[i] == ',') {
              if (fcv < MAXF_VERIFY) fv[fcv++] = E.frame.substring(stv, i);
              stv = i + 1;
            }
          }
          if (fcv > IDX_SRC_ID) {
            ownershipValid = seqStr.equalsIgnoreCase(E.seqHex);
          }
        }

        if (fromCurrentParent && ownershipValid) {
          Serial.print("[RELAY-FWD ACK OK] seq=");
          Serial.println(seqStr);

          // update neighbor ack stats
          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
              _neighbors[i].stats.ackCount++;
              break;
            }
          }

          if (_parentAckFailStreak > 0) _parentAckFailStreak--;

          // ACK berhasil saat healing → blacklist parent lama
          if (_parentSuspected &&
              _healingFromParent != 0 &&
              _healingFromParent != _parentId24) {
            _brokenParentId24    = _healingFromParent;
            _brokenParentPending = true;
            Serial.printf("[RELAY HEAL ACK OK] blacklist pending parent=%s -> using=%s\n",
                          StaticRoute::aliasForId(_healingFromParent),
                          _parentAlias.length() ? _parentAlias.c_str() : "?");
          }
          _parentSuspected   = false;
          _healingFromParent = 0;

          uint16_t ackSeq = (uint16_t)strtoul(seqStr.c_str(), nullptr, 16);
          if (_ackCb) _ackCb(ackSeq);

          _head = (_head + 1) % QSIZE;
          _count--;
          _inFlight = false;
          _retry    = 0;
        }
      }

      // =====================================
      // BACKLOG ACK HANDLER
      // =====================================
      bool backlogAckFromParent =
        (_parentId24 != 0) &&
        fromId.equalsIgnoreCase(_parentHex6);

      if (_backlogInFlight &&
          backlogAckFromParent &&
          seqStr.equalsIgnoreCase(_backlogSeqInFlight)) {
        Serial.print("[RELAY-BACKLOG ACK OK] seq=");
        Serial.println(seqStr);

        _bHead = (_bHead + 1) % BQSIZE;
        _bCount--;
        _backlogInFlight    = false;
        _backlogRetry       = 0;
        _backlogSeqInFlight = "";

        uint16_t ackSeq = (uint16_t)strtoul(seqStr.c_str(), nullptr, 16);
        if (_ackCb) _ackCb(ackSeq);
      }

      // =====================================
      // SELF ORIGIN ACK HANDLER
      // =====================================
      // ACK untuk paket yang dikirim Relay itu sendiri (bukan forward child)
      bool selfAckFromParent =
        (_parentId24 != 0) &&
        fromId.equalsIgnoreCase(_parentHex6);

      if (selfAckFromParent &&
          _selfLastSeqHex.length() > 0 &&
          dst.equalsIgnoreCase(_idHex6) &&
          seqStr.equalsIgnoreCase(_selfLastSeqHex)) {

        Serial.print("[RELAY SELF ACK OK] seq=");
        Serial.println(seqStr);

        // Update link stats
        for (int i = 0; i < MAX_NEIGHBORS; ++i) {
          if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
            _neighbors[i].stats.txCount++;
            _neighbors[i].stats.ackCount++;
            break;
          }
        }

        if (_parentAckFailStreak > 0) _parentAckFailStreak--;

        // Healing: jika sebelumnya failover, blacklist parent lama
        if (_parentSuspected &&
            _healingFromParent != 0 &&
            _healingFromParent != _parentId24) {
          _brokenParentId24    = _healingFromParent;
          _brokenParentPending = true;
          Serial.printf("[RELAY HEAL SELF ACK OK] blacklist pending parent=%s -> using=%s\n",
                        StaticRoute::aliasForId(_healingFromParent),
                        _parentAlias.length() ? _parentAlias.c_str() : "?");
        }
        _parentSuspected   = false;
        _healingFromParent = 0;

        uint16_t ackSeq = (uint16_t)strtoul(seqStr.c_str(), nullptr, 16);
        if (_ackCb) _ackCb(ackSeq);

        _selfLastSeqHex = ""; // clear agar tidak double-trigger
      }

      continue;
    }

    // =========================
    // 1B) ROUTE_HELLO (gossip topologi)
    // =========================
    if (line.startsWith(MeshProto::ROUTE_HELLO)) {
      // Layout HELLO:
      //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
      //  4=parentAlias, 5=hopToSink, 6=frameIdx, 7=posInFrame,
      //  8=battPct, 9=role, 10=neighCount, 11..= triplet neighbors
      const int MAXF_HELLO = 32;
      String fh[MAXF_HELLO];
      int fch = 0, stH = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          if (fch < MAXF_HELLO) fh[fch++] = line.substring(stH, i);
          stH = i + 1;
        }
      }
      if (fch < 11) {
        Serial.print("[RELAY HELLO] Drop: field < 11 | line=");
        Serial.println(line);
        continue;
      }

      String srcIdHex    = fh[2]; srcIdHex.trim();
      String aliasSelf   = fh[3]; aliasSelf.trim();
      String parentAlias = fh[4]; parentAlias.trim();
      uint8_t  hopToSink = (uint8_t)fh[5].toInt();
      uint32_t frameIdxH = (uint32_t)fh[6].toInt();

      NetRuntime::DeviceRole advertisedRole =
        (NetRuntime::DeviceRole)fh[9].toInt();

      uint8_t neighCount = (uint8_t)fh[10].toInt();

      // Frame index lokal Relay saat HELLO diterima
      uint32_t frameIdxLocal = 0;
      {
        uint32_t unixNowLocal = nowUnixLike();
        bool dummy = SlotPlan::isInMyTxWindow(_id24, unixNowLocal, frameIdxLocal);
        (void)dummy;
      }

      StaticRoute::Id24 nid = StaticRoute::idForAlias(aliasSelf);

      if (nid != 0 && nid != _id24) {
        updateNeighborFromHello(
          nid, aliasSelf, advertisedRole,
          hopToSink, (int16_t)rssi_dBm, frameIdxLocal
        );

        Serial.print("[RELAY HELLO RX] from=");
        Serial.print(aliasSelf);
        Serial.print(" hop=");
        Serial.print(hopToSink);
        Serial.print(" rssi=");
        Serial.print(rssi_dBm);
        Serial.print(" frameH=");
        Serial.print(frameIdxH);
        Serial.print(" localF=");
        Serial.println(frameIdxLocal);

        debugPrintNeighbors();
      }

      // Proses gossip tetangga (triplet: alias, hop, rssi mulai index 11)
      int offset = 11;
      for (int gi = 0; gi < neighCount; ++gi) {
        if (offset + 2 >= fch) break;

        String gAlias = fh[offset++]; gAlias.trim();
        uint8_t gHop  = (uint8_t)fh[offset++].toInt();
        int16_t gRssi = (int16_t)fh[offset++].toInt();

        StaticRoute::Id24 gId = StaticRoute::idForAlias(gAlias);
        if (gId == 0 || gId == _id24 || gHop >= 250) continue;

        bool alreadyKnown = false;
        for (int k = 0; k < MAX_NEIGHBORS; ++k) {
          if (_neighbors[k].used && _neighbors[k].id24 == gId) {
            alreadyKnown = true;
            break;
          }
        }
        if (!alreadyKnown) {
          updateNeighborFromHello(
            gId, gAlias,
            NetRuntime::DeviceRole::NODE, // role tidak diketahui via gossip
            gHop, 0,                      // rssi tidak diukur langsung
            frameIdxLocal
          );
        }
      }

      continue;
    }

    // =========================
    // 1C) Data CSV (paket dari child yang harus di-forward)
    // =========================
    const int MAXF = NUM_FIELDS;
    String f[MAXF];
    int fc = 0, st = 0;

    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        if (fc < MAXF) f[fc++] = line.substring(st, i);
        st = i + 1;
      }
    }

    // Debug field parser
    Serial.println();
    Serial.println("========== RX FIELD DEBUG ==========");
    Serial.print("LEN = "); Serial.println(line.length());
    Serial.print("RAW = "); Serial.println(line);
    Serial.print("FIELDS = "); Serial.println(fc);
    for (int i = 0; i < fc; ++i) {
      Serial.print("["); Serial.print(i); Serial.print("] => ");
      Serial.println(f[i]);
    }
    Serial.println("=====================================");

    // Validasi field minimum wajib
    const int MIN_FIELDS_RELAY = 14;
    if (fc < MIN_FIELDS_RELAY) {
      Serial.print("[RELAY] INVALID HEADER fc=");
      Serial.println(fc);
      continue;
    }

    // Pastikan field forwarding tersedia
    while (fc <= IDX_HOP_SNR_SUM) {
      f[fc++] = "0";
    }

    // PATH init jika kosong
    if (f[IDX_PATH].length() == 0) {
      const char* srcAliasC =
        StaticRoute::aliasForId(StaticRoute::parseHex24(f[IDX_SRC_ID]));
      if (srcAliasC) f[IDX_PATH] = String(srcAliasC);
    }

    if (f[IDX_HOP_COUNT].length()    == 0) f[IDX_HOP_COUNT]    = "0";
    if (f[IDX_HOP_RSSI_SUM].length() == 0) f[IDX_HOP_RSSI_SUM] = "0";
    if (f[IDX_HOP_SNR_SUM].length()  == 0) f[IDX_HOP_SNR_SUM]  = "0";

    String proto     = f[IDX_PROTO];        proto.trim();
    String srcIdHex  = f[IDX_SRC_ID];       srcIdHex.trim();
    String seqHex    = f[IDX_SEQ];          seqHex.trim();
    String tsOriginS = f[IDX_TS_ORIGIN];    tsOriginS.trim();
    String hopCntS   = f[IDX_HOP_COUNT];    hopCntS.trim();
    String hopRssiS  = f[IDX_HOP_RSSI_SUM]; hopRssiS.trim();
    String hopSnrS   = f[IDX_HOP_SNR_SUM];  hopSnrS.trim();
    String path      = f[IDX_PATH];         path.trim();
    String parentAl  = f[IDX_PARENT_ALIAS]; parentAl.trim();
    String destAl    = f[IDX_DEST_ALIAS];   destAl.trim();

    const char* myAliasC = StaticRoute::aliasForId(_id24);
    String myAlias = myAliasC ? String(myAliasC) : String("");

    // Tracking origin untuk logika backlog
    uint32_t frameIdxRx   = 0;
    uint8_t  posInFrameRx = 0;
    if (fc > IDX_FRAME_IDX) {
      String frS = f[IDX_FRAME_IDX]; frS.trim();
      frameIdxRx = (uint32_t)frS.toInt();
    }
    if (fc > IDX_POS_IN_FRAME) {
      String posS = f[IDX_POS_IN_FRAME]; posS.trim();
      posInFrameRx = (uint8_t)posS.toInt();
    }

    StaticRoute::Id24 srcId24 = StaticRoute::parseHex24(srcIdHex);
    onOriginHeard(srcId24, seqHex, frameIdxRx, posInFrameRx);

    // Relay hanya memproses paket yang parentAlias == alias dirinya
    if (!myAlias.length() || parentAl != myAlias) {
      continue;
    }

    // Duplicate suppression
    if (alreadyForwarded(srcIdHex, seqHex)) {
      Serial.print("[FWD DUP DROP] ");
      Serial.print(srcIdHex);
      Serial.print(" seq=");
      Serial.println(seqHex);
      continue;
    }
    markForwarded(srcIdHex, seqHex);

    // Cari alias hop terakhir dari PATH (child langsung)
    int lastDash     = path.lastIndexOf('-');
    String lastAlias = (lastDash < 0) ? path : path.substring(lastDash + 1);
    lastAlias.trim();

    StaticRoute::Id24 childId = StaticRoute::idForAlias(lastAlias);

    // Update hop metrics
    uint8_t  hopCount   = (uint8_t) hopCntS.toInt();
    int32_t  hopRssiSum = (int32_t) strtol(hopRssiS.c_str(), nullptr, 10);
    float    hopSnrSum  = hopSnrS.toFloat();

    hopCount++;
    hopRssiSum += rssi_dBm;
    float snrHop = (float)rssi_dBm - NOISE_FLOOR_DBM;
    hopSnrSum += snrHop;

    f[IDX_HOP_COUNT]    = String((unsigned)hopCount);
    f[IDX_HOP_RSSI_SUM] = String((long)hopRssiSum);
    f[IDX_HOP_SNR_SUM]  = String(hopSnrSum, 2);

    // Update proto
    if (proto == MeshProto::DATA_NODE_ORIGIN ||
        proto == MeshProto::DATA_RELAY_SELF) {
      proto = MeshProto::DATA_NODE_VIA;
    }
    f[IDX_PROTO] = proto;

    // Append alias Relay ke PATH
    if (path.length() == 0) {
      path = myAlias;
    } else {
      path += '-';
      path += myAlias;
    }
    f[IDX_PATH] = path;

    // Tentukan next-hop dengan anti-loop berbasis PATH
    StaticRoute::Id24 nextHopId = _parentId24;
    if (path.length() && _parentId24 != 0) {
      // Anti-loop: jika parent ada di PATH, cari alternatif
      const char* parentAliasC = StaticRoute::aliasForId(_parentId24);
      if (parentAliasC && aliasInPath(path, String(parentAliasC))) {
        StaticRoute::Id24 altId = findAlternateParentExcluding(_parentId24);
        if (altId != 0) {
          nextHopId = altId;
        } else {
          Serial.print("[RELAY ANTILOOP] Drop frame seq=");
          Serial.print(seqHex);
          Serial.print(" path=");
          Serial.println(path);
          continue;
        }
      }
    }

    if (nextHopId == 0) {
      Serial.print("[RELAY ANTILOOP] Drop frame: no next-hop, seq=");
      Serial.println(seqHex);
      continue;
    }

    const char* parentAliasNext = StaticRoute::aliasForId(nextHopId);
    if (parentAliasNext && parentAliasNext[0] != '\0') {
      f[IDX_PARENT_ALIAS] = String(parentAliasNext);
    } else {
      Serial.print("[RELAY ANTILOOP] Drop frame: next-hop alias empty, seq=");
      Serial.println(seqHex);
      continue;
    }

    // ROOT mode: parentId24 == 0 → Relay berlaku sebagai sink
    if (_parentId24 == 0) {
      if (childId != 0) {
        String childHex = StaticRoute::hex24(childId);
        String ack = "ACK," + childHex + "," + seqHex + "," + _idHex6;
        delay(random(20, 80));
        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return;
          }
          Lora.print(ack);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }
        Serial.print("[RELAY(root)] ACK child seq=");
        Serial.print(seqHex);
        Serial.print(" to ");
        Serial.println(childHex);
      }
      Serial.print("[RELAY(root) DATA RX] ");
      Serial.println(line);
      blinkStatus();
      continue;
    }

    // destAlias == alias Relay → Relay adalah final dest
    if (destAl == myAlias) {
      if (childId != 0) {
        String childHex = StaticRoute::hex24(childId);
        String ack = "ACK," + childHex + "," + seqHex + "," + _idHex6;
        delay(random(20, 80));
        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return;
          }
          Lora.print(ack);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }
        Serial.print("[RELAY FINAL] ACK child seq=");
        Serial.print(seqHex);
        Serial.print(" to ");
        Serial.println(childHex);
      }
      blinkStatus();
      continue;
    }

    // Kirim ACK ke child
    if (childId != 0) {
      String childHex = StaticRoute::hex24(childId);
      String ack = "ACK," + childHex + "," + seqHex + "," + _idHex6;
      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return;
        }
        Lora.print(ack);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _lastAnyTxGuardMs = millis();
      }
      Serial.print("[RELAY-FWD] ACK child seq=");
      Serial.print(seqHex);
      Serial.print(" to ");
      Serial.println(childHex);
    }

    // Enqueue ke parent
    String frame = joinCsv(f, fc);
    enqueueUp(frame, seqHex);

    {
      uint16_t seq16 = (uint16_t)strtoul(seqHex.c_str(), nullptr, 16);
      if (_fwdRxCb) _fwdRxCb((uint32_t)childId, seq16);
    }

    _nextForwardAllowedMs = nowMs + FORWARD_GUARD_MS;

    Serial.print("[RELAY-FWD] Enqueue up from childAlias=");
    Serial.print(lastAlias);
    Serial.print(" seq=");
    Serial.println(seqHex);

    blinkStatus();
  } // end RX while

  // =========================
  // 2) TX: forward queue ke parent
  // =========================
  if (_parentId24 != 0 && _count > 0) {
    if (!_inFlight) {
      if ((nowMs - _lastFwdTxMs >= TX_GAP_MS_FWD) &&
          (nowMs >= _nextForwardAllowedMs)) {
        QueueEntry &E = _q[_head];

        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return;
          }
          Lora.print(E.frame);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }
        _inFlight    = true;
        _retry       = 0;
        _lastFwdTxMs = nowMs;

        {
          uint16_t seq16 = (uint16_t)strtoul(E.seqHex.c_str(), nullptr, 16);
          if (_txCb) _txCb(seq16, false);
        }
        Serial.print("[RELAY TX->parent] "); Serial.println(E.frame);
        blinkStatus();
      }
    } else {
      if (isDataTxPhase() && nowMs - _lastFwdTxMs >= ACK_TIMEOUT_MS_FWD) {

        // === FAST FAILOVER ===
        StaticRoute::Id24 oldParent = _parentId24;
        StaticRoute::Id24 newParent = findAlternateParentExcluding(oldParent);

        Serial.print("[RELAY FAST FAILOVER] old=");
        Serial.print(StaticRoute::aliasForId(oldParent));
        Serial.print(" new=");
        Serial.println(StaticRoute::aliasForId(newParent));

        if (newParent != 0 && newParent != oldParent) {
          _parentId24             = newParent;
          _lastRouteEvalFrameIdx  = 0;
          _parentHex6             = StaticRoute::hex24(newParent);
          const char* newAliasC2  = StaticRoute::aliasForId(newParent);
          _parentAlias            = newAliasC2 ? String(newAliasC2) : String("");

          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (_neighbors[i].used && _neighbors[i].id24 == newParent) {
              _hopToSink = (uint8_t)(_neighbors[i].hopToSink + 1);
              break;
            }
          }

          Serial.print("[RELAY FAST FAILOVER] switch parent -> ");
          Serial.println(_parentAlias.length() ? _parentAlias.c_str() : "?");

          // Patch parentAlias di frame sebelum retry
          QueueEntry &E = _q[_head];
          String retryFrame = E.frame;
          {
            const int MAXF_FIX = NUM_FIELDS;
            String ff[MAXF_FIX];
            int ffc = 0, stf = 0;
            for (int i = 0; i <= retryFrame.length(); ++i) {
              if (i == retryFrame.length() || retryFrame[i] == ',') {
                if (ffc < MAXF_FIX) ff[ffc++] = retryFrame.substring(stf, i);
                stf = i + 1;
              }
            }
             if (ffc > IDX_PARENT_ALIAS && _parentAlias.length()) {
              ff[IDX_PARENT_ALIAS] = _parentAlias;
              retryFrame = joinCsv(ff, ffc);
              E.frame    = retryFrame;
            }
          }

          if (canTransmitNow() && requestTx()) {
            if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
              releaseTx();
              return;
            }
            Lora.print(retryFrame);
            Lora.print('\n');
            releaseTx();
            _lastTxMs = millis();
            _lastAnyTxGuardMs = millis();
          }
          _lastFwdTxMs          = nowMs;
          _nextForwardAllowedMs = nowMs + 3000;

          // Tandai sedang healing agar ACK handler bisa blacklist parent lama
          _parentSuspected   = true;
          _healingFromParent = oldParent;

          // Anggap ini sudah 1x retry agar jika gagal lagi masuk normal retry path
          _retry = 1;

          return; // stop agar tidak lanjut retry normal
        }

        // === NORMAL RETRY ===
        if (_retry < MAX_RETRY_FWD) {
          _retry++;

          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
              _neighbors[i].stats.retrySum++;
              break;
            }
          }

          QueueEntry &E = _q[_head];
          if (canTransmitNow() && requestTx()) {
            if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
              releaseTx();
              return;
            }
            Lora.print(E.frame);
            Lora.print('\n');
            releaseTx();
            _lastTxMs = millis();
            _lastAnyTxGuardMs = millis();
          }
          _lastFwdTxMs = nowMs;

          {
            uint16_t seq16 = (uint16_t)strtoul(E.seqHex.c_str(), nullptr, 16);
            if (_txCb) _txCb(seq16, true);
          }
          Serial.print("[RELAY RETRY ");
          Serial.print(_retry);
          Serial.print("] seq=");
          Serial.println(E.seqHex);

        } else {
          QueueEntry &E = _q[_head];
          Serial.print("[RELAY DROP after retry] seq="); Serial.println(E.seqHex);

          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
              _neighbors[i].stats.failCount++;
              _parentAckFailStreak++;
              if (_parentAckFailStreak > 10) _parentAckFailStreak = 10;
              break;
            }
          }

          if (_parentSuspected &&
              _healingFromParent != 0 &&
              _healingFromParent != _parentId24) {
            _brokenParentId24    = _healingFromParent;
            _brokenParentPending = true;
            Serial.printf("[RELAY DROP] blacklist pending broken=%s\n",
                          StaticRoute::aliasForId(_healingFromParent));
          }
          _parentSuspected   = false;
          _healingFromParent = 0;
          pushBacklog(E.frame, E.seqHex);
          _head     = (_head + 1) % QSIZE;
          _count--;
          _inFlight = false;
          _retry    = 0;
        }
      }
    }
  }

  // =========================
  // 3) MODE ORIGIN SELF + HELLO
  // =========================
  uint32_t unixNowSnapshot = nowUnixLike();
  uint32_t frameIdxSnapshot = 0;
  bool inDataSlotSnapshot   = SlotPlan::isInMyTxWindow(_id24, unixNowSnapshot, frameIdxSnapshot);
  uint8_t posInCycleSnapshot = SlotPlan::posInCycleSec(unixNowSnapshot);
  bool inHelloPhaseSnapshot  = SlotPlan::isHelloPhase(unixNowSnapshot);
  uint8_t posInHelloSnapshot = SlotPlan::posInHelloSec(unixNowSnapshot);

  bool isHelloFrame = inHelloPhaseSnapshot;
  bool inHelloSlot  = SlotPlan::isInMyHelloSlot(_id24, posInHelloSnapshot);

  // Reset per-frame flags
  if (frameIdxSnapshot != _selfLastFrameIdx) {
    _selfLastFrameIdx     = frameIdxSnapshot;
    _selfSentThisFrame    = false;
    _backlogSentThisFrame = false;
    _helloSentThisFrame   = false;
  }

  bool wasInHelloBefore = _wasInHelloPhase;
  _wasInHelloPhase = isHelloFrame;

  if (!isHelloFrame && wasInHelloBefore) {
    finalizeHelloFrameCollection(frameIdxSnapshot);
  }

  // Evaluasi parent setiap frame DATA
  if (!isHelloFrame && inDataSlotSnapshot &&
      frameIdxSnapshot != _lastRouteEvalFrameIdx) {
    _lastRouteEvalFrameIdx = frameIdxSnapshot;
    _routeDirty = true;
    debugPrintRoutingTable(frameIdxSnapshot);
  }

  // Clear neighbor state di awal setiap frame
  if (_neighborCollectionFrameIdx != frameIdxSnapshot) {
    _neighborCollectionFrameIdx = frameIdxSnapshot;
    beginHelloFrameCollection(frameIdxSnapshot);
  }

  // -------------------------
  // 3A) FRAME HELLO
  // -------------------------
  if (isHelloFrame) {
    bool helloTxBlocked = (_inFlight || _backlogInFlight);

    if (!helloTxBlocked && inHelloSlot && !_helloSentThisFrame) {
      uint8_t battPct = 0;

      String aliasSelf   = StaticRoute::aliasForId(_id24);
      String parentAlias = _parentAlias;

      uint8_t hopAdvertised =
        (_parentId24 == 0 || _hopToSink >= 250) ? 255 : _hopToSink;

      // Explicit role advertise
      NetRuntime::DeviceRole myRole = NetRuntime::DeviceRole::RELAY;
      if (NetRuntime::hasConfig()) {
        const NetRuntime::DeviceEntry* selfDev = NetRuntime::findDeviceById(_id24);
        if (selfDev) myRole = selfDev->role;
      }

      // Bangun paket HELLO
      String pkt;
      pkt.reserve(64);
      pkt  = MeshProto::ROUTE_HELLO;
      pkt += ",";
      pkt += String((int)DataWire::ROUTE_VER);
      pkt += ",";
      pkt += _idHex6;
      pkt += ",";
      pkt += aliasSelf;
      pkt += ",";
      pkt += parentAlias;
      pkt += ",";
      pkt += String((int)hopAdvertised);
      pkt += ",";
      pkt += String((unsigned long)frameIdxSnapshot);
      pkt += ",";
      pkt += String((int)posInCycleSnapshot);
      pkt += ",";
      pkt += String((int)battPct);
      pkt += ",";
      pkt += String((int)myRole);

      // Neighbor gossip
      int totalUsed = 0;
      for (int i = 0; i < MAX_NEIGHBORS; ++i) {
        if (_neighbors[i].used) totalUsed++;
      }
      if (totalUsed > MAX_HELLO_NEIGHBORS) totalUsed = MAX_HELLO_NEIGHBORS;

      pkt += ",";
      pkt += String((int)totalUsed);

      int sent = 0;
      for (int i = 0; i < MAX_NEIGHBORS && sent < totalUsed; ++i) {
        if (!_neighbors[i].used) continue;
        pkt += ",";
        pkt += _neighbors[i].alias;
        pkt += ",";
        pkt += String((int)_neighbors[i].hopToSink);
        pkt += ",";
        pkt += String((int)_neighbors[i].lastRssi);
        sent++;
      }

      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return;
        }
        Lora.print(pkt);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _helloSentThisFrame = true;
      }

      Serial.print("[RELAY HELLO TX] ");
      Serial.println(pkt);
      blinkStatus();
    }

    // Di frame HELLO: tidak ada origin self / backlog
    // Forwarding dari child tetap berjalan di bagian atas
    return;
  }

  // -------------------------
  // 3B) FRAME DATA normal: Origin self Relay
  // -------------------------
  if (inDataSlotSnapshot && !_selfSentThisFrame &&
      !_inFlight && !_backlogInFlight &&
      _parentId24 != 0) {
    if (_payloadBuilder) {
      uint16_t seq      = _localSeq++;
      uint32_t tsOrigin = unixNowSnapshot;
      String payload    = _payloadBuilder(unixNowSnapshot, seq);

      if (payload.length() > 0) {
        String pkt = DataWire::buildRelayData(
          _idHex6, _id24, _parentId24,
          seq, tsOrigin, frameIdxSnapshot, posInCycleSnapshot,
          payload
        );

        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return;
          }
          Lora.print(pkt);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }

        if (_txCb) _txCb(seq, false);

        // Simpan seq agar ACK handler bisa mencocokkan
        char selfSeqBuf[5];
        snprintf(selfSeqBuf, sizeof(selfSeqBuf), "%04X", (unsigned int)seq);
        _selfLastSeqHex = String(selfSeqBuf);

        Serial.print("[RELAY SELF TX] ");
        Serial.println(pkt);

        _selfSentThisFrame = true;
        blinkStatus();
      } else {
        Serial.println("[RELAY SELF] payload kosong, skip origin");
      }
    }
  }

  // =========================
  // 3C) Backlog TX (slot tail, overheard aware, TTL)
  // =========================
  if (!_backlogInFlight &&
      !_inFlight &&
      _bCount > 0 &&
      _parentId24 != 0 &&
      (nowMs - _backlogAckStartMs > 3000)) {
    BacklogEntry &BE = _bq[_bHead];

    if (frameIdxSnapshot >= _nextBacklogFrameIdxAllowed &&
        canSendBacklogNow(BE, unixNowSnapshot, frameIdxSnapshot, posInCycleSnapshot)) {
      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return;
        }
        Lora.print(BE.frame);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _lastAnyTxGuardMs = millis();
      }

      {
        uint16_t seq16 = (uint16_t)strtoul(BE.seqHex.c_str(), nullptr, 16);
        if (_txCb) _txCb(seq16, false);
      }

      _backlogInFlight      = true;
      _backlogRetry         = 0;
      _backlogAckStartMs    = nowMs;
      _backlogFrameIdx      = frameIdxSnapshot;
      _backlogSentThisFrame = true;
      _backlogSeqInFlight   = BE.seqHex;

      Serial.print("[RELAY-BACKLOG TX] ");
      Serial.println(BE.frame);
      blinkStatus();
    }
  }
  // Backlog retry
  else if (_backlogInFlight && _backlogSeqInFlight.length()) {
    if (nowMs - _backlogAckStartMs >= ACK_TIMEOUT_MS_RELAY) {
      if (_backlogRetry < MAX_RETRY_RELAY) {
        BacklogEntry &BE = _bq[_bHead];
        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return;
          }
          Lora.print(BE.frame);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }

        {
          uint16_t seq16 = (uint16_t)strtoul(BE.seqHex.c_str(), nullptr, 16);
          if (_txCb) _txCb(seq16, true);
        }

        _backlogRetry++;
        _backlogAckStartMs = millis();
        _backlogFrameIdx   = frameIdxSnapshot;

        Serial.print("[RELAY-BACKLOG RETRY ");
        Serial.print(_backlogRetry);
        Serial.print("] ");
        Serial.println(BE.frame);
        blinkStatus();
      } else {
        _backlogInFlight            = false;
        _backlogRetry               = 0;
        _nextBacklogFrameIdxAllowed = frameIdxSnapshot + 1;
        Serial.println("[RELAY-BACKLOG] give up this frame, retry next frame");
      }
    }
  }

  // =========================
  // ROUTING SAFE EXECUTION (NON BLOCKING)
  // =========================
  if (_routeDirty &&
      !isHelloFrame &&
      !_inFlight &&
      !_backlogInFlight &&
      (nowMs - _lastAnyTxGuardMs > 250)) {
    _routeDirty = false;
    chooseBestParentFromNeighbors(_lastRouteEvalFrameIdx);
  }
}

// ======================================================
// 7. BACKLOG / QUEUE MANAGEMENT
// ======================================================

void MeshRelay::enqueueUp(const String& frame, const String& seqHex) {
  if (_count >= QSIZE) {
    Serial.println("[RELAY] Up-queue full, drop frame");
    return;
  }
  _q[_tail].frame  = frame;
  _q[_tail].seqHex = seqHex;
  _tail = (_tail + 1) % QSIZE;
  _count++;
}

void MeshRelay::pushBacklog(const String& frame, const String& seqHex) {
  if (!frame.length()) return;

  if (_bCount >= BQSIZE) {
    _bHead = (_bHead + 1) % BQSIZE;
    _bCount--;
  }

  BacklogEntry &be = _bq[_bTail];
  be.frame  = frame;
  be.seqHex = seqHex;
  be.srcId24           = 0;
  be.slotId            = 0;
  be.createdFrameIdx   = 0;
  be.lastHeardFrameIdx = 0;
  be.lastHeardPos      = 0;

  const int MAXF = DataWire::NUM_FIELDS;
  String f[MAXF];
  int fc = 0, st = 0;

  for (int i = 0; i <= frame.length(); ++i) {
    if (i == frame.length() || frame[i] == ',') {
      if (fc < MAXF) f[fc++] = frame.substring(st, i);
      st = i + 1;
    }
  }

  if (fc > DataWire::IDX_SRC_ID) {
    String srcHex = f[DataWire::IDX_SRC_ID]; srcHex.trim();
    be.srcId24 = StaticRoute::parseHex24(srcHex);
  }
  if (fc > DataWire::IDX_SLOT_ID) {
    String slotS = f[DataWire::IDX_SLOT_ID]; slotS.trim();
    be.slotId = (uint8_t)slotS.toInt();
  }
  if (fc > DataWire::IDX_FRAME_IDX) {
    String frS = f[DataWire::IDX_FRAME_IDX]; frS.trim();
    be.createdFrameIdx = (uint32_t)frS.toInt();
  }

  _bTail = (_bTail + 1) % BQSIZE;
  _bCount++;

  Serial.print("[RELAY-BACKLOG] store seq=");
  Serial.print(seqHex);
  Serial.print(" srcId=");
  Serial.println(StaticRoute::hex24(be.srcId24));
}

bool MeshRelay::canSendBacklogNow(const BacklogEntry& be,
                                  uint32_t unixNow,
                                  uint32_t frameIdx,
                                  uint8_t posInFrame) const {
  // Hard TTL
  if ((frameIdx - be.createdFrameIdx) > 10) return false;

  if (be.createdFrameIdx != 0 &&
      frameIdx > be.createdFrameIdx &&
      (frameIdx - be.createdFrameIdx) >= NetReliability::BACKLOG_TTL_FRAMES) {
    return false;
  }

  if (be.lastHeardFrameIdx != 0 && frameIdx < be.lastHeardFrameIdx) {
    return false;
  }

  uint8_t tailSec = NetReliability::BACKLOG_TAIL_SEC;
  if (!isInTailOfDataWindow(be.srcId24, unixNow, tailSec)) return false;

  if (posInFrame <= be.lastHeardPos && be.lastHeardPos != 0) {
    return false;
  }

  return true;
}

// ======================================================
// 8. FORWARD DUPLICATE SUPPRESSION
// ======================================================

bool MeshRelay::alreadyForwarded(const String& srcHex, const String& seqHex) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_FWD_SIZE; ++i) {
    if (_seenFwd[i].srcHex == srcHex &&
        _seenFwd[i].seqHex == seqHex) {
      if (now - _seenFwd[i].tsMs < 120000UL) return true;
    }
  }
  return false;
}

void MeshRelay::markForwarded(const String& srcHex, const String& seqHex) {
  _seenFwd[_seenFwdPos].srcHex = srcHex;
  _seenFwd[_seenFwdPos].seqHex = seqHex;
  _seenFwd[_seenFwdPos].tsMs   = millis();
  _seenFwdPos++;
  if (_seenFwdPos >= SEEN_FWD_SIZE) _seenFwdPos = 0;
}

// ======================================================
// 9. EVENT HANDLER
// ======================================================

void MeshRelay::onOriginHeard(StaticRoute::Id24 srcId,
                               const String& seqHex,
                               uint32_t frameIdx,
                               uint8_t posInFrame) {
  if (_bCount <= 0 || srcId == 0) return;
  BacklogEntry &be = _bq[_bHead];
  if (be.srcId24 != srcId) return;

  if (be.lastHeardFrameIdx != frameIdx) {
    be.lastHeardFrameIdx = frameIdx;
    be.lastHeardPos      = posInFrame;
  }

  if (seqHex == be.seqHex) {
    Serial.print("[RELAY-BACKLOG] delivered seq=");
    Serial.println(seqHex);

    if (_bCount > 0) {
      _bHead = (_bHead + 1) % BQSIZE;
      _bCount--;
    }
    _backlogInFlight = false;
    _backlogRetry    = 0;
  }
}

// ======================================================
// 10. NEIGHBOR MANAGEMENT
// ======================================================

void MeshRelay::clearNeighbors() {
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    _neighbors[i].id24               = 0;
    _neighbors[i].alias              = "";
    _neighbors[i].lastRssi           = 0;
    _neighbors[i].hopToSink          = 255;
    _neighbors[i].lastSeenFrame      = 0;
    _neighbors[i].lastSeenMs         = 0;
    _neighbors[i].seenThisHelloFrame = false;
    _neighbors[i].helloFrameIdx      = 0;
    _neighbors[i].used               = false;
    _neighbors[i].role               = NetRuntime::DeviceRole::NODE;
    _neighbors[i].stats.txCount      = 0;
    _neighbors[i].stats.ackCount     = 0;
    _neighbors[i].stats.failCount    = 0;
    _neighbors[i].stats.rssiAvg      = 0;
    _neighbors[i].stats.rssiVar      = 0;
    _neighbors[i].stats.retrySum     = 0;
  }
}

void MeshRelay::beginHelloFrameCollection(uint32_t frameIdx) {
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    _neighbors[i].seenThisHelloFrame = false;
    _neighbors[i].helloFrameIdx      = frameIdx;
  }
  Serial.printf("[RELAY HELLO FRAME BEGIN] frame=%lu\n", (unsigned long)frameIdx);
}

void MeshRelay::finalizeHelloFrameCollection(uint32_t frameIdx) {
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    if (_neighbors[i].seenThisHelloFrame) continue;

    Serial.printf("[RELAY HELLO PURGE] alias=%s id=%06lX frame=%lu\n",
                  _neighbors[i].alias.c_str(),
                  (unsigned long)_neighbors[i].id24,
                  (unsigned long)frameIdx);

    _neighbors[i].used               = false;
    _neighbors[i].id24               = 0;
    _neighbors[i].alias              = "";
    _neighbors[i].lastRssi           = 0;
    _neighbors[i].hopToSink          = 255;
    _neighbors[i].lastSeenFrame      = 0;
    _neighbors[i].lastSeenMs         = 0;
    _neighbors[i].seenThisHelloFrame = false;
    _neighbors[i].helloFrameIdx      = 0;
    _neighbors[i].role               = NetRuntime::DeviceRole::NODE;
  }
  Serial.printf("[RELAY HELLO FRAME FINALIZE] frame=%lu\n", (unsigned long)frameIdx);
}

void MeshRelay::updateNeighborFromHello(StaticRoute::Id24 nid,
                                        const String& alias,
                                        NetRuntime::DeviceRole role,
                                        uint8_t hopToSink,
                                        int16_t rssi_dBm,
                                        uint32_t frameIdx) {
  if (nid == 0 || nid == _id24) return;

  int freeIdx  = -1;
  int foundIdx = -1;

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used) {
      if (_neighbors[i].id24 == nid) { foundIdx = i; break; }
    } else if (freeIdx < 0) {
      freeIdx = i;
    }
  }

  int idx = foundIdx;
  if (idx < 0) {
    if (freeIdx >= 0) {
      idx = freeIdx;
    } else {
      // Ganti entry paling lama / RSSI terburuk
      uint32_t worstAge  = 0;
      int16_t  worstRssi = 32767;
      idx = 0;
      for (int i = 0; i < MAX_NEIGHBORS; ++i) {
        uint32_t age  = millis() - _neighbors[i].lastSeenMs;
        int16_t  rssi = _neighbors[i].lastRssi;
        bool worse = (age > worstAge) || (age == worstAge && rssi < worstRssi);
        if (worse) { worstAge = age; worstRssi = rssi; idx = i; }
      }
    }
  }

  bool metricChanged = false;
  if (_neighbors[idx].used) {
    if (_neighbors[idx].lastRssi   != rssi_dBm)  metricChanged = true;
    if (_neighbors[idx].hopToSink  != hopToSink)  metricChanged = true;
    if (_neighbors[idx].role       != role)        metricChanged = true;
  }

  _neighbors[idx].used               = true;
  _neighbors[idx].id24               = nid;
  _neighbors[idx].alias              = alias;
  _neighbors[idx].role               = role;
  _neighbors[idx].lastRssi           = rssi_dBm;
  _neighbors[idx].hopToSink          = hopToSink;
  _neighbors[idx].lastSeenFrame      = frameIdx;
  _neighbors[idx].lastSeenMs         = millis();
  _neighbors[idx].seenThisHelloFrame = true;
  _neighbors[idx].helloFrameIdx      = frameIdx;

  if (metricChanged) {
    Serial.printf("[RELAY HELLO UPDATE] alias=%s rssi=%d hop=%u role=%d\n",
                  alias.c_str(), rssi_dBm, (unsigned)hopToSink, (int)role);
  }

  // RSSI EMA
  float alpha   = 0.2f;
  float prevAvg = _neighbors[idx].stats.rssiAvg;
  float newRssi = (float)rssi_dBm;

  _neighbors[idx].stats.rssiAvg =
    (prevAvg == 0) ? newRssi
    : (0.8f * prevAvg) + (0.2f * newRssi);

  if (_neighbors[idx].id24 == _parentId24) {
    _parentLastRssi = (int16_t)newRssi;
  }

  float diff = newRssi - _neighbors[idx].stats.rssiAvg;
  _neighbors[idx].stats.rssiVar =
    (1.0f - alpha) * _neighbors[idx].stats.rssiVar +
    alpha * diff * diff;
}

uint8_t MeshRelay::neighborCount() const {
  uint8_t count = 0;
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used) ++count;
  }
  return count;
}

void MeshRelay::debugPrintNeighbors() const {
  Serial.print("[RELAY NEIGH] ");
  Serial.print(_idHex6);
  Serial.print(" alias=");
  Serial.print(StaticRoute::aliasForId(_id24));
  Serial.print(" neighbors=");
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    Serial.print(" [");
    Serial.print(_neighbors[i].alias);
    Serial.print(" hop=");
    Serial.print(_neighbors[i].hopToSink);
    Serial.print(" rssi=");
    Serial.print(_neighbors[i].lastRssi);
    Serial.print(" lastF=");
    Serial.print(_neighbors[i].lastSeenFrame);
    Serial.print("]");
  }
  Serial.println();
}

void MeshRelay::debugPrintRoutingTable(uint32_t frameIdx) const {
  Serial.println();
  Serial.println("==================================================");
  Serial.printf(
    "[RELAY ROUTING TABLE] frame=%lu self=%s parent=%s hop=%u\n",
    (unsigned long)frameIdx,
    _idHex6.c_str(),
    _parentAlias.length() ? _parentAlias.c_str() : "NONE",
    (unsigned)_hopToSink
  );
  Serial.println("--------------------------------------------------");
  Serial.println(
    "ALIAS | HOP | RSSI | RSSI_AVG | VAR | TX | ACK | FAIL | RETRY | BASE | EFFECT | AGE"
  );
  Serial.println("--------------------------------------------------");

  uint32_t now = millis();
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    const NeighborInfo& n = _neighbors[i];
    float baseScore    = computeLinkScore(n);
    float effectScore  = computeEffectiveRouteScore(n);
    uint32_t age       = (now >= n.lastSeenMs) ? (now - n.lastSeenMs) : 0;

    Serial.printf(
      "%s | %3u | %4d | %8.1f | %5.1f | %3u | %3u | %4u | %5u | %5.2f | %6.2f | %lu\n",
      n.alias.c_str(),
      (unsigned)n.hopToSink,
      (int)n.lastRssi,
      n.stats.rssiAvg,
      n.stats.rssiVar,
      n.stats.txCount,
      n.stats.ackCount,
      n.stats.failCount,
      n.stats.retrySum,
      baseScore,
      effectScore,
      (unsigned long)age
    );
  }
  Serial.println("==================================================");
  Serial.println();
}

// ======================================================
// 11. ROUTING ENGINE
// ======================================================

float MeshRelay::computeLinkScore(const NeighborInfo& n) const {
  const LinkStats& s = n.stats;

  float smoothedRssi = (s.rssiAvg != 0.0f) ? s.rssiAvg : (float)n.lastRssi;
  float rssiNorm     = (smoothedRssi + 120.0f) / 70.0f;
  if (rssiNorm < 0.0f) rssiNorm = 0.0f;
  if (rssiNorm > 1.0f) rssiNorm = 1.0f;

  float psr = (s.txCount > 5)
    ? (float)s.ackCount / (float)s.txCount
    : 0.7f;
  if (psr < 0.0f) psr = 0.0f;
  if (psr > 1.0f) psr = 1.0f;

  float etxInv;
  if (s.txCount > 0 && s.ackCount > 0) {
    etxInv = (float)s.ackCount / (float)s.txCount;
  } else {
    etxInv = 1.0f / 3.0f;
  }
  if (etxInv < 0.0f) etxInv = 0.0f;
  if (etxInv > 1.0f) etxInv = 1.0f;

  float rssiVar = s.rssiVar;
  if (rssiVar < 0.0f) rssiVar = 0.0f;
  float stability = 1.0f / (1.0f + rssiVar);

  float stabilityBoost = (n.id24 == _parentId24) ? 0.08f : 0.0f;

  return 0.60f * rssiNorm +
         0.20f * psr     +
         0.12f * etxInv  +
         0.08f * stability +
         stabilityBoost;
}

float MeshRelay::computeEffectiveRouteScore(const NeighborInfo& n) const {
  float base    = computeLinkScore(n);
  float penalty = 0.0f;

  if (n.id24 == _parentId24) {
    penalty += _parentAckFailStreak * 0.08f;
    if (_parentLastRssi != 0) {
      float drop = (float)(_parentLastRssi - n.lastRssi);
      if (drop > 5) penalty += drop * 0.01f;
    }
    penalty += n.stats.failCount * 0.03f;
    if (penalty > 0.6f) penalty = 0.6f;
  }

  return base - penalty;
}

StaticRoute::Id24 MeshRelay::findAlternateParentExcluding(StaticRoute::Id24 excludeId) {
  using StaticRoute::Id24;
  static constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 90000UL;

  int16_t RSSI_MIN_PARENT = NetRuntime::rssiMinParent();
  if (RSSI_MIN_PARENT < -120) RSSI_MIN_PARENT = -120;

  Id24    bestId    = 0;
  float   bestScore = -9999.0f;
  uint8_t bestHop   = 255;
  uint8_t bestRole  = 255;

  uint32_t now = millis();

  Serial.println("[RELAY FAST FAILOVER SCAN]");

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used)              continue;
    Id24 nid = _neighbors[i].id24;
    if (nid == excludeId)                 continue;
    if (nid == _id24)                     continue;
    if (_neighbors[i].hopToSink >= 250)   continue;
    if (_neighbors[i].lastRssi < RSSI_MIN_PARENT) continue;
    if (_neighbors[i].stats.failCount > 8) continue;
    if (now - _neighbors[i].lastSeenMs > NEIGHBOR_TIMEOUT_MS) continue;

    int16_t rssi = _neighbors[i].lastRssi;
    if (rssi < -140 || rssi > 0) continue;

    uint8_t rolePriority = 2;
    switch (_neighbors[i].role) {
      case NetRuntime::DeviceRole::SINK:  rolePriority = 0; break;
      case NetRuntime::DeviceRole::RELAY: rolePriority = 1; break;
      default:                            rolePriority = 2; break;
    }

    float score = computeLinkScore(_neighbors[i]);
    if (isnan(score) || isinf(score)) continue;

    uint8_t hop = _neighbors[i].hopToSink;

    Serial.printf("  candidate=%s role=%u hop=%u rssi=%d score=%.2f\n",
                  _neighbors[i].alias.c_str(),
                  (unsigned)rolePriority, (unsigned)hop, (int)rssi, score);

    bool better = false;
    if (bestId == 0)                         better = true;
    else if (rolePriority < bestRole)        better = true;
    else if (rolePriority == bestRole) {
      if (hop < bestHop)                     better = true;
      else if (hop == bestHop && score > bestScore) better = true;
    }

    if (better) {
      bestId    = nid;
      bestRole  = rolePriority;
      bestHop   = hop;
      bestScore = score;
    }
  }

  return bestId;
}

void MeshRelay::chooseBestParentFromNeighbors(uint32_t frameIdx) {
  using StaticRoute::Id24;
  using NetRuntime::DeviceRole;

  static constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 90000UL;
  static constexpr float    HYSTERESIS_MARGIN   = 0.35f;

  int16_t RSSI_MIN_PARENT = NetRuntime::rssiMinParent();
  if (RSSI_MIN_PARENT < -120) RSSI_MIN_PARENT = -120;

  uint32_t now = millis();

  // 0. Resolve blacklist pending
  if (_brokenParentPending && _brokenParentId24 != 0) {
    _brokenParentUntilFrameIdx = frameIdx + 2;
    _brokenParentPending       = false;
    Serial.printf("[RELAY ROUTE] blacklist parent=%s until frame=%lu\n",
                  StaticRoute::aliasForId(_brokenParentId24),
                  (unsigned long)_brokenParentUntilFrameIdx);
  }

  // 1. Expire stale neighbors
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    if (now - _neighbors[i].lastSeenMs > NEIGHBOR_TIMEOUT_MS) {
      Serial.print("[RELAY NEIGHBOR EXPIRE] ");
      Serial.println(_neighbors[i].alias);
      _neighbors[i].used = false;
    }
  }

  // 2. Parent presence check
  bool parentFound = false;
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
      parentFound = true;
      break;
    }
  }

  if (_parentId24 != 0 && !parentFound) {
    Serial.printf("[RELAY ROUTE] parent not in fresh table, clear -> %s\n",
                  _parentAlias.length() ? _parentAlias.c_str() : "?");
    _parentId24          = 0;
    _parentHex6          = "";
    _parentAlias         = "";
    _hopToSink           = 255;
    _parentAckFailStreak = 0;
    _parentDegradeScore  = 0.0f;
  }

  // 3. Snapshot current parent
  int16_t currentParentRssi   = -32768;
  float   currentScore        = -9999.0f;
  uint8_t currentHop          = (_hopToSink == 255) ? 254 : _hopToSink;
  uint8_t currentRolePriority = 255;

  if (_parentId24 != 0) {
    for (int i = 0; i < MAX_NEIGHBORS; ++i) {
      if (!_neighbors[i].used) continue;
      if (_neighbors[i].id24 != _parentId24) continue;

      currentParentRssi = _neighbors[i].lastRssi;
      float raw = computeLinkScore(_neighbors[i]);
      if (isnan(raw) || isinf(raw)) raw = -9999.0f;
      currentScore = raw - _parentDegradeScore;

      switch (_neighbors[i].role) {
        case DeviceRole::SINK:  currentRolePriority = 0; break;
        case DeviceRole::RELAY: currentRolePriority = 1; break;
        default:                currentRolePriority = 2; break;
      }
      break;
    }
  }

  // 4. Scan kandidat terbaik
  Id24    bestId           = 0;
  uint8_t bestHop          = 255;
  int16_t bestRssi         = -32768;
  float   bestScore        = -9999.0f;
  uint8_t bestRolePriority = 255;

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;
    auto &n = _neighbors[i];

    if (n.lastRssi < RSSI_MIN_PARENT) continue;
    if (n.stats.failCount > 8)         continue;
    if (n.hopToSink >= 250)            continue;

    // Skip blacklisted parent
    if (n.id24 == _brokenParentId24 &&
        frameIdx < _brokenParentUntilFrameIdx) {
      Serial.printf("[RELAY ROUTE BLACKLIST] skip=%s until frame=%lu\n",
                    n.alias.c_str(), (unsigned long)_brokenParentUntilFrameIdx);
      continue;
    }

    // Relay tidak boleh memilih dirinya sendiri sebagai parent
    if (n.id24 == _id24) continue;

    uint8_t rolePriority = 255;
    switch (n.role) {
      case DeviceRole::SINK:  rolePriority = 0; break;
      case DeviceRole::RELAY: rolePriority = 1; break;
      default:                rolePriority = 2; break;
    }

    float base    = computeLinkScore(n);
    float penalty = 0.0f;

    if (n.id24 == _parentId24) {
      penalty += _parentAckFailStreak * 0.08f;
      if (_parentLastRssi != 0) {
        float drop = (float)(_parentLastRssi - n.lastRssi);
        if (drop > 5) penalty += drop * 0.01f;
      }
      penalty += n.stats.failCount * 0.03f;
      if (penalty > 0.6f) penalty = 0.6f;
      _parentDegradeScore = penalty;
    }

    float score = base - penalty;

    bool betterCandidate = false;
    if (bestId == 0)                                        betterCandidate = true;
    else if (rolePriority < bestRolePriority)               betterCandidate = true;
    else if (rolePriority == bestRolePriority) {
      if (n.hopToSink < bestHop)                           betterCandidate = true;
      else if (n.hopToSink == bestHop && score > bestScore) betterCandidate = true;
    }

    if (!betterCandidate) continue;

    bestRolePriority = rolePriority;
    bestScore        = score;
    bestId           = n.id24;
    bestHop          = n.hopToSink;
    bestRssi         = n.lastRssi;
  }

  // 5. Tidak ada kandidat valid
  if (bestId == 0) {
    Serial.println("[RELAY ROUTE] NO VALID PARENT in neighbor table");
    _parentId24  = 0;
    _parentHex6  = "";
    _parentAlias = "";
    _hopToSink   = 255;
    return;
  }

  uint8_t newHop = bestHop + 1;

  Serial.println();
  Serial.println("--------------- RELAY ROUTE DECISION ----------------");
  Serial.printf("best=%s score=%.2f hop=%u rssi=%d\n",
                StaticRoute::aliasForId(bestId), bestScore, bestHop, bestRssi);
  Serial.printf("current=%s hop=%u rssi=%d score=%.2f\n",
                _parentAlias.length() ? _parentAlias.c_str() : "NONE",
                currentHop, currentParentRssi, currentScore);
  Serial.printf("ackFail=%u degrade=%.2f blacklist=%s untilF=%lu\n",
                _parentAckFailStreak, _parentDegradeScore,
                StaticRoute::aliasForId(_brokenParentId24),
                (unsigned long)_brokenParentUntilFrameIdx);
  Serial.println("------------------------------------------------------");

  // 6. Keputusan switch / hold
  bool shouldSwitch =
    (_parentId24 == 0) ||
    (bestRolePriority < currentRolePriority) ||
    (bestRolePriority == currentRolePriority && newHop < currentHop) ||
    (bestRolePriority == currentRolePriority && newHop == currentHop &&
     bestScore > currentScore + HYSTERESIS_MARGIN);

  if (!shouldSwitch && _parentId24 != 0) {
    _hopToSink = currentHop;
    Serial.printf("[RELAY ROUTE] frame=%lu HOLD parent=%s (no improvement)\n",
                  (unsigned long)frameIdx, _parentAlias.length() ? _parentAlias.c_str() : "?");
    return;
  }

  // 7. Terapkan parent baru
  const char* alias = StaticRoute::aliasForId(bestId);
  if (!alias || !alias[0]) {
    Serial.printf("[RELAY ROUTE] reject: invalid alias id=%06lX\n",
                  (unsigned long)bestId);
    return;
  }

  bool isSwitch = (bestId != _parentId24);

  _parentId24  = bestId;
  _parentHex6  = StaticRoute::hex24(bestId);
  _parentAlias = String(alias);
  _hopToSink   = newHop;

  if (isSwitch) {
    _parentAckFailStreak = 0;
    _parentDegradeScore  = 0.0f;
  }

  Serial.printf("[RELAY ROUTE] frame=%lu %s parent=%s hop=%u score=%.2f\n",
                (unsigned long)frameIdx,
                isSwitch ? "SWITCH" : "RECONFIRM",
                _parentAlias,
                (unsigned)newHop,
                bestScore);
}
