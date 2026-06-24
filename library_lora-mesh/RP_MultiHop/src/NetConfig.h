#pragma once
#include <Arduino.h>

// ----------------------------------------
// 1) Ukuran antrian / backlog
// ----------------------------------------
namespace NetQueues {
  // Node: antrian forward + backlog
  static constexpr int NODE_FWD_QSIZE      = 16;
  static constexpr int NODE_BACKLOG_QSIZE  = 10;

  // Relay: antrian forward + backlog
  static constexpr int RELAY_FWD_QSIZE     = 16;
  static constexpr int RELAY_BACKLOG_QSIZE = 10;
}

// ----------------------------------------
// 2) Neighbor table & HELLO neighbors
// ----------------------------------------
namespace NetNeighbors {
  static constexpr int MAX_NEIGHBORS        = 8;
  static constexpr int MAX_HELLO_NEIGHBORS  = 3;
}

// ----------------------------------------
// 3) Reliability / retry / timing ACK
// ----------------------------------------
namespace NetReliability {
  // Node origin (kirim data sendiri)
  static constexpr uint32_t NODE_ACK_TIMEOUT_MS   = 2500;
  static constexpr uint8_t  NODE_MAX_RETRY        = 1;

  // Node forward (Node sebagai relay utk child)
  static constexpr uint32_t FWD_ACK_TIMEOUT_MS    = 2500;
  static constexpr uint8_t  FWD_MAX_RETRY         = 1;
  static constexpr uint32_t FWD_TX_GAP_MS         = 50;
  static constexpr uint32_t FWD_FORWARD_GUARD_MS  = 150;

  // Relay (forward / backlog)
  static constexpr uint32_t RELAY_ACK_TIMEOUT_MS   = 2500;
  static constexpr uint8_t  RELAY_MAX_RETRY        = 1;
  static constexpr uint32_t RELAY_TX_GAP_MS        = 50;
  static constexpr uint32_t RELAY_FORWARD_GUARD_MS = 150;

  // Backlog behaviour
  // - BACKLOG_TTL_FRAMES: berapa banyak frame DATA sebelum pesan backlog hangus
  // - BACKLOG_TAIL_SEC  : backlog hanya dikirim di detik-detik terakhir slot origin
  static constexpr uint8_t BACKLOG_TTL_FRAMES = 10; // kira-kira 10 frame
  static constexpr uint8_t BACKLOG_TAIL_SEC   = 4;  // 4 detik terakhir slot origin
}

// ----------------------------------------
// 4) Topologi & analitik di Sink
// ----------------------------------------
namespace NetTopology {
  // Sink: summary & loss tracker, HELLO graph
  static constexpr int MAX_NODES_TRACK = 8;   // summary table (R2, R1, N1, N2, N3, ...)
  static constexpr int MAX_TRACKERS    = 8;   // LossTracker di MeshSink
  static constexpr int MAX_DEVICES     = 16;  // DeviceInfo utk HELLO topology
  static constexpr int MAX_LINKS       = 32;  // LinkInfo utk HELLO topology

  // TDMA: slot data terakhir dalam frame (dipakai utk trigger flush summary)
  static constexpr uint8_t LAST_DATA_SLOT_ID = 5;

  // Sensitivitas RX untuk hitung linkMargin
  static constexpr float RX_SENSITIVITY_DBM = -124.0f;  // kira-kira utk 2.4 kbps
}

// ----------------------------------------
// 5) Mode output Sink (RAW vs ANALYZED)
// ----------------------------------------
namespace NetPrint {
  enum class SinkPrintMode : uint8_t {
    RAW_ONLY,
    ANALYZED_ONLY
  };

  // Default behaviour sekarang
  static constexpr SinkPrintMode SINK_PRINT_MODE = SinkPrintMode::RAW_ONLY;
}
