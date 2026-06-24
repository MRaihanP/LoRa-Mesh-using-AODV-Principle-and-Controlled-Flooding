#pragma once
#include <Arduino.h>
#include "Base.h"
#include "Packet.h"
#include "ID.h"
#include "StaticRoute.h"
#include "SlotPlan.h"
#include "NetConfig.h"

class MeshRelay : public MeshDeviceBase {
public:
  explicit MeshRelay(const BoardPins& pins);

  void begin() override;
  void loop()  override;

  // =========================
  // Payload & Callback API
  // =========================
  using PayloadBuilderFn    = String (*)(uint32_t unixNow, uint16_t seq);
  using TxCallbackFn        = void (*)(uint16_t seq, bool isRetry);
  using AckCallbackFn       = void (*)(uint16_t seq);
  using ForwardRxCallbackFn = void (*)(uint32_t fromId24, uint16_t seq);

  void setPayloadBuilder(PayloadBuilderFn fn)       { _payloadBuilder = fn; }
  void setTxCallback(TxCallbackFn fn)               { _txCb = fn; }
  void setAckCallback(AckCallbackFn fn)             { _ackCb = fn; }
  void setForwardRxCallback(ForwardRxCallbackFn fn) { _fwdRxCb = fn; }

  uint8_t neighborCount() const;

  // =========================
  // Timer TX
  // =========================
  static constexpr uint16_t MIN_TX_GAP_MS      = 80;
  static constexpr uint16_t TX_BUSY_TIMEOUT_MS = 1200;

private:

  // ============================================================
  // 1. Identity & Routing Information
  // ============================================================
  StaticRoute::Id24 _id24;
  StaticRoute::Id24 _parentId24;

  String _idHex6;
  String _parentHex6;

  String _alias;
  String _parentAlias;

  int8_t  _slotIndex;
  uint8_t _hopToSink;

  // ============================================================
  // 2. Routing / Parent Selection State
  // ============================================================
  uint8_t  _parentAckFailStreak;
  int16_t  _parentLastRssi;
  float    _parentDegradeScore;

  bool     _parentConfirmed;
  uint8_t  _failedParentCount;

  uint32_t _lastRouteEvalFrameIdx;

  bool     _routeDirty;

  // FAST SELF-HEALING STATE
  bool              _parentSuspected;
  bool              _brokenParentPending;
  StaticRoute::Id24 _healingFromParent;
  StaticRoute::Id24 _brokenParentId24;
  uint32_t          _brokenParentUntilFrameIdx;

  // ============================================================
  // 3. TX Control
  // ============================================================
  bool     _txBusy;
  uint32_t _lastTxStartMs;
  uint32_t _lastTxMs;

  bool canTransmitNow() const;
  bool isDataTxPhase() const;
  bool requestTx();
  void releaseTx();

  // ============================================================
  // 4. Origin (Self) TX State
  // ============================================================
  uint16_t _localSeq;
  bool     _selfSentThisFrame;
  uint32_t _selfLastFrameIdx;
  String   _selfLastSeqHex;

  // ============================================================
  // 5. Forward Queue (Upstream TX Buffer)
  // ============================================================
  static constexpr int QSIZE = NetQueues::RELAY_FWD_QSIZE;

  struct QueueEntry {
    String frame;
    String seqHex;
  };

  QueueEntry _q[QSIZE];
  int _head;
  int _tail;
  int _count;

  bool     _inFlight;
  uint8_t  _retry;
  uint32_t _lastFwdTxMs;
  uint32_t _nextForwardAllowedMs;

  // ============================================================
  // 6. Backlog Queue (Retry / Recovery Buffer)
  // ============================================================
  static constexpr int BQSIZE = NetQueues::RELAY_BACKLOG_QSIZE;

  struct BacklogEntry {
    String            frame;
    String            seqHex;
    StaticRoute::Id24 srcId24;
    uint8_t           slotId;
    uint32_t          createdFrameIdx;
    uint32_t          lastHeardFrameIdx;
    uint8_t           lastHeardPos;
  };

  struct SeenForward {
    String   srcHex;
    String   seqHex;
    uint32_t tsMs;
  };

  BacklogEntry _bq[BQSIZE];

  static constexpr uint8_t SEEN_FWD_SIZE = 32;
  SeenForward _seenFwd[SEEN_FWD_SIZE];
  uint8_t     _seenFwdPos;

  int _bHead;
  int _bTail;
  int _bCount;

  bool     _backlogInFlight;
  uint8_t  _backlogRetry;
  uint32_t _backlogAckStartMs;
  uint32_t _backlogFrameIdx;
  uint32_t _nextBacklogFrameIdxAllowed;
  bool     _backlogSentThisFrame;
  String   _backlogSeqInFlight;

  // ============================================================
  // 7. Neighbor Table (HELLO-based topology)
  // ============================================================
  static constexpr int MAX_NEIGHBORS       = NetNeighbors::MAX_NEIGHBORS;
  static constexpr int MAX_HELLO_NEIGHBORS = NetNeighbors::MAX_HELLO_NEIGHBORS;

  struct LinkStats {
    uint16_t txCount   = 0;
    uint16_t ackCount  = 0;
    uint16_t failCount = 0;
    float    rssiAvg   = 0;
    float    rssiVar   = 0;
    uint16_t retrySum  = 0;
  };

  struct NeighborInfo {
    bool              used          = false;
    StaticRoute::Id24 id24          = 0;
    String            alias;
    int16_t           lastRssi      = 0;
    uint8_t           hopToSink     = 255;
    uint32_t          lastSeenFrame = 0;
    uint32_t          lastSeenMs    = 0;
    bool              seenThisHelloFrame = false;
    uint32_t          helloFrameIdx = 0;
    NetRuntime::DeviceRole role     = NetRuntime::DeviceRole::NODE;
    LinkStats         stats;
  };

  NeighborInfo _neighbors[MAX_NEIGHBORS];

  float computeLinkScore(const NeighborInfo& n) const;
  float computeEffectiveRouteScore(const NeighborInfo& n) const;

  // ============================================================
  // 8. Neighbor / Parent Selection Helpers
  // ============================================================
  StaticRoute::Id24 findAlternateParentExcluding(StaticRoute::Id24 excludeId);

  void clearNeighbors();
  void beginHelloFrameCollection(uint32_t frameIdx);
  void finalizeHelloFrameCollection(uint32_t frameIdx);

  void updateNeighborFromHello(
    StaticRoute::Id24 nid,
    const String& alias,
    NetRuntime::DeviceRole advertisedRole,
    uint8_t hopToSink,
    int16_t rssi,
    uint32_t frameIdx
  );

  void debugPrintNeighbors() const;
  void debugPrintRoutingTable(uint32_t frameIdx) const;
  void chooseBestParentFromNeighbors(uint32_t frameIdx);

  // ============================================================
  // 9. HELLO / Discovery State
  // ============================================================
  bool     _helloSentThisFrame;
  uint16_t _helloSeq;
  uint32_t _lastHelloFrameIdx;
  bool     _wasInHelloPhase;
  uint32_t _neighborCollectionFrameIdx;

  // ============================================================
  // 10. Queue Operations
  // ============================================================
  void enqueueUp(const String& frame, const String& seqHex);
  void pushBacklog(const String& frame, const String& seqHex);

  // ============================================================
  // 11. Origin / Duplicate Tracking
  // ============================================================
  void onOriginHeard(
    StaticRoute::Id24 srcId,
    const String&     seqHex,
    uint32_t          frameIdx,
    uint8_t           posInFrame
  );

  bool alreadyForwarded(const String& srcHex, const String& seqHex);
  void markForwarded(const String& srcHex, const String& seqHex);

  // ============================================================
  // 12. Backlog Decision Logic
  // ============================================================
  bool canSendBacklogNow(
    const BacklogEntry& be,
    uint32_t unixNow,
    uint32_t frameIdx,
    uint8_t  posInFrame
  ) const;

  // ============================================================
  // 13. Callbacks & Configuration
  // ============================================================
  PayloadBuilderFn    _payloadBuilder;
  TxCallbackFn        _txCb;
  AckCallbackFn       _ackCb;
  ForwardRxCallbackFn _fwdRxCb;
};
