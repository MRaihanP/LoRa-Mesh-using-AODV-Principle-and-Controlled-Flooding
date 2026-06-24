#pragma once

#include <Arduino.h>
#include "Base.h"
#include "Packet.h"
#include "ID.h"
#include "StaticRoute.h"
#include "SlotPlan.h"
#include "NetConfig.h"


class MeshNode : public MeshDeviceBase {
public:
  explicit MeshNode(const BoardPins& pins);

  void begin() override;
  void loop()  override;

  // =========================
  // Payload & Callback API
  // =========================
  using PayloadBuilderFn = String (*)(uint32_t unixNow, uint16_t seq);
  using TxCallbackFn  = void (*)(uint16_t seq, bool isRetry);
  using AckCallbackFn = void (*)(uint16_t seq);
  using ForwardRxCallbackFn = void (*)(uint32_t fromId24, uint16_t seq);

  void setPayloadBuilder(PayloadBuilderFn fn) { _payloadBuilder = fn; }
  void setTxCallback(TxCallbackFn fn)   { _txCb = fn; }
  void setAckCallback(AckCallbackFn fn) { _ackCb = fn; }
  void setForwardRxCallback(ForwardRxCallbackFn fn) { _fwdRxCb = fn; }

  uint8_t neighborCount() const;

  // =========================
  // Timer TX
  // =========================
  static constexpr uint16_t MIN_TX_GAP_MS = 80;
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

  bool   _hasChild;
  uint8_t _slotIndex;

  // ============================================================
  // 2. Routing / Parent Selection State
  // ============================================================
  uint32_t _candidateParentId = 0;
  uint32_t _candidateSinceMs = 0;
  uint8_t  _candidateFail = 0;

  uint8_t  _parentAckFailStreak;
  int16_t  _parentLastRssi;
  float    _parentDegradeScore;

  bool     _parentConfirmed = false;
  uint8_t  _failedParentCount;

  uint32_t _lastRouteEvalFrameIdx;
  uint8_t  _hopToSink;

  bool _routeDirty = false;

  // FAST SELF-HEALING STATE
  bool              _parentSuspected        = false; // ACK timeout pertama terjadi
  bool              _brokenParentPending    = false; // tunggu frameIdx untuk set blacklist
  StaticRoute::Id24 _healingFromParent      = 0;    // parent asal saat heal
  StaticRoute::Id24 _brokenParentId24       = 0;    // parent yang diblacklist
  uint32_t          _brokenParentUntilFrameIdx = 0; // blacklist berlaku sampai frame ini

  // ============================================================
  // 3. Transmission Control
  // ============================================================
  bool     _txBusy = false;
  uint32_t _lastTxStartMs = 0;
  uint32_t _lastTxMs = 0;

  TxCallbackFn  _txCb  = nullptr;
  AckCallbackFn _ackCb = nullptr;
  ForwardRxCallbackFn _fwdRxCb = nullptr;

  bool canTransmitNow() const;
  bool isDataTxPhase() const;
  bool requestTx();
  void releaseTx();


  // ============================================================
  // 4. Origin TX Mode State
  // ============================================================
  uint16_t _seq;
  bool     _waitingAck;
  uint32_t _ackStartMs;
  uint8_t  _retry;

  uint32_t _lastFrameIdx;
  bool     _sentThisFrame;

  String   _lastOriginFrame;
  String   _lastOriginSeqHex;
  bool     _hasLastOriginFrame;

  uint32_t _parentLostSinceMs;
  String   _lastOriginRetryPacket;


  // ============================================================
  // 5. Forward Queue (Upstream TX Buffer)
  // ============================================================
  static constexpr int QSIZE = NetQueues::NODE_FWD_QSIZE;

  struct FwdEntry {
    String frame;
    String seqHex;
  };

  FwdEntry _q[QSIZE];
  int _head;
  int _tail;
  int _count;

  bool     _inFlight;
  uint8_t  _fwdRetry;
  uint32_t _fwdLastTxMs;
  uint32_t _nextForwardAllowedMs;


  // ============================================================
  // 6. Backlog Queue (Retry / Recovery Buffer)
  // ============================================================
  static constexpr int BQSIZE = NetQueues::NODE_BACKLOG_QSIZE;

  struct BacklogEntry {
    String frame;
    String seqHex;

    StaticRoute::Id24 srcId24 = 0;

    uint8_t slotId = 0;

    uint32_t createdFrameIdx = 0;

    uint32_t lastHeardFrameIdx = 0;

    uint8_t lastHeardPos = 0;
  };

  struct SeenForward {
    String srcHex;
    String seqHex;
    uint32_t tsMs;
  };

  BacklogEntry _bq[BQSIZE];

  static constexpr uint8_t SEEN_FWD_SIZE = 32;

  SeenForward _seenFwd[SEEN_FWD_SIZE];
  uint8_t _seenFwdPos = 0;

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
  static constexpr int MAX_NEIGHBORS = NetNeighbors::MAX_NEIGHBORS;
  static constexpr int MAX_HELLO_NEIGHBORS = NetNeighbors::MAX_HELLO_NEIGHBORS;

  struct LinkStats {
    uint16_t txCount = 0;
    uint16_t ackCount = 0;
    uint16_t failCount = 0;

    float rssiAvg = 0;
    float rssiVar = 0;

    uint16_t retrySum = 0;
  };

  struct NeighborInfo {
      bool used = false;

      StaticRoute::Id24 id24 = 0;

      String alias;

      int16_t lastRssi = 0;

      uint8_t hopToSink = 255;

      uint32_t lastSeenFrame = 0;
      uint32_t lastSeenMs = 0;

      // FRAME-BASED FRESHNESS
      bool seenThisHelloFrame = false;

      uint32_t helloFrameIdx = 0;

      NetRuntime::DeviceRole role =
        NetRuntime::DeviceRole::NODE;

      LinkStats stats;
  };

  NeighborInfo _neighbors[MAX_NEIGHBORS];

  float computeLinkScore(const NeighborInfo& n) const;


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
  float computeEffectiveRouteScore(const NeighborInfo& n) const;
  void chooseBestParentFromNeighbors(uint32_t frameIdx);

  StaticRoute::Id24 chooseNextHopForPacket(const String& path);

  // ============================================================
  // 9. HELLO / Discovery State
  // ============================================================
  bool     _helloSentThisFrame;
  uint16_t _helloSeq;
  uint32_t _lastHelloFrameIdx;
  bool     _wasInHelloPhase;   // untuk deteksi transisi DATA -> HELLO
  uint32_t _neighborCollectionFrameIdx = 0;


  // ============================================================
  // 10. Queue Operations
  // ============================================================
  void enqueueUp(const String& frame, const String& seqHex);
  void pushBacklog(const String& frame, const String& seqHex);


  // ============================================================
  // 11. Origin Tracking Helpers
  // ============================================================
  void onOriginHeard(
      StaticRoute::Id24 srcId,
      const String& seqHex,
      uint32_t frameIdx,
      uint8_t posInFrame
  );

  bool alreadyForwarded(const String& srcHex,
                        const String& seqHex);

  void markForwarded(const String& srcHex,
                    const String& seqHex);

  // ============================================================
  // 12. Backlog Decision Logic
  // ============================================================
  bool canSendBacklogNow(
      const BacklogEntry& be,
      uint32_t unixNow,
      uint32_t frameIdx,
      uint8_t posInFrame
  ) const;


  // ============================================================
  // 13. Configuration & Utilities
  // ============================================================
  PayloadBuilderFn _payloadBuilder = nullptr;


  // ============================================================
  // 14. Proto Handlers
  // ============================================================

  void handleRx(String &rxBuf,
                uint32_t nowMs,
                uint32_t &lastAnyTxGuardMs);

  void handleAckFrame(const String &line);

  void handleOriginAck(const String &dst,
                       const String &seqStr);

  void handleForwardAck(const String &fromId,
                        const String &seqStr);

  void handleBacklogAck(const String &seqStr);

  void handleRouteHello(const String &line,
                        int rssi_dBm,
                        uint32_t nowMs);

  void handleDataFrame(const String &line,
                       int rssi_dBm,
                       uint32_t nowMs,
                       uint32_t &lastAnyTxGuardMs);

  void handleTxForward(uint32_t nowMs,
                       uint32_t &lastAnyTxGuardMs);

  void processForwardNewTx(uint32_t nowMs,
                           uint32_t &lastAnyTxGuardMs);

  void processForwardRetry(uint32_t nowMs,
                           uint32_t &lastAnyTxGuardMs);

  void handleOriginAndHello(uint32_t nowMs,
                            uint32_t &lastAnyTxGuardMs);

  void handleHelloFrame(uint32_t unixNow,
                        uint32_t frameIdx,
                        uint8_t posInCycle,
                        uint32_t &lastAnyTxGuardMs);

  void handleOriginFrame(uint32_t unixNow,
                         uint32_t frameIdx,
                         uint8_t posInCycle,
                         bool inDataSlot,
                         uint32_t nowMs,
                         uint32_t &lastAnyTxGuardMs);

  void handleBacklog(uint32_t nowMs,
                     uint32_t &lastAnyTxGuardMs);

  void processBacklogNew(uint32_t unixNow,
                         uint32_t frameIdx,
                         uint8_t posInCycle,
                         uint32_t nowMs,
                         uint32_t &lastAnyTxGuardMs);

  void processBacklogRetry(uint32_t frameIdx,
                           uint32_t &lastAnyTxGuardMs);

  bool sendPacket(const String &packet,
                  uint32_t &lastAnyTxGuardMs);

};