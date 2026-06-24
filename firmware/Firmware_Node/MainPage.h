#pragma once

#include <RP_TFTDisplay.h>
#include "OS.h"
#include "pins_and_config.h"
#include <math.h>
#include <string.h>

using IMU  = PinsAndConfig::IMU;
using SHT  = PinsAndConfig::SHT30;
using BMP  = PinsAndConfig::BMP280;
using GNSS = PinsAndConfig::GNSS;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline void fillRoundedRectLocal(GfxRGB888& g, int W, int H, int r,
                                        uint8_t R,uint8_t G,uint8_t B){
  if (r<0) r=0; if (r>H/2) r=H/2; if (r>W/2) r=W/2;
  for (int yy=0; yy<H; ++yy){
    int inset=0;
    if (r>0){
      if (yy < r){
        float dy = (float)(r - 1 - yy);
        float dx = sqrtf((float)r*r - dy*dy);
        inset = (int)(r - floorf(dx));
      }
      int y2 = (H - 1 - yy);
      if (y2 < r){
        float dy = (float)(r - 1 - y2);
        float dx = sqrtf((float)r*r - dy*dy);
        int inset2 = (int)(r - floorf(dx));
        if (inset2 > inset) inset = inset2;
      }
    }
    uint8_t* p = g.pix + ((size_t)yy*W)*3;
    for (int xx=0; xx<W; ++xx){
      const bool inBody = (xx>=inset && xx<(W-inset));
      if (inBody){ p[0]=R; p[1]=G; p[2]=B; } else { p[0]=0; p[1]=0; p[2]=0; }
      p+=3;
    }
  }
}

static inline void netIconNoBg(GfxRGB888& g, int Rrx,int Rry, int cx,int cy, int outerR,
                               uint8_t r,uint8_t gg,uint8_t b){
  if (outerR <= 6) return;
  const float fDot=0.14f, fGap=0.10f, fThick=0.12f, thetaDeg=45.f;
  float rDot0 = fDot   * outerR;
  float gap0  = fGap   * outerR;
  float th0   = fThick * outerR;
  float total0 = rDot0 + (gap0 + th0) * 3.0f; if (total0 < 1.f) total0 = 1.f;
  float S = (float)outerR / total0;
  float rDot = max(2.0f, rDot0 * S);
  float gap  = max(1.0f, gap0  * S);
  float th   = max(2.0f, th0   * S);
  float ri1 = rDot + gap, ro1 = ri1 + th;
  float ri2 = ro1 + gap,  ro2 = ri2 + th;
  float ri3 = ro2 + gap,  ro3 = ri3 + th;
  icFillWedgeRingLR(g, Rrx,Rry, cx,cy, ri3,ro3, thetaDeg, r,gg,b);
  icFillWedgeRingLR(g, Rrx,Rry, cx,cy, ri2,ro2, thetaDeg, r,gg,b);
  icFillWedgeRingLR(g, Rrx,Rry, cx,cy, ri1,ro1, thetaDeg, r,gg,b);
  icFillCircle(g, Rrx,Rry, cx, cy, (int)roundf(rDot), r,gg,b);
}

// -----------------------------------------------------------------------------
// MainPage
// -----------------------------------------------------------------------------
class MainPage {
public:
  void begin(AppOS* os, TopBar* topBar){
    _os  = os;
    _top = topBar;

    // Path widget
    path = new UiPathWidget(_os->ui, _os->bg, _os->blit);
    path->props.x      = 0;
    path->props.y      = 0;
    path->props.w      = TFT_WIDTH;
    path->props.h      = TFT_HEIGHT;
    path->props.tileW  = TFT_WIDTH;
    path->props.tileH  = Theme::STRIP_H;
    path->props.stroke = 6;
    path->props.pathR  = 255;
    path->props.pathG  = 30;
    path->props.pathB  = 30;
    path->props.cursorR     = 255;
    path->props.cursorG     = 30;
    path->props.cursorB     = 30;
    path->props.cursorSide  = 40;
    path->props.cursorNotch = 8;
    path->props.scaleMin    = 4.0f;
    path->props.scaleMax    = 64.0f;
    path->props.zoomAlpha   = 0.65f;
    path->props.follow      = true;
    path->props.followTau   = 0.22f;

    // Dummy awal: cursor di (0,0), menghadap atas (headingRad=0)
    path->clear();
    path->setCenterWorld(0.0f, 0.0f);
    path->pushPoint(0.0f, 0.0f, true, 0.0f);

    // Kompas
    UiCompassOverlay::Props cp;
    cp.yTop         = Theme::BAR_H;
    cp.size         = 0.7f;
    cp.headingDeg   = 0.0f;
    cp.badgeOffsetY = 0;
    cp.tauMs        = 120.0f;
    cp.instanceId   = 0;
    compass.setProps(cp);

    compass.getPaintBounds(_compY, _compH);
    if (_compY < 0) { _compH += _compY; _compY = 0; }
    if (_compY + _compH > TFT_HEIGHT) _compH = TFT_HEIGHT - _compY;
    if (_compH < 1) _compH = 1;

    computeBoxesFromCompass();
    if (_lH > 0) drawRoundedBoxOnce(_lX, _lY, _lW, _lH, Theme::BOX_RADIUS);
    if (_rH > 0) drawRoundedBoxOnce(_rX, _rY, _rW, _rH, Theme::BOX_RADIUS);
    layoutRightBox();

    // Tombol bawah (tanpa bottom bar)
    layoutBottomButtons();

    // Occluders & first paint
    applyOccludersForUI();
    drawBottomButtonsIfDirty(true);

    _rightBoxDirty = true;
    _leftBoxDirty  = true;
  }

  void onQSClosed(){
    computeBoxesFromCompass();
    if (_lH > 0) drawRoundedBoxOnce(_lX, _lY, _lW, _lH, Theme::BOX_RADIUS);
    if (_rH > 0) drawRoundedBoxOnce(_rX, _rY, _rW, _rH, Theme::BOX_RADIUS);
    layoutRightBox();

    layoutBottomButtons();
    applyOccludersForUI();

    _rightBoxDirty = true;
    _leftBoxDirty  = true;
    drawBottomButtonsIfDirty(true);
  }

  void tick(uint32_t nowMs, float dt){
    int tx=_lastTouchX, ty=_lastTouchY;
    const bool touching = _os->readTouch1(tx,ty);
    _lastTouchX = tx;
    _lastTouchY = ty;

    // START (toggle) & Track Back (momentary, DISABLED kalau belum ada path)
    if (!_touchWasDown && touching){
      bool insideStart = pointInRect(_btnStart, tx,ty);
      bool insideTB    = pointInRect(_btnTB,    tx,ty);

      // Gate Start: hanya boleh di-hold kalau GNSS pernah lock
      _startHeld = insideStart && _gnssEverLocked;

      // Track Back hanya aktif kalau path sudah ada
      _tbHeld = insideTB && hasAnyPath();

      if (_startHeld){
        _startWasOnAtPress = _started;
        _stopHoldFired     = false;
        _startHoldStartMs  = nowMs;
      }

      if (_tbHeld){
        _tbPressedVisual = true;
        _tbDirty         = true;
      }
    }
    else if (_touchWasDown && touching){
      // Gerakan jari sambil masih menyentuh
      if (_tbHeld){
        bool inside = pointInRect(_btnTB, tx,ty);
        if (inside != _tbPressedVisual){
          _tbPressedVisual = inside;
          _tbDirty         = true;
        }
      }

      // Jika jari keluar dari area Start, batal long-press
      if (_startHeld && !pointInRect(_btnStart, tx,ty)){
        _startHeld = false;
      }
    }
    else if (_touchWasDown && !touching){
      // Lepas sentuhan
      if (_startHeld && pointInRect(_btnStart, _lastTouchX, _lastTouchY)){
        if (!_startWasOnAtPress && _gnssEverLocked){
          _started    = true;
          _startDirty = true;
          beginNewSession();
        }
      }

      // Track Back: sekarang hanya feedback visual, TANPA aksi apa pun
      if (_tbHeld){
        _tbPressedVisual = false;
        _tbDirty         = true;
      }

      _startHeld = false;
      _tbHeld    = false;
    }
    _touchWasDown = touching;

    // Long-press Stop: hanya berlaku jika saat touch-down state-nya ON
    if (_startHeld && _startWasOnAtPress && _started){
      uint32_t elapsed = nowMs - _startHoldStartMs;
      if (elapsed >= 3000 && !_stopHoldFired){
        _stopHoldFired = true;
        _started       = false;
        _startDirty    = true;
        // Sesi tetap tersimpan; nanti start lagi akan buat sesi baru.
      }
    }

    // ==== Gate tombol Start berdasarkan GNSS lock (once) ====
    if (!_gnssEverLocked){
      if (GNSS::gnssFixOK && GNSS::hasFix &&
          isfinite(GNSS::latDeg) && isfinite(GNSS::lonDeg)){
        _gnssEverLocked = true;
        _startDirty     = true;          // ubah warna tombol Start
        ensureNavOrigin(GNSS::latDeg, GNSS::lonDeg);
      }
    }

    // ==== Telemetry L-Box: tarik langsung dari SHT30 & BMP280 ====
    updateTelemetryDirtyFlag();

    // ==== Sinkronisasi jumlah satelit dari backend GNSS ====
    int satNow = (int)GNSS::numSV;   // 0..255
    if (satNow == 0) satNow = -1;    // 0 satelit → "null"
    if (satNow != _satVal){
      _satVal = satNow;
      _rightBoxDirty = true;
    }

    // ==== Neighbor count dari backend MultiHop ====
    int netNow = (int)PinsAndConfig::NetMultiHop::neighborCount;
    if (netNow != _netVal){
      _netVal = netNow;
      _rightBoxDirty = true;
    }

    // ==== Kompas: pakai heading asli dari IMU (north-up) ====
    if (IMU::imuOk){
      float headingDegUI = IMU::headingDeg;
      if (!isnan(headingDegUI)){
        setCompassHeadingDeg(headingDegUI);
      }
    }

    // ==== Sampling path dari GNSS/PDR fused saat tracking ON ====
    sampleGnssToPath(nowMs);

    // ==== Rotasi world: heading-up (ikut perangkat) ====
    {
      float headingRad = deviceHeadingRad();       // 0 = north
      float phi        = (float)HALF_PI - headingRad;
      path->setRotationRad(phi);
    }

    // ==== Path update (kamera, zoom, dsb.) ====
    path->run(dt);

    // ==== Render strip ====
    drawCompassBandOnce();
    paintLeftBoxContentIfDirty();
    if (_rightBoxDirty) paintRightBoxStatus();
    drawBottomButtonsIfDirty(false);
  }

  // === Public setters / state ===
  void pushWorldPoint(float wx, float wy, bool /*valid*/, float /*hd_rad*/){
    // Simpan world coord mentah (nantinya bisa dipakai analis)
    addWorldSample(wx, wy);

    // Cursor selalu menghadap atas layar, karena cursorArrow(0) sekarang sudah up
    const float hd_up = 0.0f;
    if (path) path->pushPoint(wx, wy, true, hd_up);
  }

  void setCompassHeadingDeg(float deg){
    UiCompassOverlay::Props cp;
    cp.yTop=Theme::BAR_H; cp.size=0.7f; cp.headingDeg=deg;
    cp.badgeOffsetY=0; cp.tauMs=120.0f; cp.instanceId=0;
    compass.setProps(cp);
  }
  void setNetworkValue(int v){ if (_netVal != v){ _netVal = v; _rightBoxDirty = true; } }
  void setSatelliteValue(int v){ if (_satVal != v){ _satVal = v; _rightBoxDirty = true; } }

  // Start marker di path (untuk demo dan nanti data asli)
  void setPathStartMarker(float wx, float wy){
    if (path) path->setStartMarker(wx, wy, 255, 255, 0, 8); // kuning, radius 8 px
  }
  void clearPathStartMarker(){
    if (path) path->clearStartMarker();
  }

  void setTemp(float c){ _hasTemp=true; _tempC=c; updateTelemetryDirtyFlag(); }
  void setHumid(float p){ _hasHum=true; _humidPct=p; updateTelemetryDirtyFlag(); }
  void setAlt(float m){ _hasAlt=true; _altM=m; updateTelemetryDirtyFlag(); }
  void setMSL(float m){ _hasMSL=true; _mslM=m; updateTelemetryDirtyFlag(); }
  bool isStarted() const { return _started; }

  // Dipanggil oleh MainPageDemo ketika ingin memulai sesi baru (kompat lama)
  void clearPathAndWorldBuffer(){
    if (path) {
      path->clear();
      path->clearStartMarker();
      path->setCenterWorld(0.0f, 0.0f);
    }
    _worldHead   = 0;
    _worldCount  = 0;

    // Reset state sesi & sampler
    _sessHasOrigin = false;
    _haveLastRel   = false;
    _lastSampleMs  = millis();
  }

  // Track Back request untuk demo lama: masih ada untuk kompat
  bool consumeTrackBackRequest(){
    return false;
  }

  // === Manual positioning per tombol ===
  void setStartButtonManual(bool enable){
    _manualStart = enable;
    layoutBottomButtons();
    applyOccludersForUI();
    drawBottomButtonsIfDirty(true);
  }
  void setStartButtonPos(int x,int y){
    _manualStart = true; _manualStartX=x; _manualStartY=y;
    layoutBottomButtons(); applyOccludersForUI(); drawBottomButtonsIfDirty(true);
  }
  void setTrackBackButtonManual(bool enable){
    _manualTB = enable;
    layoutBottomButtons();
    applyOccludersForUI();
    drawBottomButtonsIfDirty(true);
  }
  void setTrackBackButtonPos(int x,int y){
    _manualTB = true; _manualTBX=x; _manualTBY=y;
    layoutBottomButtons(); applyOccludersForUI(); drawBottomButtonsIfDirty(true);
  }

private:
  // ===== Konstanta & state navigasi global/sesi =====
  static constexpr float    DEG2RAD        = 0.01745329251994f; // pi/180
  static constexpr float    R_EARTH_M      = 6371000.0f;

  // Threshold path GNSS fused
  static constexpr float    MIN_STEP_M     = 5.0f;    // langkah minimal ~5 m
  static constexpr uint32_t MIN_SAMPLE_DT  = 500;     // ms antar sample
  static constexpr float    MIN_SPEED_MPS  = 1.0f;    // minimal speed 1 m/s

  // Start hanya aktif kalau GNSS pernah lock 3D dan lat/lon valid
  bool   _gnssEverLocked    = false;

  // Origin dunia (lat/lon → koordinat meter lokal, equirectangular)
  bool   _navOriginSet      = false;
  float  _navOriginLatRad   = 0.0f;
  float  _navOriginLonRad   = 0.0f;

  // Origin per sesi (awal tracking)
  bool   _sessHasOrigin     = false;
  float  _sessFirstWx       = 0.0f;
  float  _sessFirstWy       = 0.0f;

  // Sampling path (relatif terhadap origin sesi)
  bool     _haveLastRel     = false;
  float    _lastRelWx       = 0.0f;
  float    _lastRelWy       = 0.0f;
  uint32_t _lastSampleMs    = 0;

  AppOS*  _os  = nullptr;
  TopBar* _top = nullptr;

  UiCompassOverlay compass;
  UiPathWidget*    path = nullptr;

  int _compY=0, _compH=0;

  int _lX=0,_lY=0,_lW=0,_lH=0;
  int _rX=0,_rY=0,_rW=0,_rH=0;

  int  _netVal = -1;
  int  _satVal = -1;
  bool _rightBoxDirty = true;
  int  _rPadX=14, _rPadY=6;
  int  _netCX=0, _netCY=0, _netR=12;
  int  _satX=0,  _satY=0; float _satScale=0.16f, _satRotDeg=-45.f;

  bool  _leftBoxDirty = true;
  bool  _hasTemp=false, _hasHum=false, _hasAlt=false, _hasMSL=false;
  float _tempC=0.f, _humidPct=0.f, _altM=0.f, _mslM=0.f;
  char  _lastLine[4][32] = {{0}};

  // World buffer (bookkeeping sederhana)
  struct WorldSample {
    float x;
    float y;
  };
  static constexpr int WORLD_BUF_CAP = 256;
  WorldSample _worldBuf[WORLD_BUF_CAP];
  int         _worldHead  = 0;
  int         _worldCount = 0;

  // Heading dunia dari demo lama (tidak dipakai lagi)
  float _demoHeadingRad = 0.0f;

  // Track Back request (1-shot)
  bool _trackBackRequested = false;

  struct Rect { int x=0,y=0,w=0,h=0; };
  Rect _btnStart{}, _btnTB{};

  static constexpr int START_W=140, START_H=28, START_R=10;
  static constexpr int TB_W   =140, TB_H   =28, TB_R   =10;
  static constexpr int GAP_BTN=10;
  static constexpr int SHIFT_RIGHT = 20; // cluster geser 20 px ke kanan

  // Warna
  static constexpr uint8_t START_ON_R =   0;
  static constexpr uint8_t START_ON_G = 180;
  static constexpr uint8_t START_ON_B =   0;   // hijau
  static constexpr uint8_t START_OFF_R= 0;
  static constexpr uint8_t START_OFF_G= 0;
  static constexpr uint8_t START_OFF_B= 200;   // merah
  static constexpr uint8_t BTN_TX_R   =255;
  static constexpr uint8_t BTN_TX_G   =255;
  static constexpr uint8_t BTN_TX_B   =255;
  static constexpr uint8_t TB_BASE_R  =60;
  static constexpr uint8_t TB_BASE_G  =60;
  static constexpr uint8_t TB_BASE_B  =60;
  static constexpr uint8_t TB_HI_R    =160;
  static constexpr uint8_t TB_HI_G    =160;
  static constexpr uint8_t TB_HI_B    =160;

  bool _started=false;
  bool _startHeld=false;
  bool _tbHeld=false;
  bool _tbPressedVisual=false;

  bool _startDirty=true;
  bool _tbDirty=true;

  bool     _startWasOnAtPress = false;   // state Start/Stop saat touch-down
  uint32_t _startHoldStartMs  = 0;       // waktu mulai hold (ms)
  bool     _stopHoldFired     = false;   // agar Stop hanya sekali per hold

  // Manual positions
  bool _manualStart=true, _manualTB=true;
  int  _manualStartX=10,_manualStartY=440;
  int  _manualTBX=170,_manualTBY=440;

  bool _touchWasDown=false;
  int  _lastTouchX=0, _lastTouchY=0;

  bool hasAnyPath() const {
    // Path dianggap "ada" kalau sudah punya minimal 2 titik world sample.
    // (1 titik saja = baru start / origin, belum ada jejak)
    return (_worldCount >= 2);
  }

  static inline bool pointInRect(const Rect& r, int x, int y){
    return (x >= r.x && x < r.x + r.w &&
            y >= r.y && y < r.y + r.h);
  }
  static inline int clampi(int v,int lo,int hi){
    return v<lo?lo:(v>hi?hi:v);
  }

  void addWorldSample(float wx, float wy){
    _worldBuf[_worldHead].x = wx;
    _worldBuf[_worldHead].y = wy;
    _worldHead = (_worldHead + 1) % WORLD_BUF_CAP;
    if (_worldCount < WORLD_BUF_CAP) _worldCount++;
  }

  // === Helper: heading device (rad) dari IMU, fallback 0 ===
  float deviceHeadingRad() const {
    if (IMU::imuOk && !isnan(IMU::headingDeg)){
      return IMU::headingDeg * DEG2RAD;
    }
    return 0.0f;
  }

  // === Helper: set origin dunia sekali (lat0, lon0 dalam derajat) ===
  void ensureNavOrigin(float latDeg, float lonDeg){
    if (_navOriginSet) return;
    _navOriginLatRad = latDeg * DEG2RAD;
    _navOriginLonRad = lonDeg * DEG2RAD;
    _navOriginSet    = true;
  }

  // === Helper: lat/lon fused → world absolute (meter) ===
  void latLonToWorld(float latDeg, float lonDeg, float& wx, float& wy){
    if (!_navOriginSet){
      ensureNavOrigin(latDeg, lonDeg);
    }

    float latRad = latDeg * DEG2RAD;
    float lonRad = lonDeg * DEG2RAD;

    float dLat = latRad - _navOriginLatRad;
    float dLon = lonRad - _navOriginLonRad;

    float cosLat0 = cosf(_navOriginLatRad);

    wx = dLon * cosLat0 * R_EARTH_M;
    wy = dLat * R_EARTH_M;
  }

  // === Mulai sesi baru saat Start ON pertama kali ===
  void beginNewSession(){
    using G = PinsAndConfig::GNSS;

    // Reset sampler
    _haveLastRel   = false;
    _lastSampleMs  = millis();

    // Butuh posisi fused yang valid, tapi kalau belum ada, kita tetap clear path
    if (!G::hasFix ||
        !isfinite(G::latDeg) || !isfinite(G::lonDeg)){
      _sessHasOrigin = false;
      clearPathAndWorldBuffer();
      return;
    }

    // Pastikan origin dunia ter-set
    ensureNavOrigin(G::latDeg, G::lonDeg);

    float wxAbs, wyAbs;
    latLonToWorld(G::latDeg, G::lonDeg, wxAbs, wyAbs);

    _sessFirstWx   = wxAbs;
    _sessFirstWy   = wyAbs;
    _sessHasOrigin = true;

    // Reset path & buffer
    clearPathAndWorldBuffer();

    // Titik awal sesi di (0,0) + start marker
    setPathStartMarker(0.0f, 0.0f);
    float hd = deviceHeadingRad();
    pushWorldPoint(0.0f, 0.0f, true, hd);

    // Center kamera di awal
    if (path){
      path->setCenterWorld(0.0f, 0.0f);
    }

    _haveLastRel   = false;
    _lastSampleMs  = millis();
  }

  // === Sampling titik path dari GNSS fused → world (meter) ===
  void sampleGnssToPath(uint32_t nowMs){
    using G = PinsAndConfig::GNSS;

    if (!_started) return;

    // Perlu fix fused yang valid
    if (!G::hasFix ||
        !isfinite(G::latDeg) || !isfinite(G::lonDeg)) return;

    // Gate speed minimal (drift low-speed)
    if (G::speed2D_mps < MIN_SPEED_MPS) return;

    if (!_sessHasOrigin){
      // Coba pakai posisi sekarang sebagai origin sesi
      beginNewSession();
      if (!_sessHasOrigin) return;
    }

    // Interval waktu minimal
    if (nowMs - _lastSampleMs < MIN_SAMPLE_DT) return;

    // Konversi lat/lon fused → world absolute (meter)
    float wxAbs, wyAbs;
    latLonToWorld(G::latDeg, G::lonDeg, wxAbs, wyAbs);

    // Relatif terhadap origin sesi (wx0, wy0)
    float relX = wxAbs - _sessFirstWx;
    float relY = wyAbs - _sessFirstWy;

    if (!_haveLastRel){
      _lastRelWx   = relX;
      _lastRelWy   = relY;
      _haveLastRel = true;
      _lastSampleMs = nowMs;

      // Push titik awal kalau cukup jauh dari (0,0)
      if (path && (fabsf(relX) > 1e-3f || fabsf(relY) > 1e-3f)){
        pushWorldPoint(relX, relY, true, deviceHeadingRad());
      }
      return;
    }

    float dx   = relX - _lastRelWx;
    float dy   = relY - _lastRelWy;
    float dist = sqrtf(dx*dx + dy*dy);
    if (dist < MIN_STEP_M){
      _lastSampleMs = nowMs;
      return;
    }

    // Lolos threshold → tambahkan titik baru ke path
    pushWorldPoint(relX, relY, true, deviceHeadingRad());

    _lastRelWx   = relX;
    _lastRelWy   = relY;
    _lastSampleMs = nowMs;
  }

  // Layout & paint
  void drawRoundedBoxOnce(int x,int y,int w,int h,int r){
    _os->blit.blit(_os->ui.tft(), _os->bg, x,y,w,h, [&](GfxRGB888& gfx,int, int,int Rw,int Rh){
      fillRoundedRectLocal(gfx, Rw, Rh, r, Theme::UI_R,Theme::UI_G,Theme::UI_B);
    });
  }

  void computeBoxesFromCompass(){
    const int boxY = _compY + _compH + Theme::EDGE_BUF;
    const int viewBottom = TFT_HEIGHT; // tanpa bottom bar
    const int availH = max(0, viewBottom - boxY);
    _lW = Theme::LBOX_W; _lH = min(Theme::LBOX_H, availH);
    _rW = Theme::RBOX_W; _rH = min(Theme::RBOX_H, availH);
    _lX = Theme::EDGE_BUF;                   _lY = boxY;
    _rX = TFT_WIDTH - Theme::EDGE_BUF - _rW; _rY = boxY;
  }

  void layoutRightBox(){
    _netCY = _rY + _rH/2;
    _netCX = _rX + _rPadX + _netR;
    const int maxNetCX = _rX + _rW - _rPadX - 12 - (_netR + 12);
    if (_netCX > maxNetCX) _netCX = maxNetCX;
    _satY = _netCY;
    _satX = _rX + _rW - _rPadX - 22;
    _rightBoxDirty = true;
  }

  void layoutBottomButtons(){
    const int gapBottom = 12;
    const int baseY = TFT_HEIGHT - gapBottom - START_H;

    if (_manualStart){
      int x = clampi(_manualStartX, 0, TFT_WIDTH-START_W);
      int y = clampi(_manualStartY, 0, TFT_HEIGHT-START_H);
      _btnStart = { x,y, START_W, START_H };
    } else {
      int x = Theme::EDGE_BUF + SHIFT_RIGHT;
      _btnStart = { x, baseY, START_W, START_H };
    }

    if (_manualTB){
      int x = clampi(_manualTBX, 0, TFT_WIDTH-TB_W);
      int y = clampi(_manualTBY, 0, TFT_HEIGHT-TB_H);
      _btnTB = { x,y, TB_W, TB_H };
    } else {
      int x = _btnStart.x + _btnStart.w + GAP_BTN;
      int y = TFT_HEIGHT - gapBottom - TB_H;
      _btnTB = { x,y, TB_W, TB_H };
    }

    _startDirty = true;
    _tbDirty    = true;
  }

  void applyOccludersForUI(){
    path->clearOccluders();
    path->addOccluder(0, 0, TFT_WIDTH, Theme::BAR_H);               // top bar
    path->addOccluder(0, _compY, TFT_WIDTH, _compH);                 // kompas
    if (_lH > 0) path->addOccluder(_lX, _lY, _lW, _lH);              // left box
    if (_rH > 0) path->addOccluder(_rX, _rY, _rW, _rH);              // right box
    path->addOccluder(_btnStart.x, _btnStart.y, _btnStart.w, _btnStart.h);
    path->addOccluder(_btnTB.x,    _btnTB.y,    _btnTB.w,    _btnTB.h);
  }

  void drawButtonTile(int x,int y,int w,int h,int radius,
                      uint8_t cr,uint8_t cg,uint8_t cb,
                      const char* label,int scale=2,bool bold=true){
    _os->blit.blit(_os->ui.tft(), _os->bg, x,y,w,h, [&](GfxRGB888& gfx,int, int,int Rw,int Rh){
      fillRoundedRectLocal(gfx, Rw, Rh, radius, cr,cg,cb);
      const int tw = SimpleFont::textWidth(label, scale, 1);
      const int tx = (Rw - tw)/2;
      const int ty = (Rh/2) + 7*scale/2;
      SimpleFont::drawTextStyled(
        gfx, tx, ty, label,
        BTN_TX_R, BTN_TX_G, BTN_TX_B,
        scale, 1, SimpleFont::AlignLeft, -1,
        bold ? SimpleFont::Bold : SimpleFont::Normal
      );
    });
  }

  void drawStartButton(){
    const bool stopState = _started;

    uint8_t rr,gg,bb;
    const char* txt;

    if (!_gnssEverLocked){
      // GNSS belum pernah lock → tombol Start disabled (abu-abu)
      rr = TB_BASE_R;
      gg = TB_BASE_G;
      bb = TB_BASE_B;
      txt = "Start";
    } else {
      // Normal: hijau untuk Start, merah untuk Stop
      rr  = stopState ? START_OFF_R : START_ON_R;
      gg  = stopState ? START_OFF_G : START_ON_G;
      bb  = stopState ? START_OFF_B : START_ON_B;
      txt = stopState ? "Stop" : "Start";
    }

    drawButtonTile(_btnStart.x,_btnStart.y,_btnStart.w,_btnStart.h,
                   START_R, rr,gg,bb, txt, 2,true);
  }

  void drawTrackBackButton(){
    const bool enabled = hasAnyPath();
    const bool hi = _tbPressedVisual && enabled;

    uint8_t rr, gg, bb;

    if (!enabled){
      // Disabled
      rr = TB_BASE_R;
      gg = TB_BASE_G;
      bb = TB_BASE_B;
    } else {
      // Enabled normal
      rr = hi ? TB_HI_R : TB_BASE_R;
      gg = hi ? TB_HI_G : TB_BASE_G;
      bb = hi ? TB_HI_B : TB_BASE_B;
    }

    drawButtonTile(_btnTB.x,_btnTB.y,_btnTB.w,_btnTB.h,
                  TB_R, rr,gg,bb, "Track Back", 2,true);
  }

  void drawBottomButtonsIfDirty(bool force){
    if (force || _startDirty){ drawStartButton(); _startDirty=false; }
    if (force || _tbDirty)   { drawTrackBackButton(); _tbDirty=false; }
  }

  void drawCompassBandOnce(){
    int ox=0, oy=_compY; int ow=TFT_WIDTH, oh=_compH;
    _os->blit.blit(_os->ui.tft(), _os->bg, ox,oy,ow,oh, [&](GfxRGB888& gfx,int Rrx,int Rry,int, int){
      compass.paint(gfx, Rrx, Rry);
    });
  }

  void drawRightBoxTextLocal(GfxRGB888& g, int xLocal, int yBaseLocal, int val){
    const int scale = 1;
    char buf[16];
    if (val < 0) strcpy(buf, "null");
    else snprintf(buf, sizeof(buf), "%d", val);
    SimpleFont::drawTextStyled(g, xLocal, yBaseLocal, "=", 255,255,255, scale,1, SimpleFont::AlignLeft,-1, SimpleFont::Bold);
    const int wEq = SimpleFont::textWidth("=", scale, 1);
    SimpleFont::drawTextStyled(g, xLocal + wEq + 4, yBaseLocal, buf, 255,255,255, scale,1, SimpleFont::AlignLeft,-1, SimpleFont::Bold);
  }

  void paintRightBoxStatus(){
    if (_rH <= 0) return;
    _os->blit.blit(_os->ui.tft(), _os->bg, _rX, _rY, _rW, _rH, [&](GfxRGB888& gfx,int Rrx,int Rry,int Rw,int Rh){
      fillRoundedRectLocal(gfx, Rw, Rh, Theme::BOX_RADIUS, Theme::UI_R,Theme::UI_G,Theme::UI_B);
      netIconNoBg(gfx, Rrx,Rry, _netCX,_netCY, _netR, 255,255,255);
      const int yBaseLocal = (_rY + _rH/2) + (7*1/2) - Rry;
      const int netTextXLocal = (_netCX + _netR + 6) - Rrx;
      drawRightBoxTextLocal(gfx, netTextXLocal, yBaseLocal, _netVal);
      satelliteIcon(gfx, Rrx,Rry, _satX, _satY, _satScale, _satRotDeg);
      const int satTextXLocal = (_satX + 12) - Rrx;
      drawRightBoxTextLocal(gfx, satTextXLocal, yBaseLocal, _satVal);
    });
    _rightBoxDirty = false;
  }

  void composeTelemetry(char out0[32], char out1[32], char out2[32], char out3[32]){
    const uint32_t now      = millis();
    const uint32_t MAX_AGE  = 5000;

    bool  tempOK = false, humOK = false, altOK = false, mslOK = false;
    float tempC  = 0.0f,  humPct = 0.0f, altM = 0.0f, mslM = 0.0f;

    // SHT30
    uint32_t ageSHT = now - SHT::lastUpdateMs;
    if (SHT::tempValid && ageSHT < MAX_AGE){
      tempC  = SHT::tempC;
      tempOK = true;
    }
    if (SHT::humValid && ageSHT < MAX_AGE){
      humPct = SHT::humPct;
      humOK  = true;
    }

    // BMP280
    uint32_t ageBMP = now - BMP::lastUpdateMs;
    if (BMP::altValid && ageBMP < MAX_AGE){
      mslM  = BMP::altMSL_m;
      mslOK = true;

      if (BMP::relAltValid){
        altM  = BMP::relAlt_m;
        altOK = true;
      } else {
        altM  = BMP::altMSL_m;
        altOK = true;
      }
    }

    if (tempOK) snprintf(out0,32, "%0.1f °C",   tempC);
    else        snprintf(out0,32, "null");

    if (humOK)  snprintf(out1,32, "%0.0f %%",   humPct);
    else        snprintf(out1,32, "null");

    if (altOK)  snprintf(out2,32, "%0.1f m",    altM);
    else        snprintf(out2,32, "null");

    if (mslOK)  snprintf(out3,32, "%0.1f mdpl", mslM);
    else        snprintf(out3,32, "null");
  }

  void updateTelemetryDirtyFlag(){
    char now0[32], now1[32], now2[32], now3[32];
    composeTelemetry(now0,now1,now2,now3);
    if (strcmp(now0,_lastLine[0]) || strcmp(now1,_lastLine[1]) ||
        strcmp(now2,_lastLine[2]) || strcmp(now3,_lastLine[3])){
      strncpy(_lastLine[0], now0, 32);
      strncpy(_lastLine[1], now1, 32);
      strncpy(_lastLine[2], now2, 32);
      strncpy(_lastLine[3], now3, 32);
      _leftBoxDirty = true;
    }
  }

  void paintLeftBoxContentIfDirty(){
    if (!_leftBoxDirty || _lH<=0) return;
    const int padX = 10, padY = 16;
    const int scale = 1;
    const int gap   = 5;
    const int lineH = 7*scale + gap;

    const char* L0="Temp"; const char* L1="Humid"; const char* L2="Alt"; const char* L3="MSL";
    const int w0 = SimpleFont::textWidth(L0, scale, 1);
    const int w1 = SimpleFont::textWidth(L1, scale, 1);
    const int w2 = SimpleFont::textWidth(L2, scale, 1);
    const int w3 = SimpleFont::textWidth(L3, scale, 1);
    const int labelW = max(max(w0,w1), max(w2,w3));

    const int cx = _lX + padX;
    const int cy = _lY + padY;
    int cw = max(0, _lW - 2*padX);
    int ch = max(0, _lH - 2*padY);

    _os->blit.blit(_os->ui.tft(), _os->bg, cx,cy,cw,ch, [&](GfxRGB888& gfx,int,int,int Rw,int Rh){
      uint8_t* p = gfx.pix; size_t N = (size_t)Rw*Rh;
      for (size_t i=0;i<N;++i){ *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B; }

      auto drawRow = [&](int rowY, const char* lbl, const char* val){
        SimpleFont::drawTextStyled(gfx, 0, rowY, lbl, 255,255,255, scale,1, SimpleFont::AlignLeft,-1, SimpleFont::Bold);
        const int colonX = labelW + 2;
        SimpleFont::drawTextStyled(gfx, colonX, rowY, ":", 255,255,255, scale,1, SimpleFont::AlignLeft,-1, SimpleFont::Bold);
        const int valX = colonX + SimpleFont::textWidth(":", scale,1) + 6;
        SimpleFont::drawTextStyled(gfx, valX, rowY, val, 255,255,255, scale,1, SimpleFont::AlignLeft,-1, SimpleFont::Bold);
      };

      const int y0 = 7*scale;
      drawRow(y0 + 0*lineH, L0, _lastLine[0]);
      drawRow(y0 + 1*lineH, L1, _lastLine[1]);
      drawRow(y0 + 2*lineH, L2, _lastLine[2]);
      drawRow(y0 + 3*lineH, L3, _lastLine[3]);
    });

    _leftBoxDirty = false;
  }
};
