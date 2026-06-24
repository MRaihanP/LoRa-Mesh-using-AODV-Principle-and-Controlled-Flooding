#pragma once
#include <Arduino.h>
#include "StaticRoute.h"
#include "SlotPlan.h"

namespace MeshProto {
  // Jenis paket DATA / ROUTE
  static constexpr const char* DATA_NODE_ORIGIN = "01";  // DATA dari Node (origin)
  static constexpr const char* DATA_NODE_VIA    = "02";  // DATA Node via relay
  static constexpr const char* DATA_RELAY_SELF  = "03";  // DATA originating di Relay
  static constexpr const char* ROUTE_HELLO      = "10";  // paket HELLO (routing gossip)
}

namespace DataWire {

// -----------------------------------------------------------------------------
// Layout field FINAL (tanpa legacy sensor kolom per-kolom).
//
// Semua telemetry / sensor / info aplikasi sekarang masuk ke IDX_PAYLOAD
// sebagai satu string bebas.
// -----------------------------------------------------------------------------
enum FieldIndex : uint8_t {
  // --- HEADER JARINGAN (CORE) ---
  IDX_PROTO = 0,        //  1 proto ("01","02","03")
  IDX_ROUTE_VER,        //  2 routeVer
  IDX_SRC_ID,           //  3 srcId (HEX6)
  IDX_PARENT_ALIAS,     //  4 parentAlias (next-hop alias)
  IDX_PATH,             //  5 path (jejak alias, "N1-R2-R1")
  IDX_DEST_ALIAS,       //  6 destAlias ("S")
  IDX_SEQ,              //  7 seq (HEX4)
  IDX_TS_ORIGIN,        //  8 tsOrigin (Unix time origin)
  IDX_SLOT_ID,          //  9 slotId (1..N)

  // --- FOOTER JARINGAN & ANALITIK ---
  IDX_HOP_COUNT,        // 10 hopCount (diupdate tiap hop)
  IDX_NET_FLAGS,        // 11 netFlags (bitmask, "00" default)
  IDX_RETRY_COUNT,      // 12 retryCount
  IDX_BACKLOG_COUNT,    // 13 backLogCount
  IDX_FRAME_IDX,        // 14 frameIdx (RTC-based)
  IDX_POS_IN_FRAME,     // 15 posInFrame (detik dalam frame)
  IDX_E2E_LATENCY,      // 16 e2eLatency (diisi Sink)
  IDX_AVG_LAT_HOP,      // 17 avgLatHop (Sink)
  IDX_HOP_RSSI_SUM,     // 18 hopRssiSum (akumulasi per hop)
  IDX_AVG_RSSI,         // 19 avgRssi (Sink)
  IDX_HOP_SNR_SUM,      // 20 hopSnrSum (akumulasi per hop)
  IDX_AVG_SNR,          // 21 avgSnr (Sink)
  IDX_THROUGHPUT,       // 22 throughput (Sink)
  IDX_LINK_MARGIN,      // 23 linkMargin (Sink)
  IDX_PACKET_LOSS,      // 24 packetLoss (Sink)
  IDX_PACKET_SIZE,      // 25 packetSize (diisi Sink, bytes)
  IDX_PAYLOAD,          // 26 payload aplikasi (string bebas)
  NUM_FIELDS            // total field = 27
};

// Konfigurasi analitik (dipakai Node/Relay/Sink)
static constexpr float NOISE_FLOOR_DBM  = -92.0f;   // kira-kira noise floor
static constexpr float SENSITIVITY_DBM  = -129.0f;  // sensitivitas link LoRa
static constexpr uint8_t ROUTE_VER      = 1;
static constexpr const char* DEST_ALIAS = "S";

// Helper untuk build CSV dari array String
inline String joinCsv(String f[NUM_FIELDS], int count = NUM_FIELDS) {
  String out;
  for (int i = 0; i < count; ++i) {
    if (i) out += ',';
    out += f[i];
  }
  return out;
}

// ---------------------------------------------------------------------------
// Builder GENERIC untuk frame DATA (dipakai Node & Relay).
//   - proto       : "01"/"02"/"03"
//   - srcIdHex    : ID 6 hex
//   - srcId24     : ID uint32_t
//   - parentId24  : ID parent di pohon (next-hop awal)
//   - seq         : sequence (uint16, akan di-HEX4 di wire)
//   - tsOrigin    : unix time origin
//   - frameIdx    : index frame (RTC)
//   - posInFrame  : posisi detik dalam frame
//   - payload     : string bebas aplikasi
// ---------------------------------------------------------------------------
inline String buildDataFrame(const char* proto,
                             const String& srcIdHex,
                             StaticRoute::Id24 srcId24,
                             StaticRoute::Id24 parentId24,
                             uint16_t seq,
                             uint32_t tsOrigin,
                             uint32_t frameIdx,
                             uint8_t  posInFrame,
                             const String& payload)
{
  String f[NUM_FIELDS];

  const char* myAlias     = StaticRoute::aliasForId(srcId24);
  const char* parentAlias = StaticRoute::aliasForId(parentId24);
  uint8_t     slotId      = SlotPlan::slotIdFor(srcId24);

  char buf[16];

  // 1 proto
  f[IDX_PROTO] = proto;

  // 2 routeVer
  f[IDX_ROUTE_VER] = String((int)ROUTE_VER);

  // 3 srcId
  f[IDX_SRC_ID] = srcIdHex;

  // 4 parentAlias
  f[IDX_PARENT_ALIAS] = parentAlias ? String(parentAlias) : String("?");

  // 5 path -> mulai dari alias origin
  f[IDX_PATH] = myAlias ? String(myAlias) : String("?");

  // 6 destAlias
  f[IDX_DEST_ALIAS] = DEST_ALIAS;

  // 7 seq (HEX 4 digit)
  snprintf(buf, sizeof(buf), "%04X", (unsigned int)seq);
  f[IDX_SEQ] = String(buf);

  // 8 tsOrigin
  f[IDX_TS_ORIGIN] = String((unsigned long)tsOrigin);

  // 9 slotId
  f[IDX_SLOT_ID] = String((int)slotId);

  // --- footer network & analitik diinisialisasi default ---

  // 10 hopCount (mulai 0, diupdate per hop)
  f[IDX_HOP_COUNT] = "0";

  // 11 netFlags
  f[IDX_NET_FLAGS] = "00";

  // 12 retryCount
  f[IDX_RETRY_COUNT] = "0";

  // 13 backLogCount
  f[IDX_BACKLOG_COUNT] = "0";

  // 14 frameIdx
  f[IDX_FRAME_IDX] = String((unsigned long)frameIdx);

  // 15 posInFrame
  f[IDX_POS_IN_FRAME] = String((int)posInFrame);

  // 16 e2eLatency (diisi Sink)
  f[IDX_E2E_LATENCY] = "0";

  // 17 avgLatHop (Sink)
  f[IDX_AVG_LAT_HOP] = "0";

  // 18 hopRssiSum (akumulasi per hop)
  f[IDX_HOP_RSSI_SUM] = "0";

  // 19 avgRssi (Sink)
  f[IDX_AVG_RSSI] = "0";

  // 20 hopSnrSum (akumulasi per hop)
  f[IDX_HOP_SNR_SUM] = "0";

  // 21 avgSnr (Sink)
  f[IDX_AVG_SNR] = "0";

  // 22 throughput (Sink)
  f[IDX_THROUGHPUT] = "0";

  // 23 linkMargin (Sink)
  f[IDX_LINK_MARGIN] = "0";

  // 24 packetLoss (Sink)
  f[IDX_PACKET_LOSS] = "0";

  // 25 packetSize → sementara 0, diisi actual length di Sink
  f[IDX_PACKET_SIZE] = "0";

  // 26 payload aplikasi (bebas)
  f[IDX_PAYLOAD] = payload;

  return joinCsv(f);
}

// ---------------------------------------------------------------------------
// Convenience: Node origin
// ---------------------------------------------------------------------------
inline String buildNodeData(const String& srcIdHex,
                            StaticRoute::Id24 srcId24,
                            StaticRoute::Id24 parentId24,
                            uint16_t seq,
                            uint32_t tsOrigin,
                            uint32_t frameIdx,
                            uint8_t  posInFrame,
                            const String& payload)
{
  return buildDataFrame(MeshProto::DATA_NODE_ORIGIN,
                        srcIdHex, srcId24, parentId24,
                        seq, tsOrigin, frameIdx, posInFrame,
                        payload);
}

// ---------------------------------------------------------------------------
// Convenience: Relay self-origin
// ---------------------------------------------------------------------------
inline String buildRelayData(const String& srcIdHex,
                             StaticRoute::Id24 srcId24,
                             StaticRoute::Id24 parentId24,
                             uint16_t seq,
                             uint32_t tsOrigin,
                             uint32_t frameIdx,
                             uint8_t  posInFrame,
                             const String& payload)
{
  return buildDataFrame(MeshProto::DATA_RELAY_SELF,
                        srcIdHex, srcId24, parentId24,
                        seq, tsOrigin, frameIdx, posInFrame,
                        payload);
}

} // namespace DataWire
