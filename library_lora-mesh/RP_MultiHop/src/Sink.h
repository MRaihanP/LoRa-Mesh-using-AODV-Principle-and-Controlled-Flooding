#pragma once
#include <Arduino.h>
#include "Base.h"
#include "ID.h"
#include "StaticRoute.h"
#include "NetConfig.h"

class MeshSink : public MeshDeviceBase {
public:
  explicit MeshSink(const BoardPins& pins);

  void begin() override;
  void loop()  override;

  // ===================== HOOK API =====================
  // Event yang diminta:
  // 1) Sink TX HELLO
  // 2) Sink RX HELLO
  // 3) Sink RX DATA (string data CSV)
  // 4) Frame advance (ganti frame)

  typedef void (*HookFn)(void* user);
  typedef void (*HookLineFn)(void* user, const char* line);

  void setHookTxHello(HookFn fn, void* user)      { _hkTxHello = fn; _hkUser = user; }
  void setHookRxHello(HookLineFn fn, void* user)  { _hkRxHello = fn; _hkUser = user; }
  void setHookRxData(HookLineFn fn, void* user)   { _hkRxData  = fn; _hkUser = user; }
  void setHookFrameAdvance(HookFn fn, void* user) { _hkFrameAdv = fn; _hkUser = user; }

private:
  using Id24 = StaticRoute::Id24;

  static constexpr int MAX_TRACKERS = NetTopology::MAX_TRACKERS;
  static constexpr int MAX_DEVICES  = NetTopology::MAX_DEVICES;
  static constexpr int MAX_LINKS    = NetTopology::MAX_LINKS;
  uint32_t _id24;
  String   _idHex6;

  // ===== hook storage =====
  HookFn     _hkTxHello  = nullptr;
  HookLineFn _hkRxHello  = nullptr;
  HookLineFn _hkRxData   = nullptr;
  HookFn     _hkFrameAdv = nullptr;
  void*      _hkUser     = nullptr;

  // Packet loss per-ID (srcId)
  struct LossTracker {
    Id24     id;
    uint16_t lastSeq;
    uint32_t lost;
    bool     used;
  };


  LossTracker _lossTrackers[MAX_TRACKERS];

  // Helper untuk packet loss per ID
  LossTracker* findOrCreateTracker(Id24 id);

  // Helper alias <-> id
  static const char* aliasForId(Id24 id);
  static Id24        idForAlias(const String& alias);

  // Helper ambil alias child terakhir dari field path "N2-N1-R2-R1-S"
  static bool extractChildAliasFromPath(const String& path, String& outChildAlias);

  // =====================================================
  // HELLO topology graph (global di Sink)
  // =====================================================
  struct DeviceInfo {
    Id24     id;
    String   alias;
    uint8_t  hopSelf;        // hopToSink yang diiklankan node tsb
    uint8_t  battPct;
    uint32_t lastHelloFrame;
    bool     used;
  };

  struct LinkInfo {
    Id24     srcId;
    Id24     dstId;
    int16_t  lastRssi;       // RSSI dari src->dst (yang di-report src)
    uint8_t  hopDst;         // hopToSink dari dst (menurut dst)
    uint32_t lastHelloFrame; // frame HELLO terakhir link ini muncul
    bool     used;
  };

  DeviceInfo _devices[MAX_DEVICES];
  LinkInfo   _links[MAX_LINKS];

  uint32_t _lastHelloSummaryFrameIdx = 0; // untuk antispam summary

  // State HELLO / BEACON Sink sendiri
  uint32_t _lastHelloFrameIdx   = 0;
  bool     _helloSentThisFrame  = false;

  void clearHelloGraph();
  void updateDeviceFromHello(Id24 id,
                             const String& alias,
                             uint8_t hopSelf,
                             uint8_t battPct,
                             uint32_t frameIdx);
  void updateLinkFromHello(Id24 srcId,
                           Id24 dstId,
                           uint8_t hopDst,
                           int16_t rssi,
                           uint32_t frameIdx);
  void printHelloSummary(uint32_t frameIdx) const;
};
