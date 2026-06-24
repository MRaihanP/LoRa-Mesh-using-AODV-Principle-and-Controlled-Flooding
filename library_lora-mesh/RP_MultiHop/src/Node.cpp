// ======================================================
// 1. INCLUDE SECTION
// ======================================================
#include "Node.h"
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
bool aliasInPath(const String& path, const String& alias) {
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

// ======================================================
// 3. CONSTRUCTOR
// ======================================================

MeshNode::MeshNode(const BoardPins& pins)
: MeshDeviceBase("NODE", pins),
  // IDENTITY & ROUTING STATE
  _id24(0),
  _parentId24(0),
  _hopToSink(255),

  // SEQUENCE / PACKET STATE
  _seq(1),
  _lastOriginSeqHex(""),
  _hasLastOriginFrame(false),
  _lastOriginFrame(""),

  // TX CONTROL STATE
  _waitingAck(false),
  _ackStartMs(0),
  _retry(0),
  _lastTxMs(0),
  _inFlight(false),

  // FRAME / SCHEDULING STATE
  _lastFrameIdx(0),
  _sentThisFrame(false),
  _nextForwardAllowedMs(0),

  // FORWARDING STATE
  _fwdRetry(0),
  _fwdLastTxMs(0),

  // MAIN QUEUE STATE
  _head(0),
  _tail(0),
  _count(0),

  // BACKLOG QUEUE STATE
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
  _lastRouteEvalFrameIdx(0),
  _wasInHelloPhase(false),

  // PARENT RELIABILITY STATE
  _parentAckFailStreak(0),
  _parentLastRssi(0),
  _parentDegradeScore(0.0f),

  // FAST SELF-HEALING STATE
  _parentSuspected(false),
  _brokenParentPending(false),
  _healingFromParent(0),
  _brokenParentId24(0),
  _brokenParentUntilFrameIdx(0)
{}

// ======================================================
// 4. LIFECYCLE CORE
// ======================================================

// Node initialization: bootstrap identity, routing state, and neighbor system
void MeshNode::begin() {
  MeshDeviceBase::begin();
  initLoraUart();

  Id::begin();
  _id24   = Id::id24();
  _idHex6 = Id::idHex6();

  // routing bootstrap state
  _parentId24 = 0;
  _parentHex6 = "";
  _parentAlias = "";

  _parentConfirmed = false;
  _candidateParentId = 0;
  _parentLostSinceMs = 0;

  _hopToSink = 255;
  _failedParentCount = 0;

  Serial.printf("[NODE] ID=%s, dynamic bootstrap, childLearning=REALTIME\n",
                _idHex6.c_str());

  // NOTE:
  // battery management moved out of node layer (application level)

  clearNeighbors();

  _wasInHelloPhase = false;

  uint32_t now = millis();

  _nextForwardAllowedMs = now;
  _nextBacklogFrameIdxAllowed = 0;
  _backlogInFlight = false;
  _backlogRetry = 0;
}

// Check whether node is currently allowed to transmit DATA
bool MeshNode::isDataTxPhase() const {
  uint32_t unixNow = nowUnixLike();
  uint32_t frameIdxDummy = 0;

  // block DATA during HELLO phase
  if (SlotPlan::isHelloPhase(unixNow))
    return false;

  // allow only within assigned TX window
  return SlotPlan::isInMyTxWindow(_id24, unixNow, frameIdxDummy);
}

void MeshNode::loop() {
  static uint32_t _lastAnyTxGuardMs = 0;
  static constexpr uint32_t TX_GLOBAL_GAP_MS = 120;
  // Konfigurasi reliability diambil dari NetConfig.h
  static const uint32_t ACK_TIMEOUT_MS_NODE = NetReliability::NODE_ACK_TIMEOUT_MS;
  static const uint8_t  MAX_RETRY_NODE      = NetReliability::NODE_MAX_RETRY;

  static const uint32_t ACK_TIMEOUT_MS_FWD  = NetReliability::FWD_ACK_TIMEOUT_MS;
  static const uint8_t  MAX_RETRY_FWD       = NetReliability::FWD_MAX_RETRY;
  static const uint32_t TX_GAP_MS_FWD       = NetReliability::FWD_TX_GAP_MS;
  static const uint32_t FORWARD_GUARD_MS    = NetReliability::FWD_FORWARD_GUARD_MS;

  static String rxBuf;
  uint32_t nowMs = millis();

  // =========================
  // 1) RX: kumpulkan byte dari LoRa UART
  // =========================
  while (Lora.available()) {

      char c = (char)Lora.read();
      rxBuf += c;

      // Frame LoRa forwarding bisa panjang (>150 byte)
      // jadi buffer harus lebih besar.
      // Clear hanya jika benar-benar corrupt.
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
    if (nlIdx + 1 >= rxBuf.length()) break;

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
      int fcAck = 0;
      int s     = 0;
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

      // ACK untuk origin (Node sebagai child dari Relay/parent)
      if (_parentId24 != 0 &&
          dst == _idHex6 &&
          _waitingAck)
      {
          char seqBuf[5];
          snprintf(seqBuf, sizeof(seqBuf), "%04X",
                  (unsigned int)((_seq == 0)
                  ? 0xFFFF
                  : (_seq - 1)));

          String expectedSeq = String(seqBuf);

          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (!_neighbors[i].used)
              continue;
            if (_neighbors[i].id24 != _parentId24)
              continue;
            // ACK SUCCESS
            // _neighbors[i].stats.ackCount++;
            // turunkan fail streak jika sebelumnya sempat gagal
            if (_parentAckFailStreak > 0)
              _parentAckFailStreak--;
            break;
          }

          if (seqStr.equalsIgnoreCase(expectedSeq)) {

              Serial.print("[NODE] ACK OK seq=");
              Serial.println(seqStr);

              uint16_t ackSeq =
                  (_seq == 0)
                  ? 0xFFFF
                  : (uint16_t)(_seq - 1);

              if (_ackCb) {
                  _ackCb(ackSeq);
              }

              _waitingAck = false;
              _retry      = 0;

              // =====================================
              // UPDATE LINK STATS (ACK SUCCESS)
              // =====================================
              for (int i = 0; i < MAX_NEIGHBORS; ++i) {

                if (!_neighbors[i].used)
                  continue;

                if (_neighbors[i].id24 != _parentId24)
                  continue;

                _neighbors[i].stats.txCount++;

                _neighbors[i].stats.ackCount++;

                break;
              }

              if (_parentAckFailStreak > 0) {
                _parentAckFailStreak--;
              }

              // ACK berhasil: jika sedang dalam mode healing,
              // blacklist parent lama (yg gagal) untuk 2 frame ke depan.
              // Penyelesaian frameIdx dilakukan saat chooseBestParent dipanggil.
              if (_parentSuspected &&
                  _healingFromParent != 0 &&
                  _healingFromParent != _parentId24)
              {
                _brokenParentId24   = _healingFromParent;
                _brokenParentPending = true; // frameIdx diselesaikan di chooseBestParent
                Serial.printf("[HEAL ACK OK] blacklist pending parent=%s -> using=%s\n",
                              StaticRoute::aliasForId(_healingFromParent),
                              _parentAlias.c_str());
              }

              _parentSuspected  = false;
              _healingFromParent = 0;
          }
      }

      // =====================================
      // FORWARD ACK HANDLER
      // =====================================
      if (_inFlight && _count > 0) {

          FwdEntry &E = _q[_head];

          bool fromCurrentParent =
              (_parentId24 != 0) &&
              fromId.equalsIgnoreCase(_parentHex6);

          bool ownershipValid = false;

          {
              const int MAXF_VERIFY = NUM_FIELDS;
              String fv[MAXF_VERIFY];

              int fcv = 0;
              int stv = 0;

              for (int i = 0; i <= E.frame.length(); ++i) {
                  if (i == E.frame.length() || E.frame[i] == ',') {
                      if (fcv < MAXF_VERIFY) {
                          fv[fcv++] = E.frame.substring(stv, i);
                      }
                      stv = i + 1;
                  }
              }

              if (fcv > IDX_SRC_ID) {

                  String srcVerify = fv[IDX_SRC_ID];
                  srcVerify.trim();

                  ownershipValid =
                      srcVerify.length() &&
                      seqStr.equalsIgnoreCase(E.seqHex);
              }
          }

          if (fromCurrentParent && ownershipValid)
          {
              Serial.print("[NODE-FWD ACK OK] seq=");
              Serial.println(seqStr);

              _head = (_head + 1) % QSIZE;
              _count--;

              _inFlight = false;
              _fwdRetry = 0;
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
          seqStr.equalsIgnoreCase(_backlogSeqInFlight))
      {
          Serial.print("[NODE-BACKLOG ACK OK] seq=");
          Serial.println(seqStr);

          _bHead = (_bHead + 1) % BQSIZE;
          _bCount--;

          _backlogInFlight = false;
          _backlogRetry    = 0;
          _backlogSeqInFlight = "";

          uint16_t ackSeq = (uint16_t)strtoul(seqStr.c_str(), nullptr, 16);
          
      }

      // ACK untuk backlog / forward queue tidak di-handle di Node (hanya Relay)
      continue;
    }

    // =========================
    // 1B) ROUTE_HELLO (gossip topologi)
    // =========================
    if (line.startsWith(MeshProto::ROUTE_HELLO)) {
      // Layout HELLO (konsisten dengan TX):
      //  0=proto(10), 1=routeVer, 2=srcIdHex, 3=aliasSelf,
      //  4=parentAlias, 5=hopToSink, 6=frameIdx, 7=posInFrame,
      //  8=battPct, 9=role, 10=neighCount, 11..= triplet neighbors
      const int MAXF_HELLO = 32;
      String fh[MAXF_HELLO];
      int    fch = 0, stH = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          if (fch < MAXF_HELLO) {
            fh[fch++] = line.substring(stH, i);
          }
          stH = i + 1;
        }
      }
      if (fch < 11) {
        Serial.print("[NODE HELLO] Drop: field < 11 | line=");
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

      // Hitung frameIdx LOKAL Node saat HELLO ini diterima
      uint32_t frameIdxLocal = 0;
      {
        uint32_t unixNowLocal = nowUnixLike();
        bool dummy = SlotPlan::isInMyTxWindow(_id24, unixNowLocal, frameIdxLocal);
        (void)dummy;
      }

      StaticRoute::Id24 nid = StaticRoute::idForAlias(aliasSelf);

      // Update neighbor table dengan pengirim langsung (jangan simpan diri sendiri)
      if (nid != 0 && nid != _id24) {
        updateNeighborFromHello(
          nid,
          aliasSelf,
          advertisedRole,
          hopToSink,
          (int16_t)rssi_dBm,
          frameIdxLocal
        );

        Serial.print("[NODE HELLO RX] from=");
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

        // Hanya proses gosip yang valid dan bukan diri sendiri
        StaticRoute::Id24 gId = StaticRoute::idForAlias(gAlias);
        if (gId == 0 || gId == _id24 || gHop >= 250) continue;

        // Gossip: update tabel hanya jika tetangga ini belum dikenal
        // atau data gossip lebih fresh dari yang ada.
        // Gunakan rssi pengirim sebagai rssi gossip (tidak diketahui langsung).
        // Flag gossip dengan rssi = 0 agar tidak menimpa data direct link.
        bool alreadyKnown = false;
        for (int k = 0; k < MAX_NEIGHBORS; ++k) {
          if (_neighbors[k].used && _neighbors[k].id24 == gId) {
            alreadyKnown = true;
            break;
          }
        }
        if (!alreadyKnown) {
          // Tetangga belum dikenal langsung: tambahkan dari gossip
          // dengan RSSI 0 (tidak diukur langsung) agar tidak dipilih
          // sebagai parent kecuali tidak ada pilihan lain.
          updateNeighborFromHello(
            gId,
            gAlias,
            NetRuntime::DeviceRole::NODE,  // role tidak diketahui via gossip
            gHop,
            0,           // rssi tidak diketahui langsung
            frameIdxLocal
          );
        }
      }

      continue; // jangan diproses sebagai DATA
    }

    // =========================
    // 1C) Data CSV (paket dari child yang harus di-forward)
    // =========================
    const int MAXF = NUM_FIELDS;  // dari DataWire::NUM_FIELDS
    String f[MAXF];
    int fc = 0, st = 0;

    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        if (fc < MAXF) {
          f[fc++] = line.substring(st, i);
        }
        st = i + 1;
      }
    }

    // =========================
    // DEBUG FIELD PARSER
    // =========================
    Serial.println();
    Serial.println();
    Serial.println("========== RX FIELD DEBUG ==========");

    Serial.print("LEN = ");
    Serial.println(line.length());

    Serial.print("RAW = ");
    Serial.println(line);

    Serial.print("FIELDS = ");
    Serial.println(fc);

    for (int i = 0; i < fc; ++i) {

        Serial.print("[");
        Serial.print(i);
        Serial.print("] => ");

        Serial.println(f[i]);
    }

    Serial.println("====================================");

    Serial.println("====================================");
    
    // ======================================================
    // FLEXIBLE HEADER VALIDATION
    // ======================================================
    //
    // Packet origin dari child belum tentu sudah memiliki
    // seluruh footer forwarding.
    //
    // Jadi validasi cukup berdasarkan field inti yang
    // benar-benar wajib untuk forwarding.
    // ======================================================

    // field minimum wajib:
    const int MIN_FIELDS_NODE = 14;

    if (fc < MIN_FIELDS_NODE) {

        Serial.print("[NODE] INVALID HEADER fc=");
        Serial.println(fc);

        continue;
    }

    // ======================================================
    // SAFE OPTIONAL FIELD EXTENSION
    // ======================================================
    //
    // Pastikan seluruh field forwarding tersedia.
    // Jika belum ada, isi default agar parser aman.
    // ======================================================

    while (fc <= IDX_HOP_SNR_SUM) {
      f[fc++] = "0";
    }

    // path kosong -> init
    if (f[IDX_PATH].length() == 0) {
      const char* srcAliasC =
          StaticRoute::aliasForId(
              StaticRoute::parseHex24(f[IDX_SRC_ID]));

      if (srcAliasC)
        f[IDX_PATH] = String(srcAliasC);
    }

    // hop counter default
    if (f[IDX_HOP_COUNT].length() == 0)
      f[IDX_HOP_COUNT] = "0";

    // RSSI sum default
    if (f[IDX_HOP_RSSI_SUM].length() == 0)
      f[IDX_HOP_RSSI_SUM] = "0";

    // SNR sum default
    if (f[IDX_HOP_SNR_SUM].length() == 0)
      f[IDX_HOP_SNR_SUM] = "0";

    // --- Ambil field penting ---
    String proto       = f[IDX_PROTO];        proto.trim();
    String srcIdHex    = f[IDX_SRC_ID];       srcIdHex.trim();
    String seqHex      = f[IDX_SEQ];          seqHex.trim();
    String tsOriginS   = f[IDX_TS_ORIGIN];    tsOriginS.trim();
    String hopCntS     = f[IDX_HOP_COUNT];    hopCntS.trim();
    String hopRssiS    = f[IDX_HOP_RSSI_SUM]; hopRssiS.trim();
    String hopSnrS     = f[IDX_HOP_SNR_SUM];  hopSnrS.trim();
    String path        = f[IDX_PATH];         path.trim();
    String parentAl    = f[IDX_PARENT_ALIAS]; parentAl.trim();
    String destAl      = f[IDX_DEST_ALIAS];   destAl.trim();

    // Alias Node ini
    const char* myAliasC = StaticRoute::aliasForId(_id24);
    String myAlias = myAliasC ? String(myAliasC) : String("");

    // Tracking activity origin utk logika backlog:
    uint32_t frameIdxRx   = 0;
    uint8_t  posInFrameRx = 0;
    if (fc > IDX_FRAME_IDX) {
      String frS = f[IDX_FRAME_IDX];
      frS.trim();
      frameIdxRx = (uint32_t)frS.toInt();
    }
    if (fc > IDX_POS_IN_FRAME) {
      String posS = f[IDX_POS_IN_FRAME];
      posS.trim();
      posInFrameRx = (uint8_t)posS.toInt();
    }
    StaticRoute::Id24 srcId24 = StaticRoute::parseHex24(srcIdHex);
    onOriginHeard(srcId24, seqHex, frameIdxRx, posInFrameRx);

    // Parent hanya memproses paket jika parentAlias == alias dirinya sendiri
    if (!myAlias.length() || parentAl != myAlias) {
      // Paket ini bukan ditujukan untuk Node ini sebagai parent → drop
      continue;
    }

    // duplicate suppression
    if (alreadyForwarded(srcIdHex, seqHex)) {

      Serial.print("[FWD DUP DROP] ");
      Serial.print(srcIdHex);
      Serial.print(" seq=");
      Serial.println(seqHex);

      continue;
    }

    markForwarded(srcIdHex, seqHex);

    // Cari alias hop terakhir dari PATH (hop sebelum Node ini = child langsung)
    int lastDash   = path.lastIndexOf('-');
    String lastAlias = (lastDash < 0) ? path : path.substring(lastDash + 1);
    lastAlias.trim();

    StaticRoute::Id24 childId = StaticRoute::idForAlias(lastAlias);

    // Timestamp origin dan footer hop
    uint32_t tsOrigin   = (uint32_t)strtoul(tsOriginS.c_str(), nullptr, 10);
    uint8_t  hopCount   = (uint8_t) hopCntS.toInt();
    int32_t  hopRssiSum = (int32_t) strtol(hopRssiS.c_str(),   nullptr, 10);
    float    hopSnrSum  = hopSnrS.toFloat();

    // Update hopCount + RSSI + SNR di hop Node ini
    hopCount++;
    hopRssiSum += rssi_dBm;
    float snrHop = (float)rssi_dBm - NOISE_FLOOR_DBM;  // NOISE_FLOOR_DBM dari DataWire
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

    // Append alias Node ini ke PATH
    if (path.length() == 0) {
      path = myAlias;
    } else {
      path += '-';
      path += myAlias;
    }
    f[IDX_PATH] = path;

    // Tentukan next-hop dengan anti-loop berbasis PATH
    StaticRoute::Id24 nextHopId = _parentId24;
    if (path.length()) {
      nextHopId = chooseNextHopForPacket(path);
    }

    // Kalau 0 -> tidak ada next-hop aman (loop / dynamic OFF dan parent ada di PATH)
    if (nextHopId == 0) {
      Serial.print("[NODE ANTILOOP] Drop frame seq=");
      Serial.print(seqHex);
      Serial.print(" path=");
      Serial.println(path);
      continue;  // jangan forward / enqueue
    }

    const char* parentAliasNextC = StaticRoute::aliasForId(nextHopId);
    if (parentAliasNextC && parentAliasNextC[0] != '\0') {
      f[IDX_PARENT_ALIAS] = String(parentAliasNextC);
    } else {
      Serial.print("[NODE ANTILOOP] Drop frame: next-hop alias empty, seq=");
      Serial.println(seqHex);
      continue;
    }

    // Kalau destAlias == alias Node ini → Node dianggap final dest → ACK tapi tidak forward
    if (destAl == myAlias) {
      if (childId != 0) {
        String childHex = StaticRoute::hex24(childId);
        String ack = "ACK," + childHex + "," + seqHex + "," + _idHex6;
        delay(random(20, 120));
        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return; // HARD LOCK: cegah TX storm
          }
          Lora.print(ack);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }
        Serial.print("[NODE-FINAL] ACK child seq=");
        Serial.print(seqHex);
        Serial.print(" to ");
        Serial.println(childHex);
      }
      blinkStatus();
      continue;
    }

    // Kirim ACK ke child (ID child diambil dari alias childId)
    if (childId != 0) {
      String childHex = StaticRoute::hex24(childId);
      String ack = "ACK," + childHex + "," + seqHex + "," + _idHex6;
      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return; // HARD LOCK: cegah TX storm
        }
        Lora.print(ack);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _lastAnyTxGuardMs = millis();
      }
      Serial.print("[NODE-FWD] ACK child seq=");
      Serial.print(seqHex);
      Serial.print(" to ");
      Serial.println(childHex);
    }

    // Rekonstruksi frame dan enqueue ke parent
    String frame = joinCsv(f, fc);  // fc boleh > MIN_FIELDS_NODE (termasuk payload)
    enqueueUp(frame, seqHex);

    {
      uint16_t seq16 = (uint16_t)strtoul(seqHex.c_str(), nullptr, 16);
      if (_fwdRxCb) _fwdRxCb((uint32_t)childId, seq16);
    }

    _nextForwardAllowedMs = nowMs + FORWARD_GUARD_MS;

    Serial.print("[NODE-FWD] Enqueue up from childAlias=");
    Serial.print(lastAlias);
    Serial.print(" seq=");
    Serial.println(seqHex);

    blinkStatus();
  }

  // =========================
  // 2) TX: forward queue
  // =========================
  if (_parentId24 != 0 && _count > 0) {

    if (_waitingAck || _backlogInFlight) {

        // forward pause sementara

    } else if (!_inFlight) {
        if ((nowMs - _fwdLastTxMs >= TX_GAP_MS_FWD) &&
            (nowMs >= _nextForwardAllowedMs))
        {
          FwdEntry &E = _q[_head];

          // update TX stats ke parent
          for (int i = 0; i < MAX_NEIGHBORS; ++i) {
            if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
              // _neighbors[i].stats.txCount++;
              break;
            }
          }

          if (canTransmitNow() && requestTx()) {
            if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
              releaseTx();
              return; // HARD LOCK: cegah TX storm
            }
            Lora.print(E.frame);
            Lora.print('\n');
            releaseTx();
            _lastTxMs = millis();
            _lastAnyTxGuardMs = millis();
          }
          _inFlight    = true;
          _fwdRetry    = 0;
          _fwdLastTxMs = nowMs;
          Serial.print("[NODE-FWD TX->parent] ");
          {
            uint16_t seq16 = (uint16_t)strtoul(E.seqHex.c_str(), nullptr, 16);
            if (_txCb) _txCb(seq16, false); 
          }
          Serial.println(E.frame);
          blinkStatus();
        }
      } else {
        if (isDataTxPhase() && nowMs - _fwdLastTxMs >= ACK_TIMEOUT_MS_FWD) {

          // === FAST FAILOVER ===
          StaticRoute::Id24 oldParent = _parentId24;
          StaticRoute::Id24 newParent = findAlternateParentExcluding(oldParent);

          Serial.print("[FAST FAILOVER] old=");
          Serial.print(StaticRoute::aliasForId(oldParent));
          Serial.print(" new=");
          Serial.println(StaticRoute::aliasForId(newParent));

          if (newParent != 0 && newParent != oldParent) {
            _parentId24 = newParent;
            _lastRouteEvalFrameIdx = 0; // force re-evaluation
            _parentHex6 = StaticRoute::hex24(newParent);
            _parentAlias = StaticRoute::aliasForId(newParent);

            for (int i = 0; i < MAX_NEIGHBORS; ++i) {

                if (_neighbors[i].used &&
                    _neighbors[i].id24 == newParent)
                {
                    _hopToSink =
                        (uint8_t)(_neighbors[i].hopToSink + 1);

                    break;
                }
            }

            Serial.print("[FAST FAILOVER] switch parent -> ");
            Serial.println(_parentAlias);

            // retry langsung TANPA nambah retry counter
            FwdEntry &E = _q[_head];

            String retryFrame = E.frame;

            {
              const int MAXF_FIX = NUM_FIELDS;
              String ff[MAXF_FIX];

              int ffc = 0;
              int stf = 0;

              for (int i = 0; i <= retryFrame.length(); ++i) {
                if (i == retryFrame.length() || retryFrame[i] == ',') {

                  if (ffc < MAXF_FIX) {
                    ff[ffc++] = retryFrame.substring(stf, i);
                  }

                  stf = i + 1;
                }
              }

              if (ffc > IDX_PARENT_ALIAS) {

                const char* newAliasC =
                  StaticRoute::aliasForId(newParent);

                if (newAliasC && newAliasC[0] != '\0') {

                  ff[IDX_PARENT_ALIAS] = String(newAliasC);

                  retryFrame = joinCsv(ff, ffc);

                  E.frame = retryFrame;
                }
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
            _fwdLastTxMs = nowMs;
            _nextForwardAllowedMs = nowMs + 3000;
            return; // penting: stop agar tidak lanjut ke retry normal
          }

          // === NORMAL RETRY ===
          if (_fwdRetry < MAX_RETRY_FWD) {
            _fwdRetry++;

            for (int i = 0; i < MAX_NEIGHBORS; ++i) {
              if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
                _neighbors[i].stats.retrySum++;
                break;
              }
            }

            FwdEntry &E = _q[_head];
            if (canTransmitNow() && requestTx()) {
              if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
                releaseTx();
                return; // HARD LOCK: cegah TX storm
              }
              Lora.print(E.frame);
              Lora.print('\n');
              releaseTx();
              _lastTxMs = millis();
              _lastAnyTxGuardMs = millis();
            }

            _fwdLastTxMs = nowMs;

            Serial.print("[NODE-FWD RETRY ");
            Serial.print(_fwdRetry);
            Serial.print("] seq=");
            Serial.println(E.seqHex);

          } else {
            FwdEntry &E = _q[_head];

            Serial.print("[NODE-FWD DROP after retry] seq=");
            Serial.println(E.seqHex);

            for (int i = 0; i < MAX_NEIGHBORS; ++i) {
              if (_neighbors[i].used &&
                  _neighbors[i].id24 == _parentId24)
              {
                  _neighbors[i].stats.failCount++;

                  if (_neighbors[i].id24 == _parentId24) {
                    _parentAckFailStreak++;

                    if (_parentAckFailStreak > 10)
                      _parentAckFailStreak = 10;
                  }
                  break;
              }
            }

            if (_parentSuspected && _healingFromParent != 0 &&
                _healingFromParent != _parentId24) {
              _brokenParentId24 = _healingFromParent;
              _brokenParentPending = true; // frameIdx diselesaikan di chooseBestParent
              Serial.printf("[NODE-FWD DROP] blacklist pending broken=%s\n",
                            StaticRoute::aliasForId(_healingFromParent));
            }

            _parentSuspected   = true;
            _healingFromParent = 0;

            pushBacklog(E.frame, E.seqHex);

            _head = (_head + 1) % QSIZE;
            _count--;
            _inFlight = false;
            _fwdRetry = 0;
          }
        }
      }
  }

  // =========================
  // 3) MODE ORIGIN (Node kirim data sendiri, pakai slot RTC)
  // =========================

  uint32_t now = nowMs;
  uint32_t unixNowSnapshot = nowUnixLike();

  uint32_t frameIdxSnapshot = 0;
  bool inDataSlotSnapshot = SlotPlan::isInMyTxWindow(_id24, unixNowSnapshot, frameIdxSnapshot);
  uint8_t posInCycleSnapshot =SlotPlan::posInCycleSec(unixNowSnapshot);
  bool inHelloPhaseSnapshot =SlotPlan::isHelloPhase(unixNowSnapshot);
  uint8_t posInHelloSnapshot =SlotPlan::posInHelloSec(unixNowSnapshot);

  // HELLO aktif hanya pada phase HELLO 20 detik
  bool isHelloFrame = inHelloPhaseSnapshot;
  bool inHelloSlot = SlotPlan::isInMyHelloSlot(_id24, posInHelloSnapshot);

  if (frameIdxSnapshot != _lastFrameIdx) {
    _lastFrameIdx = frameIdxSnapshot;
    _sentThisFrame        = false;
    _backlogSentThisFrame = false;
    _helloSentThisFrame   = false;
  }

  bool wasInHelloBefore = _wasInHelloPhase;

  _wasInHelloPhase = isHelloFrame;

  if (!isHelloFrame && wasInHelloBefore) {
      finalizeHelloFrameCollection(frameIdxSnapshot);
  }

  // Evaluasi parent di awal setiap FRAME DATA (bukan frame HELLO).
  if (!isHelloFrame && inDataSlotSnapshot && frameIdxSnapshot != _lastRouteEvalFrameIdx) {
    _lastRouteEvalFrameIdx = frameIdxSnapshot;
    _routeDirty = true;   // defer execution
    debugPrintRoutingTable(frameIdxSnapshot);
  }

  // Clear neighbor table HANYA pada tepi transisi DATA -> HELLO (sekali saja).
  // Menggunakan wasInHelloBefore agar deteksi tepi tetap bekerja
  // meskipun _wasInHelloPhase sudah diupdate di atas.
  if (_neighborCollectionFrameIdx != frameIdxSnapshot) {
      _neighborCollectionFrameIdx = frameIdxSnapshot;
      beginHelloFrameCollection(frameIdxSnapshot);
  }

  // -------------------------
  // 3A) FRAME HELLO (routing gossip, tanpa origin/backlog)
  // -------------------------
  if (isHelloFrame) {
    // GLOBAL TX GUARD
    bool helloTxBlocked =
        (_waitingAck || _inFlight || _backlogInFlight);

    if (!helloTxBlocked &&
        inHelloSlot &&
        !_helloSentThisFrame) {
      // battPct di HELLO sekarang dummy saja (jaringan tidak pakai ini)
      uint8_t battPct = 0;

      String aliasSelf   = StaticRoute::aliasForId(_id24);
      String parentAlias = StaticRoute::aliasForId(_parentId24);

      // Hop yang akan diiklankan:
      // - default = hop statis (anti-korup)
      // - kalau _hopToSink sudah valid (bukan 255), pakai hop dinamis
      uint8_t hopAdvertised =
        (_parentId24 == 0 || _hopToSink >= 250)
        ? 255
        : _hopToSink;

      // Layout HELLO:
      //  proto(10), routeVer, srcIdHex, aliasSelf, parentAlias,
      //  hopToSink, frameIdx, posInFrame, battPct, neighCount, [neighbors.]
      String pkt;
      pkt.reserve(64);
      pkt  = MeshProto::ROUTE_HELLO;              // proto = "10"
      pkt += ",";
      pkt += String((int)DataWire::ROUTE_VER);    // routeVer
      pkt += ",";
      pkt += _idHex6;                             // srcIdHex
      pkt += ",";
      pkt += aliasSelf;                           // aliasSelf
      pkt += ",";
      pkt += parentAlias;                         // parentAlias
      pkt += ",";
      pkt += String((int)hopAdvertised);          // hopToSink
      pkt += ",";
      pkt += String((unsigned long)frameIdxSnapshot);     // frameIdx
      pkt += ",";
      pkt += String((int)posInCycleSnapshot);            // posInCycle superframe
      pkt += ",";
      pkt += String((int)battPct);                // battPct

      pkt += ",";

      // explicit advertised role
      NetRuntime::DeviceRole myRole = NetRuntime::DeviceRole::NODE;

      if (NetRuntime::hasConfig()) {
        const NetRuntime::DeviceEntry* selfDev =
            NetRuntime::findDeviceById(_id24);

        if (selfDev) {
          myRole = selfDev->role;
        }
      }

      pkt += String((int)myRole);

      // ==== Neighbor gossip ====
      int totalUsed = 0;
      for (int i = 0; i < MAX_NEIGHBORS; ++i) {
        if (_neighbors[i].used) totalUsed++;
      }
      if (totalUsed > MAX_HELLO_NEIGHBORS) totalUsed = MAX_HELLO_NEIGHBORS;

      pkt += ",";
      pkt += String((int)totalUsed);              // neighCount

      int sent = 0;
      for (int i = 0; i < MAX_NEIGHBORS && sent < totalUsed; ++i) {
        if (!_neighbors[i].used) continue;

        pkt += ",";
        pkt += _neighbors[i].alias;                  // neighAlias_i
        pkt += ",";
        pkt += String((int)_neighbors[i].hopToSink); // neighHop_i
        pkt += ",";
        pkt += String((int)_neighbors[i].lastRssi);  // neighRssi_i

        sent++;
      }

      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return; // HARD LOCK: cegah TX storm
        }
        Lora.print(pkt);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _helloSentThisFrame = true;
      }

      Serial.print("[NODE HELLO TX] ");
      Serial.println(pkt);

      blinkStatus();
    }

    // Di frame HELLO:
    // - tidak kirim origin DATA
    // - tidak kirim backlog
    // Forwarding dari child tetap berjalan di bagian atas loop.
    return;
  }

  // -------------------------
  // 3B) FRAME DATA normal
  // -------------------------

  // 3B-1) Origin utama 1x per frame
  if (!_waitingAck) {
    static uint32_t _lastNoParentFrameLog = 0;

    if (_parentId24 == 0 || _hopToSink >= 250) {

    if (frameIdxSnapshot != _lastNoParentFrameLog) {
        _lastNoParentFrameLog = frameIdxSnapshot;
        Serial.printf("[NODE ORIGIN] frame=%lu paused: no parent\n", frameIdxSnapshot);
      }

    }else if (inDataSlotSnapshot && !_sentThisFrame) {
      uint16_t seqToSend = _seq;
      uint32_t tsOrigin  = unixNowSnapshot;

      // Payload dibangun di luar (di .ino) lewat hook _payloadBuilder.
      String payload;
      if (_payloadBuilder) {
      payload = _payloadBuilder(unixNowSnapshot, seqToSend);
      } else {
        // Tidak ada builder → payload sengaja dikosongkan.
        payload = "";
      }

      // Parent untuk DATA origin: gunakan parent dinamis/statis yang sekarang
      StaticRoute::Id24 parentForWire = _parentId24;
      uint8_t hopForWire = _hopToSink;

      String pkt = DataWire::buildNodeData(
        _idHex6, _id24, parentForWire,
        seqToSend, tsOrigin, frameIdxSnapshot, posInCycleSnapshot,
        payload
      );

      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return; // HARD LOCK: cegah TX storm
        }
        Lora.print(pkt);
        Lora.print('\n');
        releaseTx();
       _lastTxMs = millis();
        _lastAnyTxGuardMs = millis();
      }

      if (_txCb) _txCb((uint16_t)seqToSend, false);

      Serial.print("[NODE TX] ");
      Serial.println(pkt);

      _waitingAck    = true;
      _ackStartMs    = now;
      _lastTxMs      = now;
      _retry         = 0;
      _sentThisFrame = true;
      blinkStatus();

      _seq++;

      _lastOriginFrame    = pkt;
      _lastOriginRetryPacket = pkt;

      char seqBuf[5];
      snprintf(seqBuf, sizeof(seqBuf), "%04X", (unsigned int)seqToSend);

      _lastOriginSeqHex = String(seqBuf);

      markForwarded(_idHex6, _lastOriginSeqHex);

      _hasLastOriginFrame = true;
    }
  } else {
    // === FAST SELF-HEALING ===
    // Retry hanya 1 kali dalam sebuah frame → digunakan sepenuhnya
    // untuk fast self-healing (ganti parent sementara, kirim ulang).

    if (!(_inFlight || _backlogInFlight)) {
      if (isDataTxPhase()) {
        if (now - _ackStartMs >= ACK_TIMEOUT_MS_NODE) {

          if (_retry == 0) {
            // --- RETRY 1: Fast Self-Healing ---
            _retry++;
            _parentSuspected   = true;
            _healingFromParent = _parentId24; // simpan parent asal

            uint16_t seqToSend = (_seq == 0) ? 0xFFFF : (uint16_t)(_seq - 1);

            // Cari parent alternatif terbaik selain parent saat ini
            StaticRoute::Id24 altParent =
                findAlternateParentExcluding(_parentId24);

            if (altParent != 0 && altParent != _parentId24) {
              // --- Ganti parent sementara ---
              _parentId24  = altParent;
              _parentHex6  = StaticRoute::hex24(altParent);

              const char* altAliasC = StaticRoute::aliasForId(altParent);
              _parentAlias = altAliasC ? String(altAliasC) : String("");

              for (int i = 0; i < MAX_NEIGHBORS; ++i) {
                if (_neighbors[i].used && _neighbors[i].id24 == altParent) {
                  _hopToSink = (uint8_t)(_neighbors[i].hopToSink + 1);
                  break;
                }
              }

              Serial.printf("[ORIGIN HEAL] suspected=%s -> alt=%s\n",
                StaticRoute::aliasForId(_healingFromParent),
                _parentAlias.c_str());

              // Kemas ulang paket dengan parentAlias baru
              {
                const int MAXF_H = DataWire::NUM_FIELDS;
                String hf[MAXF_H];
                int    hfc = 0, hs = 0;

                for (int i = 0; i <= _lastOriginRetryPacket.length(); ++i) {
                  if (i == _lastOriginRetryPacket.length() ||
                      _lastOriginRetryPacket[i] == ',')
                  {
                    if (hfc < MAXF_H)
                      hf[hfc++] = _lastOriginRetryPacket.substring(hs, i);
                    hs = i + 1;
                  }
                }

                if (hfc > DataWire::IDX_PARENT_ALIAS && _parentAlias.length()) {
                  hf[DataWire::IDX_PARENT_ALIAS] = _parentAlias;
                  _lastOriginRetryPacket = joinCsv(hf, hfc);
                }
              }

              if (canTransmitNow() && requestTx()) {
                if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
                  releaseTx();
                  return;
                }
                Lora.print(_lastOriginRetryPacket);
                Lora.print('\n');
                releaseTx();
                _lastTxMs = millis();
                _lastAnyTxGuardMs = millis();
              }

              Serial.printf("[ORIGIN HEAL TX] retry=%u alt=%s pkt=%s\n",
                            _retry,
                            _parentAlias.c_str(),
                            _lastOriginRetryPacket.c_str());

              if (_txCb) _txCb(seqToSend, true);

            } else {
              // Tidak ada alternatif: kirim ulang ke parent yang sama
              Serial.printf("[ORIGIN HEAL] no alt, retry same parent=%s\n",
                            _parentAlias.c_str());

              if (canTransmitNow() && requestTx()) {
                if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
                  releaseTx();
                  return;
                }
                Lora.print(_lastOriginRetryPacket);
                Lora.print('\n');
                releaseTx();
                _lastTxMs = millis();
                _lastAnyTxGuardMs = millis();
              }

              if (_txCb) _txCb(seqToSend, true);
            }

            _ackStartMs = now;
            _lastTxMs   = now;
            blinkStatus();

          } else {
            // --- Retry habis: konfirmasi parent rusak ---
            // Jika healing sudah dilakukan (pindah ke alt) tapi tetap gagal,
            // blacklist parent asal (_healingFromParent).
            if (_parentSuspected &&
                _healingFromParent != 0 &&
                _healingFromParent != _parentId24)
            {
              _brokenParentId24    = _healingFromParent;
              _brokenParentPending = true; // frameIdx diselesaikan di chooseBestParent
              Serial.printf("[ORIGIN HEAL] blacklist pending broken=%s\n",
                            StaticRoute::aliasForId(_healingFromParent));
            }

            // Penalti ke parent saat ini (alt yang juga gagal)
            for (int i = 0; i < MAX_NEIGHBORS; ++i) {
              if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
                _neighbors[i].stats.failCount++;
                break;
              }
            }
            _parentAckFailStreak++;
            if (_parentAckFailStreak > 10) _parentAckFailStreak = 10;

            Serial.println("[NODE HEAL] Retry exhausted, drop frame this slot");
            _waitingAck        = false;
            _retry             = 0;
            _parentSuspected   = false;
            _healingFromParent = 0;
          }
        }
      }
    }
  }

  // 3B-2) Backlog (DATA yang nyangkut karena parent sebelumnya gagal)
  if (!isHelloFrame &&
      !_backlogInFlight &&
      !_waitingAck &&
      !_inFlight &&
      _bCount > 0 &&
      _parentId24 != 0 &&
      (nowMs - _backlogAckStartMs > 3000))
  {
    BacklogEntry &BE = _bq[_bHead];

    // Cek apakah boleh kirim di frame ini (TTL, tail slot origin, dll)
    if (frameIdxSnapshot >= _nextBacklogFrameIdxAllowed &&
        canSendBacklogNow(BE, unixNowSnapshot, frameIdxSnapshot, posInCycleSnapshot)){
      if (canTransmitNow() && requestTx()) {
        if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
          releaseTx();
          return; // HARD LOCK: cegah TX storm
        }
        Lora.print(BE.frame);
        Lora.print('\n');
        releaseTx();
        _lastTxMs = millis();
        _lastAnyTxGuardMs = millis();
      }

      _backlogInFlight       = true;
      _backlogRetry          = 0;
      _backlogAckStartMs     = now;
      _backlogFrameIdx       = frameIdxSnapshot;
      _backlogSentThisFrame  = true;
      _backlogSeqInFlight    = BE.seqHex;

      Serial.print("[NODE-BACKLOG TX] ");
      Serial.println(BE.frame);
      blinkStatus();
    }
  }
  // 3B-3) Backlog ACK handler (tetap seperti sebelumnya)
  else if (!isHelloFrame &&
           _backlogInFlight &&
           _backlogSeqInFlight.length())
  {
    uint32_t now = millis();
    if (isDataTxPhase() && now - _backlogAckStartMs >= ACK_TIMEOUT_MS_NODE) {
      if (_backlogRetry < MAX_RETRY_NODE) {
        // Retry backlog
        BacklogEntry &BE = _bq[_bHead];
        if (canTransmitNow() && requestTx()) {
          if (millis() - _lastAnyTxGuardMs < TX_GLOBAL_GAP_MS) {
            releaseTx();
            return; // HARD LOCK: cegah TX storm
          }
          Lora.print(BE.frame);
          Lora.print('\n');
          releaseTx();
          _lastTxMs = millis();
          _lastAnyTxGuardMs = millis();
        }

        _backlogRetry++;
        _backlogAckStartMs = millis();
        _backlogFrameIdx   = frameIdxSnapshot;

        Serial.print("[NODE-BACKLOG RETRY ");
        Serial.print(_backlogRetry);
        Serial.print("] ");
        Serial.println(BE.frame);
        blinkStatus();
      } else {
        // Gagal di frame ini, coba frame berikutnya
        _backlogInFlight            = false;
        _backlogRetry               = 0;
        _nextBacklogFrameIdxAllowed = frameIdxSnapshot + 1;

        Serial.println("[NODE-BACKLOG] give up this frame, retry next frame");
      }
    }
  }
  // =========================
  // ROUTING SAFE EXECUTION (NON BLOCKING)
  // =========================
  if (_routeDirty &&
      !isHelloFrame &&
      !_waitingAck &&
      !_inFlight &&
      !_backlogInFlight &&
      (nowMs - _lastAnyTxGuardMs > 250)) {

    _routeDirty = false;
    chooseBestParentFromNeighbors(_lastRouteEvalFrameIdx);
  }
}

// ======================================================
// 5. TX CONTROL LAYER
// ======================================================
// TX control: gate and arbitration for transmission access

bool MeshNode::canTransmitNow() const {
    uint32_t now = millis();

    // enforce minimum gap between transmissions
    if (now - _lastTxMs < MIN_TX_GAP_MS) return false;
    return true;
}

bool MeshNode::requestTx() {
  uint32_t now = millis();

  // prevent overlapping transmission sessions
  if (_txBusy) {
    // recover from stuck TX state if timeout exceeded
    if (now - _lastTxStartMs < TX_BUSY_TIMEOUT_MS) return false;
    _txBusy = false;
  }

  _txBusy = true;
  _lastTxStartMs = now;

  return true;
}

void MeshNode::releaseTx() {
  _txBusy = false;
}

// ======================================================
// 6. DATA WINDOW / TIMING LOGIC
// ======================================================

// Data window mapping for source based on runtime config or fallback plan
bool findDataWindowForSource(StaticRoute::Id24 srcId,
                             uint8_t& startSecOut,
                             uint8_t& endSecOut) {
  // primary source: runtime slot configuration
  if (NetRuntime::hasConfig() && NetRuntime::current().dataSlotCount > 0) {
    const auto& cfg = NetRuntime::current();

    for (size_t i = 0; i < cfg.dataSlotCount; ++i) {
      const auto& ds = cfg.dataSlots[i];

      if (ds.ownerId == srcId) {
        startSecOut = ds.startSec;
        endSecOut = (uint8_t)((ds.startSec + ds.durationSec) % SlotPlan::frameLenSec());
        return true;
      }
    }
  } else {
    // fallback static slot plan
    const SlotPlan::SlotInfo* si = SlotPlan::findSlotFallback(srcId);

    if (si) {
      startSecOut = si->startSec;
      endSecOut = si->endSec;
      return true;
    }
  }
  return false;
}

// Check if current position is inside tail region of assigned data window
bool isInTailOfDataWindow(StaticRoute::Id24 srcId,
                          uint32_t unixNow,
                          uint8_t tailSec) {
  // invalid tail configuration
  if (tailSec == 0) return false;

  uint32_t frameLen = SlotPlan::frameLenSec();

  // invalid frame length
  if (frameLen == 0) return false;

  uint8_t pos = (uint8_t)(unixNow % frameLen);

  uint8_t startSec = 0;
  uint8_t endSec = 0;

  // resolve slot window first
  if (!findDataWindowForSource(srcId, startSec, endSec)) return false;

  bool wrapped = endSec <= startSec;
  uint8_t tailStart;

  // normal window (no wrap-around)
  if (!wrapped) {
    if (pos < startSec || pos >= endSec) return false;
    tailStart = (uint8_t)(endSec - tailSec);
    if (tailStart < startSec) tailStart = startSec;
  } else {
    // wrapped window case
    bool inWindow = (pos >= startSec || pos < endSec);

    if (!inWindow) return false;
    if (pos >= startSec) {
      tailStart = (uint8_t)((endSec + frameLen - tailSec) % frameLen);
    } else {
      tailStart = (uint8_t)(endSec - tailSec);
    }
  }

  // final tail check
  return pos >= tailStart;
}

// ======================================================
// 7. BACKLOG / QUEUE MANAGEMENT
// ======================================================

// Upstream transmission queue (FIFO)
void MeshNode::enqueueUp(const String& frame, const String& seqHex) {
  // drop if queue is full
  if (_count >= QSIZE) {
    Serial.println("[NODE-FWD] Up-queue full, drop frame");
    return;
  }

  _q[_tail].frame = frame;
  _q[_tail].seqHex = seqHex;

  _tail = (_tail + 1) % QSIZE;
  _count++;
}


// Backlog storage with metadata extraction from frame header
void MeshNode::pushBacklog(const String& frame, const String& seqHex) {
  // ignore empty frame
  if (!frame.length()) return;

  // overwrite oldest if backlog full
  if (_bCount >= BQSIZE) {
    _bHead = (_bHead + 1) % BQSIZE;
    _bCount--;
  }

  BacklogEntry &be = _bq[_bTail];

  be.frame  = frame;
  be.seqHex = seqHex;

  // reset metadata
  be.srcId24 = 0;
  be.slotId = 0;
  be.createdFrameIdx = 0;
  be.lastHeardFrameIdx = 0;
  be.lastHeardPos = 0;

  // parse CSV-like frame header
  const int MAXF = DataWire::NUM_FIELDS;
  String f[MAXF];
  int fc = 0;
  int st = 0;

  for (int i = 0; i <= frame.length(); ++i) {
    if (i == frame.length() || frame[i] == ',') {
      if (fc < MAXF) f[fc++] = frame.substring(st, i);
      st = i + 1;
    }
  }

  // extract source ID
  if (fc > DataWire::IDX_SRC_ID) {
    String srcHex = f[DataWire::IDX_SRC_ID];
    srcHex.trim();
    be.srcId24 = StaticRoute::parseHex24(srcHex);
  }

  // extract slot ID
  if (fc > DataWire::IDX_SLOT_ID) {
    String slotS = f[DataWire::IDX_SLOT_ID];
    slotS.trim();
    be.slotId = (uint8_t)slotS.toInt();
  }

  // extract frame index
  if (fc > DataWire::IDX_FRAME_IDX) {
    String frS = f[DataWire::IDX_FRAME_IDX];
    frS.trim();
    be.createdFrameIdx = (uint32_t)frS.toInt();
  }

  _bTail = (_bTail + 1) % BQSIZE;
  _bCount++;

  Serial.print("[NODE-BACKLOG] store seq=");
  Serial.print(seqHex);
  Serial.print(" srcId=");
  Serial.println(StaticRoute::hex24(be.srcId24));
}


// Backlog transmission eligibility check
bool MeshNode::canSendBacklogNow(const BacklogEntry& be,
                                 uint32_t unixNow,
                                 uint32_t frameIdx,
                                 uint8_t posInFrame) const {
  // basic TTL protection (hard limit)
  if ((frameIdx - be.createdFrameIdx) > 10) return false;

  // configurable backlog TTL window
  if (be.createdFrameIdx != 0 &&
      frameIdx > be.createdFrameIdx &&
      (frameIdx - be.createdFrameIdx) >= NetReliability::BACKLOG_TTL_FRAMES) {
    return false;
  }

  // prevent backward frame replay
  if (be.lastHeardFrameIdx != 0 && frameIdx < be.lastHeardFrameIdx) {
    return false;
  }

  uint8_t tailSec = NetReliability::BACKLOG_TAIL_SEC;

  // must be inside tail window of source slot
  if (!isInTailOfDataWindow(be.srcId24, unixNow, tailSec)) return false;

  // allow forward progress only
  if (posInFrame <= be.lastHeardPos && be.lastHeardPos != 0) {
    return false;
  }

  return true;
}

// ======================================================
// 8. NEIGHBOR MANAGEMENT
// ======================================================

// Reset entire neighbor table state
void MeshNode::clearNeighbors() {
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    _neighbors[i].id24          = 0;
    _neighbors[i].alias         = "";
    _neighbors[i].lastRssi     = 0;
    _neighbors[i].hopToSink    = 255;
    _neighbors[i].lastSeenFrame = 0;
    _neighbors[i].lastSeenMs    = 0;

    _neighbors[i].seenThisHelloFrame = false;
    _neighbors[i].helloFrameIdx      = 0;
    _neighbors[i].used         = false;
    _neighbors[i].role         = NetRuntime::DeviceRole::NODE;

    _neighbors[i].stats.txCount   = 0;
    _neighbors[i].stats.ackCount  = 0;
    _neighbors[i].stats.failCount = 0;
    _neighbors[i].stats.rssiAvg   = 0;
    _neighbors[i].stats.rssiVar   = 0;
    _neighbors[i].stats.retrySum  = 0;
  }
}

void MeshNode::beginHelloFrameCollection(uint32_t frameIdx) {

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {

    if (!_neighbors[i].used)
      continue;

    _neighbors[i].seenThisHelloFrame = false;

    _neighbors[i].helloFrameIdx = frameIdx;
  }

  Serial.printf(
    "[HELLO FRAME BEGIN] frame=%lu\n",
    (unsigned long)frameIdx
  );
}

void MeshNode::finalizeHelloFrameCollection(uint32_t frameIdx) {

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {

    if (!_neighbors[i].used)
      continue;

    // neighbor masih terlihat di frame ini
    if (_neighbors[i].seenThisHelloFrame)
      continue;

    Serial.printf(
      "[HELLO PURGE] alias=%s id=%06lX frame=%lu\n",
      _neighbors[i].alias.c_str(),
      (unsigned long)_neighbors[i].id24,
      (unsigned long)frameIdx
    );

    _neighbors[i].used = false;

    _neighbors[i].id24 = 0;

    _neighbors[i].alias = "";

    _neighbors[i].lastRssi = 0;

    _neighbors[i].hopToSink = 255;

    _neighbors[i].lastSeenFrame = 0;
    _neighbors[i].lastSeenMs = 0;

    _neighbors[i].seenThisHelloFrame = false;

    _neighbors[i].helloFrameIdx = 0;

    _neighbors[i].role =
      NetRuntime::DeviceRole::NODE;
  }

  Serial.printf(
    "[HELLO FRAME FINALIZE] frame=%lu\n",
    (unsigned long)frameIdx
  );
}

// Update or insert neighbor entry from HELLO packet
void MeshNode::updateNeighborFromHello(StaticRoute::Id24 nid,
                                       const String& alias,
                                       NetRuntime::DeviceRole role,
                                       uint8_t hopToSink,
                                       int16_t rssi_dBm,
                                       uint32_t frameIdx) {
  // ignore invalid or self node
  if (nid == 0 || nid == _id24) return;

  int freeIdx = -1;
  int foundIdx = -1;

  // search existing entry or free slot
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used) {
      if (_neighbors[i].id24 == nid) {
        foundIdx = i;
        break;
      }
    } else if (freeIdx < 0) {
      freeIdx = i;
    }
  }

  int idx = foundIdx;

  // allocate slot if not found
  if (idx < 0) {
    if (freeIdx >= 0) {
      idx = freeIdx;
    } else {
      // replace oldest entry if table is full
      uint32_t worstAge = 0;
      int16_t worstRssi = 32767;
      idx = 0;

      for (int i = 0; i < MAX_NEIGHBORS; ++i) {

        uint32_t age =
          millis() - _neighbors[i].lastSeenMs;

        int16_t rssi =
          _neighbors[i].lastRssi;

        bool worse =
          (age > worstAge) ||
          (age == worstAge && rssi < worstRssi);

        if (worse) {
          worstAge = age;
          worstRssi = rssi;
          idx = i;
        }
      }
    }
  }

  // update neighbor metadata
  bool metricChanged = false;

  if (_neighbors[idx].used) {

    if (_neighbors[idx].lastRssi != rssi_dBm)
      metricChanged = true;

    if (_neighbors[idx].hopToSink != hopToSink)
      metricChanged = true;

    if (_neighbors[idx].role != role)
      metricChanged = true;
  }

  _neighbors[idx].used = true;
  _neighbors[idx].id24 = nid;
  _neighbors[idx].alias = alias;
  _neighbors[idx].role = role;
  _neighbors[idx].lastRssi = rssi_dBm;
  _neighbors[idx].hopToSink = hopToSink;
  _neighbors[idx].lastSeenFrame = frameIdx;
  _neighbors[idx].lastSeenMs = millis();
  _neighbors[idx].seenThisHelloFrame = true;
  _neighbors[idx].helloFrameIdx = frameIdx;

  if (metricChanged) {

    Serial.printf(
      "[HELLO UPDATE] alias=%s rssi=%d hop=%u role=%d\n",
      alias.c_str(),
      rssi_dBm,
      (unsigned)hopToSink,
      (int)role
    );
  }

  // RSSI exponential moving average
  float alpha = 0.2f;

  float prevAvg = _neighbors[idx].stats.rssiAvg;
  float newRssi = (float)rssi_dBm;

  _neighbors[idx].stats.rssiAvg =
    (prevAvg == 0) ? newRssi
    : (0.8f * prevAvg) + (0.2f * newRssi);

  // Simpan RSSI terakhir parent untuk penalty calculation
  if (_neighbors[idx].id24 == _parentId24) {
      _parentLastRssi = newRssi;
  }

  // RSSI variance tracking (EMA)
  float diff = newRssi - _neighbors[idx].stats.rssiAvg;

  _neighbors[idx].stats.rssiVar = 
    (1.0f - alpha) * _neighbors[idx].stats.rssiVar +
    alpha * diff * diff;
}


// Count active neighbors
uint8_t MeshNode::neighborCount() const {
  uint8_t count = 0;

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used) ++count;
  }

  return count;
}


// Debug print neighbor table state
void MeshNode::debugPrintNeighbors() const
{
  Serial.print("[NODE NEIGH] ");
  Serial.print(_idHex6);
  Serial.print(" alias=");
  Serial.print(StaticRoute::aliasForId(_id24));
  Serial.print(" neighbors=");

  for (int i = 0; i < MAX_NEIGHBORS; ++i){
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

void MeshNode::debugPrintRoutingTable(uint32_t frameIdx) const
{
  Serial.println();
  Serial.println("==================================================");

  Serial.printf(
    "[ROUTING TABLE] frame=%lu self=%s parent=%s hop=%u\n",
    (unsigned long)frameIdx,
    _idHex6.c_str(),
    _parentAlias.c_str(),
    (unsigned)_hopToSink
  );

  Serial.println("--------------------------------------------------");

  Serial.println(
    "ALIAS | HOP | RSSI | RSSI_AVG | VAR | TX | ACK | FAIL | RETRY | BASE | EFFECT | AGE"
  );

  Serial.println("--------------------------------------------------");

  uint32_t now = millis();

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {

    if (!_neighbors[i].used)
      continue;

    const NeighborInfo& n = _neighbors[i];

    float baseScore =
      computeLinkScore(n);

    float effectiveScore =
      computeEffectiveRouteScore(n);

    uint32_t age =
      (now >= n.lastSeenMs)
      ? (now - n.lastSeenMs)
      : 0;

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
      effectiveScore,
      (unsigned long)age
    );
  }

  Serial.println("==================================================");
  Serial.println();
}

// Compute link quality score for routing decision
float MeshNode::computeLinkScore(const NeighborInfo& n) const {
  const LinkStats& s = n.stats;

  // --- RSSI (prioritas 3: dominan dalam score) ---
  float smoothedRssi = (s.rssiAvg != 0.0f)
    ? s.rssiAvg
    : (float)n.lastRssi;

  float rssiNorm = (smoothedRssi + 120.0f) / 70.0f;
  if (rssiNorm < 0.0f) rssiNorm = 0.0f;
  if (rssiNorm > 1.0f) rssiNorm = 1.0f;

  // --- Link quality (prioritas 4) ---

  // PSR: packet success ratio, range [0,1]
  float psr = (s.txCount > 5)
    ? (float)s.ackCount / (float)s.txCount
    : 0.7f;
  if (psr < 0.0f) psr = 0.0f;
  if (psr > 1.0f) psr = 1.0f;

  // ETX inverse: guard div-by-zero jika txCount=0 atau ackCount=0
  float etxInv;
  if (s.txCount > 0 && s.ackCount > 0) {
    float etx = (float)s.txCount / (float)s.ackCount;
    // etx > 0 dijamin karena txCount > 0; clamp atas agar tidak underflow
    etxInv = 1.0f / etx;   // == ackCount/txCount == psr, tapi dihitung independent
  } else {
    etxInv = 1.0f / 3.0f;  // default: asumsi ETX=3
  }
  if (etxInv < 0.0f) etxInv = 0.0f;
  if (etxInv > 1.0f) etxInv = 1.0f;

  // Stability dari variansi RSSI: guard nilai negatif (float drift)
  // Denominator selalu >= 1.0f sehingga hasil selalu dalam (0, 1]
  float rssiVar = s.rssiVar;
  if (rssiVar < 0.0f) rssiVar = 0.0f;
  float stability = 1.0f / (1.0f + rssiVar);

  // Bonus kontinuitas untuk parent aktif
  float stabilityBoost = (n.id24 == _parentId24) ? 0.08f : 0.0f;

  // Score akhir: RSSI dominan, link quality sekunder.
  // Total range natural: ~[0.0, 1.08] — tidak ada path menuju inf/nan.
  float score =
    0.60f * rssiNorm   +   // prioritas 3: RSSI
    0.20f * psr        +   // prioritas 4: delivery rate
    0.12f * etxInv     +   // prioritas 4: efisiensi transmisi
    0.08f * stability  +   // prioritas 4: stabilitas sinyal
    stabilityBoost;

  return score;
}

float MeshNode::computeEffectiveRouteScore(const NeighborInfo& n) const
{
  float base = computeLinkScore(n);

  float penalty = 0.0f;

  if (n.id24 == _parentId24) {

    penalty += _parentAckFailStreak * 0.08f;

    if (_parentLastRssi != 0) {

      float drop =
        (float)(_parentLastRssi - n.lastRssi);

      if (drop > 5)
        penalty += drop * 0.01f;
    }

    penalty += n.stats.failCount * 0.03f;

    if (penalty > 0.6f)
      penalty = 0.6f;
  }

  return base - penalty;
}

// ======================================================
// 9. ROUTING ENGINE
// ======================================================

// Routing decision engine: selects and maintains best parent node
void MeshNode::chooseBestParentFromNeighbors(uint32_t frameIdx) {
  using StaticRoute::Id24;
  using NetRuntime::DeviceRole;

  // Tidak ada hold/cooldown. Parent berubah hanya pada 2 event:
  // 1) Setelah tabel neighbor fresh dari fase hello (di sini).
  // 2) Saat fast self-healing tertrigger (di loop() retry block).

  static constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 90000UL;
  static constexpr float    HYSTERESIS_MARGIN   = 0.35f; // anti micro-oscillation

  int16_t RSSI_MIN_PARENT = NetRuntime::rssiMinParent();
  if (RSSI_MIN_PARENT < -120) RSSI_MIN_PARENT = -120;

  uint32_t now = millis();

  // -----------------------------
  // 0. RESOLVE BLACKLIST PENDING
  // Setelah healing, frameIdx baru tersedia di sini.
  // -----------------------------
  if (_brokenParentPending && _brokenParentId24 != 0) {
    _brokenParentUntilFrameIdx = frameIdx + 2;
    _brokenParentPending       = false;
    Serial.printf("[ROUTE] blacklist parent=%s until frame=%lu\n",
                  StaticRoute::aliasForId(_brokenParentId24),
                  (unsigned long)_brokenParentUntilFrameIdx);
  }

  // -----------------------------
  // 1. EXPIRE STALE NEIGHBORS
  // (Biasanya sudah bersih karena clearNeighbors di hello phase,
  //  tapi guard ini tetap dipertahankan untuk keamanan.)
  // -----------------------------
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;

    if (now - _neighbors[i].lastSeenMs > NEIGHBOR_TIMEOUT_MS) {
      Serial.print("[NEIGHBOR EXPIRE] ");
      Serial.println(_neighbors[i].alias);
      _neighbors[i].used = false;
    }
  }

  // -----------------------------
  // 2. PARENT PRESENCE CHECK
  // Jika parent tidak muncul di hello phase (tidak ada di tabel),
  // langsung hapus tanpa grace period.
  // -----------------------------
  bool parentFound = false;
  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (_neighbors[i].used && _neighbors[i].id24 == _parentId24) {
      parentFound = true;
      break;
    }
  }

  if (_parentId24 != 0 && !parentFound) {
    Serial.printf("[NODE ROUTE] parent not in fresh table, clear -> %s\n",
                  _parentAlias.c_str());
    _parentId24          = 0;
    _parentHex6          = "";
    _parentAlias         = "";
    _hopToSink           = 255;
    _parentLostSinceMs   = 0;
    _parentAckFailStreak = 0;
    _parentDegradeScore  = 0.0f;
  } else if (parentFound) {
    _parentLostSinceMs = 0;
  }

  // -----------------------------
  // 3. SNAPSHOT PARENT SAAT INI
  // -----------------------------
  int16_t currentParentRssi  = -32768;
  bool    parentStillValid   = false;
  float   currentScore       = -9999.0f;
  uint8_t currentHop         = (_hopToSink == 255) ? 254 : _hopToSink;
  uint8_t currentRolePriority = 255; // 0=SINK, 1=RELAY, 2=NODE

  if (_parentId24 != 0) {
    for (int i = 0; i < MAX_NEIGHBORS; ++i) {
      if (!_neighbors[i].used) continue;
      if (_neighbors[i].id24 != _parentId24) continue;

      currentParentRssi = _neighbors[i].lastRssi;
      parentStillValid  = (now - _neighbors[i].lastSeenMs <= NEIGHBOR_TIMEOUT_MS);

      float raw = computeLinkScore(_neighbors[i]);
      if (isnan(raw) || isinf(raw)) raw = -9999.0f;
      raw = (raw > 1000.0f) ? 1000.0f : (raw < -1000.0f) ? -1000.0f : raw;
      currentScore = raw - _parentDegradeScore;

      switch (_neighbors[i].role) {
        case DeviceRole::SINK:  currentRolePriority = 0; break;
        case DeviceRole::RELAY: currentRolePriority = 1; break;
        default:                currentRolePriority = 2; break;
      }
      break;
    }
  }

  // -----------------------------
  // 4. SCAN KANDIDAT TERBAIK
  // Skip: RSSI lemah, failCount tinggi, hop tidak valid, diblacklist.
  // -----------------------------
  Id24    bestId           = 0;
  uint8_t bestHop          = 255;
  int16_t bestRssi         = -32768;
  float   bestScore        = -9999.0f;
  uint8_t bestRolePriority = 255;

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {
    if (!_neighbors[i].used) continue;

    auto &n = _neighbors[i];

    if (n.lastRssi < RSSI_MIN_PARENT)  continue;
    if (n.stats.failCount > 8)          continue;
    if (n.hopToSink >= 250)             continue;

    // Skip parent yang masih diblacklist
    if (n.id24 == _brokenParentId24 &&
        frameIdx < _brokenParentUntilFrameIdx)
    {
      Serial.printf("[ROUTE BLACKLIST] skip=%s until frame=%lu (now=%lu)\n",
                    n.alias.c_str(),
                    (unsigned long)_brokenParentUntilFrameIdx,
                    (unsigned long)frameIdx);
      continue;
    }

    uint8_t rolePriority = 255;
    switch (n.role) {
      case DeviceRole::SINK:  rolePriority = 0; break;
      case DeviceRole::RELAY: rolePriority = 1; break;
      default:                rolePriority = 2; break;
    }

    float base    = computeLinkScore(n);
    float penalty = 0.0f;

    // Degradasi untuk parent saat ini berdasarkan riwayat kegagalan
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
    if (bestId == 0) {
      betterCandidate = true;
    } else if (rolePriority < bestRolePriority) {
      betterCandidate = true;
    } else if (rolePriority == bestRolePriority) {
      if (n.hopToSink < bestHop) {
        betterCandidate = true;
      } else if (n.hopToSink == bestHop && score > bestScore) {
        betterCandidate = true;
      }
    }

    if (!betterCandidate) continue;

    bestRolePriority = rolePriority;
    bestScore        = score;
    bestId           = n.id24;
    bestHop          = n.hopToSink;
    bestRssi         = n.lastRssi;
  }

  // -----------------------------
  // 5. TIDAK ADA KANDIDAT VALID
  // -----------------------------
  if (bestId == 0) {
    Serial.println("[NODE ROUTE] NO VALID PARENT in neighbor table");
    _parentId24 = 0;
    _parentHex6 = "";
    _parentAlias = "";
    _hopToSink  = 255;
    return;
  }

  uint8_t newHop = bestHop + 1;

  Serial.println();
  Serial.println("--------------- ROUTE DECISION ----------------");
  Serial.printf("best=%s score=%.2f hop=%u rssi=%d\n",
                StaticRoute::aliasForId(bestId), bestScore, bestHop, bestRssi);
  Serial.printf("current=%s hop=%u rssi=%d score=%.2f\n",
                _parentAlias.c_str(), currentHop, currentParentRssi, currentScore);
  Serial.printf("ackFail=%u degrade=%.2f blacklist=%s untilF=%lu\n",
                _parentAckFailStreak, _parentDegradeScore,
                StaticRoute::aliasForId(_brokenParentId24),
                (unsigned long)_brokenParentUntilFrameIdx);
  Serial.println("-----------------------------------------------");

  // -----------------------------
  // 6. KEPUTUSAN SWITCH / HOLD
  // Prioritas: Role > Hop > Score (konsisten dengan scan kandidat step 4).
  // Hysteresis hanya berlaku pada level score (prioritas 3) untuk
  // cegah micro-oscillation antar kandidat setara role dan hop.
  // -----------------------------
  bool shouldSwitch =
      (_parentId24 == 0) ||
      (bestRolePriority < currentRolePriority) ||
      (bestRolePriority == currentRolePriority && newHop < currentHop) ||
      (bestRolePriority == currentRolePriority && newHop == currentHop &&
       bestScore > currentScore + HYSTERESIS_MARGIN);

  if (!shouldSwitch && _parentId24 != 0) {
    // Kandidat tidak lebih baik dari parent saat ini → pertahankan
    _hopToSink = currentHop;
    Serial.printf("[NODE ROUTE] frame=%lu HOLD parent=%s (no improvement)\n",
                  (unsigned long)frameIdx, _parentAlias.c_str());
    return;
  }

  // -----------------------------
  // 7. TERAPKAN PARENT BARU
  // -----------------------------
  const char* alias = StaticRoute::aliasForId(bestId);

  if (!alias || !alias[0]) {
    Serial.printf("[NODE ROUTE] reject: invalid alias id=%06lX\n",
                  (unsigned long)bestId);
    return;
  }

  bool isSwitch = (bestId != _parentId24);

  _parentId24 = bestId;
  _parentHex6 = StaticRoute::hex24(bestId);
  _parentAlias = String(alias);
  _hopToSink   = newHop;

  if (isSwitch) {
    _parentAckFailStreak = 0;
    _parentDegradeScore  = 0.0f;
  }

  Serial.printf("[NODE ROUTE] frame=%lu %s parent=%s hop=%u score=%.2f\n",
                (unsigned long)frameIdx,
                isSwitch ? "SWITCH" : "RECONFIRM",
                _parentAlias.c_str(),
                (unsigned)newHop,
                bestScore);
}

StaticRoute::Id24 MeshNode::chooseNextHopForPacket(const String& path){
  using StaticRoute::Id24;

  const bool dynamicOn = NetRuntime::isDynamicRoutingEnabled();

  int16_t RSSI_MIN_PARENT = NetRuntime::rssiMinParent();

  if (RSSI_MIN_PARENT < -120)
    RSSI_MIN_PARENT = -120;

  if (_parentId24 == 0)
    return 0;

  if (!path.length())
    return _parentId24;

  const char* parentAliasC =
    StaticRoute::aliasForId(_parentId24);

  String parentAlias =
    parentAliasC ? String(parentAliasC) : "";

  if (!parentAlias.length())
    return 0;

  if (!aliasInPath(path, parentAlias))
    return _parentId24;

  if (!dynamicOn) {
    Serial.print("[NODE ANTILOOP] dynamic=OFF ");
    Serial.println(path);
    return 0;
  }

  Id24 bestId        = 0;
  uint8_t bestNewHop = 255;
  int16_t bestRssi   = -32768;
  uint8_t bestRole   = 255;   // 0=SINK 1=RELAY 2=NODE

  Serial.println("[ANTILOOP SCAN]");
  Serial.println(path);

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {

    if (!_neighbors[i].used)
      continue;

    Id24    nid    = _neighbors[i].id24;
    String  nAlias = _neighbors[i].alias;
    int16_t rssi   = _neighbors[i].lastRssi;
    uint8_t hopDyn = _neighbors[i].hopToSink;

    if (!nAlias.length()) {
      Serial.println(" reject=emptyAlias");
      continue;
    }

    if (aliasInPath(path, nAlias)) {
      Serial.printf(" reject=%s alreadyInPath\n", nAlias.c_str());
      continue;
    }

    if (hopDyn >= 250) {
      Serial.printf(" reject=%s invalidHop=%u\n", nAlias.c_str(), hopDyn);
      continue;
    }

    if (rssi < RSSI_MIN_PARENT) {
      Serial.printf(" reject=%s weakRssi=%d\n", nAlias.c_str(), rssi);
      continue;
    }

    if (_neighbors[i].stats.failCount > 8) {
      Serial.printf(" reject=%s fail=%u\n",
                    nAlias.c_str(), _neighbors[i].stats.failCount);
      continue;
    }

    if (millis() - _neighbors[i].lastSeenMs > 90000UL) {
      Serial.printf(" reject=%s stale\n", nAlias.c_str());
      continue;
    }

    // --- Prioritas 1: Role ---
    uint8_t rolePriority = 2;
    switch (_neighbors[i].role) {
      case NetRuntime::DeviceRole::SINK:  rolePriority = 0; break;
      case NetRuntime::DeviceRole::RELAY: rolePriority = 1; break;
      default:                            rolePriority = 2; break;
    }

    uint8_t newHop = (uint8_t)(hopDyn + 1);

    Serial.printf(
      " candidate=%s role=%u hop=%u rssi=%d inPath=%d fail=%u\n",
      nAlias.c_str(),
      (unsigned)rolePriority,
      (unsigned)newHop,
      (int)rssi,
      aliasInPath(path, nAlias),
      _neighbors[i].stats.failCount
    );

    // --- Perbandingan: Role > Hop > RSSI ---
    bool better = false;

    if (!bestId) {
      better = true;
    } else if (rolePriority < bestRole) {
      better = true;
    } else if (rolePriority == bestRole) {
      if (newHop < bestNewHop) {
        better = true;
      } else if (newHop == bestNewHop && rssi > bestRssi) {
        better = true;
      }
    }

    if (better) {
      bestId     = nid;
      bestRole   = rolePriority;
      bestNewHop = newHop;
      bestRssi   = rssi;
    }
  }

  if (!bestId)
    return 0;

  return bestId;
}

StaticRoute::Id24 MeshNode::findAlternateParentExcluding(StaticRoute::Id24 excludeId) {
  using StaticRoute::Id24;

  static constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 90000UL;

  int16_t RSSI_MIN_PARENT = NetRuntime::rssiMinParent();
  if (RSSI_MIN_PARENT < -120) RSSI_MIN_PARENT = -120;

  Id24 bestId = 0;
  float bestScore    = -9999.0f;
  uint8_t bestHop    = 255;
  uint8_t bestRole   = 255;   // 0=SINK, 1=RELAY, 2=NODE (lebih kecil = lebih baik)

  uint32_t now = millis();

  Serial.println("[FAST FAILOVER SCAN]");

  for (int i = 0; i < MAX_NEIGHBORS; ++i) {

    if (!_neighbors[i].used) continue;

    Id24 nid = _neighbors[i].id24;

    if (nid == excludeId) continue;
    if (nid == _id24)     continue;

    if (_neighbors[i].hopToSink >= 250) continue;

    if (_neighbors[i].lastRssi < RSSI_MIN_PARENT)
      continue;

    if (_neighbors[i].stats.failCount > 8)
      continue;

    if (now - _neighbors[i].lastSeenMs > NEIGHBOR_TIMEOUT_MS)
      continue;

    int16_t rssi      = _neighbors[i].lastRssi;
    uint8_t hop       = _neighbors[i].hopToSink;
    uint8_t failCount = _neighbors[i].stats.failCount;

    if (rssi < -140 || rssi > 0) {
      Serial.printf(" reject=%s invalidRssi=%d\n",
                    _neighbors[i].alias.c_str(), rssi);
      continue;
    }

    if (hop >= 250) {
      Serial.printf(" reject=%s invalidHop=%u\n",
                    _neighbors[i].alias.c_str(), hop);
      continue;
    }

    // --- Prioritas 1: Role ---
    uint8_t rolePriority = 2; // default NODE
    switch (_neighbors[i].role) {
      case NetRuntime::DeviceRole::SINK:  rolePriority = 0; break;
      case NetRuntime::DeviceRole::RELAY: rolePriority = 1; break;
      default:                            rolePriority = 2; break;
    }

    float score = computeLinkScore(_neighbors[i]);
    // computeLinkScore sudah dijamin tidak menghasilkan inf/nan
    // setelah fix sebelumnya, tapi guard tetap dipertahankan.
    if (isnan(score) || isinf(score)) {
      Serial.printf(" reject=%s invalidScore\n",
                    _neighbors[i].alias.c_str());
      continue;
    }

    Serial.printf(
      " candidate=%s role=%u hop=%u rssi=%d score=%.2f fail=%u age=%lu\n",
      _neighbors[i].alias.c_str(),
      (unsigned)rolePriority, (unsigned)hop, (int)rssi,
      score, (unsigned)failCount,
      (unsigned long)(now - _neighbors[i].lastSeenMs)
    );

    // --- Perbandingan: Role > Hop > Score (RSSI+LinkQuality) ---
    bool better = false;

    if (bestId == 0) {
      better = true;
    } else if (rolePriority < bestRole) {
      better = true;                              // prioritas 1: role lebih baik
    } else if (rolePriority == bestRole) {
      if (hop < bestHop) {
        better = true;                            // prioritas 2: hop lebih kecil
      } else if (hop == bestHop && score > bestScore) {
        better = true;                            // prioritas 3+4: RSSI & link quality
      }
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


// ======================================================
// FORWARD DUPLICATE SUPPRESSION
// ======================================================

bool MeshNode::alreadyForwarded(const String& srcHex,
                                const String& seqHex)
{
  uint32_t now = millis();

  for (int i = 0; i < SEEN_FWD_SIZE; ++i) {

    if (_seenFwd[i].srcHex == srcHex &&
        _seenFwd[i].seqHex == seqHex)
    {
      // duplicate cache TTL
      if (now - _seenFwd[i].tsMs < 120000UL) {
        return true;
      }
    }
  }

  return false;
}

void MeshNode::markForwarded(const String& srcHex,
                             const String& seqHex)
{
  _seenFwd[_seenFwdPos].srcHex = srcHex;
  _seenFwd[_seenFwdPos].seqHex = seqHex;
  _seenFwd[_seenFwdPos].tsMs   = millis();

  _seenFwdPos++;

  if (_seenFwdPos >= SEEN_FWD_SIZE) {
    _seenFwdPos = 0;
  }
}

// ======================================================
// 10. EVENT HANDLER
// ======================================================

// Backlog origin tracking: handle delivery confirmation and state update
void MeshNode::onOriginHeard(StaticRoute::Id24 srcId,
                             const String& seqHex,
                             uint32_t frameIdx,
                             uint8_t posInFrame) {
  if (_bCount <= 0 || srcId == 0) return;
  BacklogEntry &be = _bq[_bHead];
  if (be.srcId24 != srcId) return;

  // update only if frame context changes (avoid stale overwrite)
  if (be.lastHeardFrameIdx != frameIdx) {
    be.lastHeardFrameIdx = frameIdx;
    be.lastHeardPos = posInFrame;
  }

  // delivery confirmation check
  if (seqHex == be.seqHex) {
    Serial.print("[NODE-BACKLOG] delivered seq=");
    Serial.println(seqHex);

    // safely remove head entry
    if (_bCount > 0) {
      _bHead = (_bHead + 1) % BQSIZE;
      _bCount--;
    }

    _backlogInFlight = false;
    _backlogRetry = 0;
  }
} 