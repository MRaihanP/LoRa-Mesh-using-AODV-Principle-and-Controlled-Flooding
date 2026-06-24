#pragma once
#include "PageBase.h"
#include "pins_and_config.h"
#include "BuzzerTone.h"
#include <math.h>
#include <ctype.h>
#include <string.h>

class PageGNSS_M10 : public PageBase {
public:
  void begin(AppOS* os, TopBar* top){
    PageBase::begin(os, top);
    setTitle("GNSS M10");

    _gps = &Serial1;

    // Initial 9600 seperti sketch tester
    _gps->begin(9600, SERIAL_8N1,
                PinsAndConfig::GNSS::GPS_RX,
                PinsAndConfig::GNSS::GPS_TX);
    delay(50);

    // Auto-baud detect
    long used_baud = auto_baud();
    // Paksa ke 115200 + enable RMC/GGA/GSV/GSA/GNS seperti tester
    if (used_baud != 115200){
      configureUbxTo115200AndMessages();
      (void)try_baud(115200, 800);    // optional verify
    } else {
      configureUbxTo115200AndMessages();
    }

    // Layout awal
    recomputeLayout();
    updateContentHeight();
  }

  void onEnter() override {
    PageBase::onEnter();
    recomputeLayout();
    updateContentHeight();
  }

  void onQSClosed() override {
    if (!isActive()) return;
    recomputeLayout();
    updateContentHeight();
    PageBase::onQSClosed();
  }

  int contentHeight() const { return _contentH; }

  // Backend ringan: dipanggil tiap loop() dari RP_GUI_OOP_dev.ino,
  // berjalan terus walau page tidak dibuka.
  // --- periodic status (1 Hz) + push ke global snapshot + PDR ---
  void backendTick(uint32_t nowMs) {
    if (!_gps) return;

    // --- read chars / build lines (mirip loop() tester) ---
    while (_gps->available()){
      int c = _gps->read();
      if (c < 0) break;
      _bytes_in++;
      _last_byte_ms = millis();
      if (c == '\r') continue;
      if (c == '\n'){
        if (_linelen > 0){
          _line[_linelen] = '\0';
          if (_line[0] == '$' && nmea_checksum_ok(_line)){
            _valid_nmea++;
            char tmp[160];
            strncpy(tmp, _line, sizeof(tmp));
            tmp[sizeof(tmp)-1] = '\0';

            if      (nmea_is_type(tmp, "RMC")) parse_rmc(tmp);
            else if (nmea_is_type(tmp, "GGA")) parse_gga(tmp);
            else if (nmea_is_type(tmp, "GSV")) parse_gsv(tmp);
          }
        }
        _linelen = 0;
        continue;
      }
      if (_linelen + 1 < sizeof(_line)) _line[_linelen++] = (char)c;
      else _linelen = 0; // overflow reset
    }

    // --- periodic status (1 Hz) + push ke global snapshot + PDR ---
    if (nowMs - _lastStatusPushMs >= 1000){
      _lastStatusPushMs = nowMs;

      bool connected   = (millis() - _last_byte_ms) < DATA_TIMEOUT_MS;
      bool lock_recent = (millis() - _last_fix_ms)  < FIX_STALE_MS;
      bool locked      = connected && lock_recent;

      // Nada GPS locked (transisi false -> true)
      if (locked && !_prevLocked) {
        BuzzerTone::toneGpsLocked();
      }
      _prevLocked = locked;

      // Derive speed / heading -> vN/vE dari GNSS (RMC)
      if (locked && _rmc_has_fix){
        float speed_mps = (float)(_rmc_speed_kn * 0.514444f);
        float track_deg = (float)_rmc_track_deg;
        float rad       = track_deg * (float)(M_PI / 180.0f);
        _speed2D_mps    = speed_mps;
        _speed3D_mps    = speed_mps;
        _groundTrackDeg = track_deg;
        _velN_mps       = speed_mps * cosf(rad);
        _velE_mps       = speed_mps * sinf(rad);
        _velD_mps       = 0.0f;
      } else {
        _speed2D_mps    = 0.0f;
        _speed3D_mps    = 0.0f;
        _groundTrackDeg = NAN;
        _velN_mps       = 0.0f;
        _velE_mps       = 0.0f;
        _velD_mps       = 0.0f;
      }

      // Alt / HDOP
      _altMSL_m = (float)_gga_alt_m;
      if (_gga_hdop > 0.0){
        // heuristik: akurasi horizontal ~ HDOP * 5 m
        _hAcc_m = (float)(_gga_hdop * 5.0);
      } else {
        _hAcc_m = NAN;
      }
      _vAcc_m = NAN;

      // Posisi (GNSS raw)
      _latDeg = (float)_rmc_lat_deg;
      _lonDeg = (float)_rmc_lon_deg;

      _hasPVT       = (_valid_nmea > 0);
      _lastUpdateMs = nowMs;

      // === PDR: origin + hybrid GPS/DR (GNSS + IMU) ===
      updatePDR(nowMs, locked);

      // === Pilih sumber posisi: GNSS murni atau PDR ===
      float  outLatDeg   = NAN;
      float  outLonDeg   = NAN;
      float  outAltMSL_m = NAN;
      float  outHAcc_m   = NAN;
      float  outVAcc_m   = NAN;
      float  outSpeed2D  = 0.0f;
      float  outSpeed3D  = 0.0f;
      float  outTrackDeg = NAN;
      float  outVelN     = 0.0f;
      float  outVelE     = 0.0f;
      float  outVelD     = 0.0f;

      uint8_t outFixType   = 0;    // 0 = no fix, 1 = DR only, 3 = 3D GNSS
      bool    outHasFix    = false;
      bool    outGnssFixOK = false;

      // 1) Prioritas: GNSS kalau masih locked dan posisi valid
      if (locked && isfinite(_latDeg) && isfinite(_lonDeg)) {
        outHasFix    = true;
        outGnssFixOK = true;
        outFixType   = 3;          // 3D GNSS fix

        outLatDeg    = _latDeg;
        outLonDeg    = _lonDeg;
        outAltMSL_m  = _altMSL_m;
        outHAcc_m    = _hAcc_m;
        outVAcc_m    = _vAcc_m;

        outSpeed2D   = _speed2D_mps;
        outSpeed3D   = _speed3D_mps;
        outTrackDeg  = _groundTrackDeg;

        outVelN      = _velN_mps;
        outVelE      = _velE_mps;
        outVelD      = _velD_mps;
      }
      // 2) Kalau GNSS tidak locked tapi PDR punya posisi → pakai PDR
      else if (_pdrOriginSet &&
               isfinite(_pdrLatDeg) &&
               isfinite(_pdrLonDeg))
      {
        outHasFix    = true;       // masih punya posisi "best effort"
        outGnssFixOK = false;      // bukan GNSS
        outFixType   = 1;          // DR only

        // Posisi dari PDR (lat/lon yang sudah dikonversi dari X/Y)
        outLatDeg    = _pdrLatDeg;
        outLonDeg    = _pdrLonDeg;

        // Tinggi & vAcc boleh pakai nilai terakhir dari GNSS (atau BMP280)
        outAltMSL_m  = _altMSL_m;
        outVAcc_m    = _vAcc_m;

        // Akurasi horizontal PDR
        outHAcc_m    = _pdrHAcc_m;

        // Kecepatan & heading dari PDR (forward velocity + heading)
        outSpeed2D   = _pdrForwardVel_mps;
        outSpeed3D   = _pdrForwardVel_mps;
        outTrackDeg  = _pdrLastHeadingDeg;

        // Komponen kecepatan lokal N/E dari forward velocity
        if (isfinite(_pdrLastHeadingDeg) && _pdrForwardVel_mps > 0.0f){
          float rad = _pdrLastHeadingDeg * (float)(M_PI / 180.0f);
          outVelN = _pdrForwardVel_mps * cosf(rad);
          outVelE = _pdrForwardVel_mps * sinf(rad);
        } else {
          outVelN = 0.0f;
          outVelE = 0.0f;
        }
        outVelD = 0.0f;
      }

      // === Push ke snapshot global PinsAndConfig::GNSS ===
      PinsAndConfig::GNSS::connected      = connected;
      PinsAndConfig::GNSS::hasFix         = outHasFix;
      PinsAndConfig::GNSS::fixType        = outFixType;
      PinsAndConfig::GNSS::gnssFixOK      = outGnssFixOK;
      PinsAndConfig::GNSS::numSV          = _gga_sats;      // sats in use (GGA)

      PinsAndConfig::GNSS::latDeg         = outLatDeg;
      PinsAndConfig::GNSS::lonDeg         = outLonDeg;
      PinsAndConfig::GNSS::altMSL_m       = outAltMSL_m;

      PinsAndConfig::GNSS::hAcc_m         = outHAcc_m;
      PinsAndConfig::GNSS::vAcc_m         = outVAcc_m;

      PinsAndConfig::GNSS::speed2D_mps    = outSpeed2D;
      PinsAndConfig::GNSS::speed3D_mps    = outSpeed3D;
      PinsAndConfig::GNSS::groundTrackDeg = outTrackDeg;

      PinsAndConfig::GNSS::velN_mps       = outVelN;
      PinsAndConfig::GNSS::velE_mps       = outVelE;
      PinsAndConfig::GNSS::velD_mps       = outVelD;

      PinsAndConfig::GNSS::lastUpdateMs   = _lastUpdateMs;
      if (outHasFix) {
        PinsAndConfig::GNSS::lastFixMs = _lastUpdateMs;
      }

      // GNSS epoch UTC (dari RMC) untuk time sync RTC
      PinsAndConfig::GNSS::timeValid = _rmc_time_valid && locked;
      if (PinsAndConfig::GNSS::timeValid){
        PinsAndConfig::GNSS::epochUtc   = _rmc_epoch_utc;
        PinsAndConfig::GNSS::lastTimeMs = nowMs;
      }

      _contentDirty = true;
    }
  }

protected:
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const int RryEff = Rry + _scrollY;

    drawStatusCard   (g, Rrx, RryEff, _cardStatus);
    drawPositionCard (g, Rrx, RryEff, _cardPos);
    drawVelocityCard (g, Rrx, RryEff, _cardVel);
    drawPDRCard      (g, Rrx, RryEff, _cardPDR);   // card PDR baru
    drawDebugCard    (g, Rrx, RryEff, _cardDebug);

    _contentDirty = false;
  }

  void handleContentInput(bool pressed, int x, int y) override {
    // Scroll sederhana (drag vertical)
    const int cy = y + _scrollY;
    (void)cy;

    if (pressed) {
      if (!_dragging) {
        _dragging = true;
        _dragY0   = y;
        _scrollY0 = _scrollY;
      } else {
        const int dy = y - _dragY0;
        int newScroll = _scrollY0 - dy;
        if (newScroll < 0) newScroll = 0;
        if (newScroll > _scrollMax) newScroll = _scrollMax;
        _scrollY = newScroll;
      }
    } else {
      _dragging = false;
    }
  }

private:
  using Theme   = PinsAndConfig::Theme;
  using GNSSCfg = PinsAndConfig::GNSS;
  using IMUCfg  = PinsAndConfig::IMU;   // NOTE: struct ini harus ada di pins_and_config.h

  struct Rect { int x=0,y=0,w=0,h=0; };

  static inline int   imax(int a,int b){ return (a>b)?a:b; }
  static inline int   imin(int a,int b){ return (a<b)?a:b; }
  static inline float clampf(float v,float lo,float hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  static inline float norm360f(float a){
    a = fmodf(a, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a;
  }
  static inline float wrapDeg180f(float a){
    a = fmodf(a, 360.0f);
    if (a < -180.0f) a += 360.0f;
    if (a >  180.0f) a -= 360.0f;
    return a;
  }

  // --- Layout constants ---
  static constexpr int RADIUS      = 12;
  static constexpr int EDGE        = Theme::EDGE_BUF;
  static constexpr int GAPY12      = 6;
  static constexpr int GAPY23      = 6;
  static constexpr int GAPY34      = 6;
  static constexpr int TITLE2CARD  = 10;
  static constexpr int BOT_PAD     = 20;

  // Timeouts
  static constexpr unsigned DATA_TIMEOUT_MS = 3000; // no bytes => disconnected
  static constexpr unsigned FIX_STALE_MS    = 4000; // last fix too old => lock lost
  static constexpr unsigned STALE_GSV_MS    = 6000; // opsional untuk GSV

  // IMU / DR params
  static constexpr uint32_t IMU_STALE_MS        = 300;    // IMU dianggap stale kalau >300 ms
  static constexpr float    PDR_SPEED_DECAY_TAU = 8.0f;   // detik untuk decay speed
  static constexpr float    IMU_ALIGN_ALPHA     = 0.10f;  // smoothing offset yaw
  static constexpr float    PDR_MAX_SPEED_MPS   = 8.0f;   // batas wajar (≈ 29 km/h)
  static constexpr float    PDR_ACC_DEADZONE    = 0.05f;  // m/s², anggap noise kalau di bawah ini
  static constexpr float    PDR_ACC_GAIN        = 1.0f;   // skala kontribusi accForward ke speed
  static constexpr float    PDR_ACC_STILL_THR   = 0.12f;  // kalau |a| < ini beberapa detik → anggap diam

  // Card heights
  int _hStatus = 86;
  int _hPos    = 110;
  int _hVel    = 110;
  int _hPDR    = 130;   // card PDR sedikit lebih tinggi
  int _hDebug  = 120;

  Rect _cardStatus;
  Rect _cardPos;
  Rect _cardVel;
  Rect _cardPDR;
  Rect _cardDebug;

  // Scroll state
  int  _contentH   = 0;
  int  _scrollY    = 0;
  int  _scrollMax  = 0;
  bool _dragging   = false;
  int  _dragY0     = 0;
  int  _scrollY0   = 0;

  bool _contentDirty = true;

  // GNSS serial
  HardwareSerial* _gps = nullptr;

  // NMEA parsing / state
  char            _line[160];
  size_t          _linelen          = 0;
  unsigned long   _bytes_in         = 0;
  unsigned long   _valid_nmea       = 0;
  unsigned long   _last_byte_ms     = 0;
  unsigned long   _last_fix_ms      = 0;
  uint32_t        _lastStatusPushMs = 0;

  // RMC
  bool   _rmc_has_fix   = false;
  double _rmc_lat_deg   = 0.0;
  double _rmc_lon_deg   = 0.0;
  double _rmc_speed_kn  = 0.0;
  double _rmc_track_deg = 0.0;

  // GNSS time (UTC) dari RMC (ddmmyy + hhmmss)
  bool     _rmc_time_valid = false;
  uint32_t _rmc_epoch_utc  = 0;

  // GGA
  int    _gga_fix_quality = 0;
  int    _gga_sats        = 0;
  double _gga_hdop        = 0.0;
  double _gga_alt_m       = 0.0;

  // GSV (total sats in view jika mau)
  int           _gsv_sats_view_total = 0;
  unsigned long _gsv_last_seen_ms    = 0;

  bool     _hasPVT       = false;
  uint32_t _lastUpdateMs = 0;

  // Data turunan float (untuk card GNSS)
  float _latDeg         = NAN;
  float _lonDeg         = NAN;
  float _altMSL_m       = NAN;
  float _hAcc_m         = NAN;
  float _vAcc_m         = NAN;
  float _velN_mps       = 0.0f;
  float _velE_mps       = 0.0f;
  float _velD_mps       = 0.0f;
  float _speed2D_mps    = 0.0f;
  float _speed3D_mps    = 0.0f;
  float _groundTrackDeg = NAN;

  // === PDR: origin + hybrid GPS/Dead-Reckoning ===
  // Origin (ditetapkan saat pertama kali dapat GNSS fix)
  bool   _pdrOriginSet       = false;
  double _pdrLat0Deg         = 0.0;
  double _pdrLon0Deg         = 0.0;
  float  _pdrAlt0_m          = 0.0f;

  // Posisi PDR di koordinat lokal (North/East, meter, relatif origin)
  float  _pdrNorth_m         = 0.0f;   // fused pos N
  float  _pdrEast_m          = 0.0f;   // fused pos E

  // Info turunan PDR
  float  _pdrHorizDist_m     = 0.0f;   // jarak horizontal dari origin (m)
  float  _pdrBearingDeg      = NAN;    // bearing origin -> pos PDR (0..360)
  float  _pdrLatDeg          = NAN;    // posisi lat hasil PDR (approx)
  float  _pdrLonDeg          = NAN;    // posisi lon hasil PDR (approx)
  float  _pdrHAcc_m          = NAN;    // estimasi akurasi horizontal PDR

  // Mode PDR: 0=No origin yet, 1=GPS (snap GNSS), 2=DR (bridging)
  uint8_t   _pdrMode         = 0;
  uint32_t  _pdrLastUpdateMs = 0;

  // Forward velocity (di sepanjang heading user, m/s)
  float     _pdrForwardVel_mps     = 0.0f;

  // Heading fused (GNSS + IMU)
  float     _pdrLastHeadingDeg     = NAN;

  // IMU alignment (offset antara yaw IMU & track GNSS)
  float _imuYawOffsetDeg     = 0.0f;
  bool  _imuAlignValid       = false;

  // Deteksi "sedang diam" berbasis accel
  float    _stillAccumTime_s = 0.0f;

  // === State untuk tone GPS lock ===
  bool  _prevLocked          = false;

  // === Layout helpers ===
  void recomputeLayout(){
    const int cardsTop = _viewportY0 + TITLE2CARD;
    const int fullW    = TFT_WIDTH - 2*EDGE;
    int y = cardsTop;

    _cardStatus = { EDGE, y, fullW, _hStatus }; y += _hStatus + GAPY12;
    _cardPos    = { EDGE, y, fullW, _hPos    }; y += _hPos    + GAPY23;
    _cardVel    = { EDGE, y, fullW, _hVel    }; y += _hVel    + GAPY34;
    _cardPDR    = { EDGE, y, fullW, _hPDR    }; y += _hPDR    + GAPY34;
    _cardDebug  = { EDGE, y, fullW, _hDebug  }; y += _hDebug  + BOT_PAD;
  }

  void updateContentHeight(){
    _contentH = _cardDebug.y + _cardDebug.h;
    int vpH = _viewportH;
    int maxY = _contentH - vpH;
    if (maxY < 0) maxY = 0;
    _scrollMax = maxY;
    if (_scrollY > _scrollMax) _scrollY = _scrollMax;
  }

  // === PDR core (GNSS + IMU) ===
  void updatePDR(uint32_t nowMs, bool gnssLocked){
    const double R       = 6371000.0;       // radius bumi (m)
    const double deg2rad = M_PI / 180.0;
    const double rad2deg = 180.0 / M_PI;

    // --- Hook IMU snapshot ---
    float    imuYawDeg     = NAN;
    float    imuAccF       = NAN;  // accel forward (m/s²)
    bool     imuFresh      = false;
    uint32_t imuLast       = IMUCfg::lastUpdateMs;

    if (imuLast != 0 && (nowMs - imuLast) < IMU_STALE_MS) {
      // yawRad di sini DIANGGAP dalam derajat 0..360 (kamu yang norma­lisasi di backend)
      imuYawDeg = IMUCfg::yawRad;
      imuAccF   = IMUCfg::accForward_mps2;
      if (isfinite(imuYawDeg) && isfinite(imuAccF)) {
        imuFresh = true;
      }
    }

    // 1) Set origin ketika pertama kali dapat GNSS fix valid
    if (!_pdrOriginSet) {
      if (gnssLocked && isfinite(_latDeg) && isfinite(_lonDeg)) {
        _pdrOriginSet    = true;
        _pdrLat0Deg      = (double)_latDeg;
        _pdrLon0Deg      = (double)_lonDeg;
        _pdrAlt0_m       = _altMSL_m;
        _pdrNorth_m      = 0.0f;
        _pdrEast_m       = 0.0f;
        _pdrHorizDist_m  = 0.0f;
        _pdrBearingDeg   = NAN;
        _pdrLatDeg       = _latDeg;
        _pdrLonDeg       = _lonDeg;
        _pdrMode         = 1;  // GPS
        _pdrHAcc_m       = isfinite(_hAcc_m) ? _hAcc_m : 10.0f;
        _pdrLastUpdateMs = nowMs;
        _pdrForwardVel_mps = _speed2D_mps;
        _stillAccumTime_s  = 0.0f;
      } else {
        // Belum ada origin sama sekali → mode 0 ("No origin yet")
        _pdrMode = 0;
        return;
      }
    }

    // Hitung dt
    float dt = (_pdrLastUpdateMs == 0) ?
                1.0f : (float)(nowMs - _pdrLastUpdateMs) * 0.001f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 2.0f) dt = 2.0f;

    // 2) Jika GNSS locked → snap posisi PDR ke GNSS (di local frame)
    if (gnssLocked && isfinite(_latDeg) && isfinite(_lonDeg)) {
      double lat0 = _pdrLat0Deg * deg2rad;
      double lon0 = _pdrLon0Deg * deg2rad;
      double lat1 = (double)_latDeg * deg2rad;
      double lon1 = (double)_lonDeg * deg2rad;

      double dLat    = lat1 - lat0;
      double dLon    = lon1 - lon0;
      double meanLat = 0.5 * (lat0 + lat1);

      // equirectangular -> x=East, y=North
      double x = dLon * cos(meanLat) * R;
      double y = dLat * R;

      _pdrNorth_m = (float)y;
      _pdrEast_m  = (float)x;
      _pdrMode    = 1; // GPS mode

      // Forward velocity referensi dari GNSS
      _pdrForwardVel_mps = _speed2D_mps;

      // --- Align IMU yaw ke track GNSS (kalau ada) ---
      if (isfinite(_groundTrackDeg)) {
        float refH = norm360f(_groundTrackDeg);
        if (imuFresh) {
          float imuH = norm360f(imuYawDeg);
          float desiredOffset = wrapDeg180f(refH - imuH);

          if (!_imuAlignValid) {
            _imuYawOffsetDeg = desiredOffset;
            _imuAlignValid   = true;
          } else {
            float err = wrapDeg180f(desiredOffset - _imuYawOffsetDeg);
            _imuYawOffsetDeg = wrapDeg180f(_imuYawOffsetDeg + IMU_ALIGN_ALPHA * err);
          }
        }
      }

      // Heading fused: kalau IMU align OK gunakan IMU+offset, else fallback GNSS track
      float fusedHeading = NAN;
      if (_imuAlignValid && imuFresh) {
        fusedHeading = norm360f(imuYawDeg + _imuYawOffsetDeg);
      } else if (isfinite(_groundTrackDeg)) {
        fusedHeading = norm360f(_groundTrackDeg);
      }
      _pdrLastHeadingDeg = fusedHeading;

      // Akurasi PDR = akurasi GNSS saat ini (baseline)
      _pdrHAcc_m = isfinite(_hAcc_m) ? _hAcc_m : _pdrHAcc_m;

      _pdrLastUpdateMs = nowMs;
      _stillAccumTime_s = 0.0f;
    }
    // 3) Jika GNSS tidak locked → DR bridging dengan IMU
    else {
      _pdrMode = 2;  // DR mode (GNSS+IMU)

      // 3a) Update heading dari IMU bila align valid
      if (_imuAlignValid && imuFresh) {
        float newHeading = norm360f(imuYawDeg + _imuYawOffsetDeg);
        // Smooth sedikit heading jika mau, tapi di sini langsung pakai saja
        _pdrLastHeadingDeg = newHeading;
      }

      // 3b) Update forward velocity dari IMU accel
      if (imuFresh) {
        float a = imuAccF;

        // Deadzone: noise kecil diabaikan
        if (fabsf(a) < PDR_ACC_DEADZONE) {
          a = 0.0f;
        }

        if (a == 0.0f) {
          // Tidak ada akselerasi signifikan → akumulasi "still time"
          _stillAccumTime_s += dt;
        } else {
          _stillAccumTime_s = 0.0f;
        }

        // Integrasi accel -> speed
        _pdrForwardVel_mps += (a * PDR_ACC_GAIN) * dt;

        // Kalau sudah lama diam dan accel kecil, paksa speed turun ke 0
        if (_stillAccumTime_s > 1.5f) {
          // Decay cepat menuju 0
          _pdrForwardVel_mps *= expf(-dt / 0.5f);
          if (fabsf(_pdrForwardVel_mps) < 0.02f) {
            _pdrForwardVel_mps = 0.0f;
          }
        } else {
          // Kalau masih aktif bergerak, gunakan decay lambat supaya tidak runaway
          if (PDR_SPEED_DECAY_TAU > 0.1f) {
            float decay = expf(-dt / PDR_SPEED_DECAY_TAU);
            _pdrForwardVel_mps *= decay;
          }
        }
      } else {
        // IMU stale → hanya decay pelan
        if (PDR_SPEED_DECAY_TAU > 0.1f && _pdrForwardVel_mps != 0.0f) {
          float decay = expf(-dt / PDR_SPEED_DECAY_TAU);
          _pdrForwardVel_mps *= decay;
          if (fabsf(_pdrForwardVel_mps) < 0.02f) {
            _pdrForwardVel_mps = 0.0f;
          }
        }
      }

      // Clamp speed agar tetap wajar
      _pdrForwardVel_mps = clampf(_pdrForwardVel_mps, 0.0f, PDR_MAX_SPEED_MPS);

      // 3c) Integrasi posisi PDR dengan forward velocity + heading
      if (isfinite(_pdrLastHeadingDeg) && _pdrForwardVel_mps > 0.0f) {
        float rad = _pdrLastHeadingDeg * (float)(M_PI / 180.0f);
        float dN  = _pdrForwardVel_mps * dt * cosf(rad);
        float dE  = _pdrForwardVel_mps * dt * sinf(rad);
        _pdrNorth_m += dN;
        _pdrEast_m  += dE;

        // Akurasi PDR sedikit memburuk seiring jarak/ waktu (misal 0.5 m / detik)
        if (isfinite(_pdrHAcc_m)) {
          _pdrHAcc_m += 0.5f * dt;
        } else {
          _pdrHAcc_m = 10.0f;
        }
      }

      _pdrLastUpdateMs = nowMs;
    }

    // 4) Hitung jarak & bearing dari origin ke posisi PDR saat ini
    if (_pdrOriginSet) {
      float N = _pdrNorth_m;
      float E = _pdrEast_m;
      _pdrHorizDist_m = sqrtf(N*N + E*E);

      if (N == 0.0f && E == 0.0f) {
        _pdrBearingDeg = NAN;
      } else {
        float brad = atan2f(E, N);   // E, N -> 0=North, CW
        float bdeg = brad * (float)(180.0f / M_PI);
        if (bdeg < 0.0f) bdeg += 360.0f;
        _pdrBearingDeg = bdeg;
      }

      // 5) Konversi balik ke lat/lon approx dari N/E
      double lat0 = _pdrLat0Deg * deg2rad;
      double lon0 = _pdrLon0Deg * deg2rad;
      double dLat = (double)_pdrNorth_m / R;
      double dLon = (double)_pdrEast_m  / (R * cos(lat0));

      double latF = lat0 + dLat;
      double lonF = lon0 + dLon;

      _pdrLatDeg = (float)(latF * rad2deg);
      _pdrLonDeg = (float)(lonF * rad2deg);
    }
  }

  // === Super-light drawing helpers ===
  void fillRectClipped(GfxRGB888& g, int Rrx,int Rry,
                       int x,int y,int w,int h,
                       uint8_t rr,uint8_t gg,uint8_t bb)
  {
    if (w<=0 || h<=0) return;
    int x0 = imax(x, Rrx);
    int y0 = imax(y, Rry);
    int x1 = imin(x+w, Rrx+g.w);
    int y1 = imin(y+h, Rry+g.h);
    if (x1<=x0 || y1<=y0) return;
    for (int gy=y0; gy<y1; ++gy){
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=rr; row[1]=gg; row[2]=bb; row+=3; }
    }
  }

  void fillRoundedClipped(GfxRGB888& g, int Rrx, int Rry,
                          const Rect& r, int rad,
                          uint8_t rr,uint8_t gg,uint8_t bb)
  {
    if (r.w<=0 || r.h<=0) return;
    if (rad <= 2) { fillRectClipped(g, Rrx,Rry, r.x,r.y,r.w,r.h, rr,gg,bb); return; }

    const int x0=r.x, y0=r.y, w=r.w, h=r.h;
    int drawTop = imax(y0, Rry);
    int drawBot = imin(y0+h, Rry+g.h);
    if (drawBot <= drawTop) return;

    for (int gy=drawTop; gy<drawBot; ++gy){
      int yy = gy - y0;
      int inset=0;
      if (rad>0){
        if (yy < rad) inset = rad - 1 - yy;
        int y2 = (h - 1 - yy);
        if (y2 < rad){
          int inset2 = rad - 1 - y2;
          if (inset2 > inset) inset = inset2;
        }
        if (inset > rad) inset = rad;
      }
      int gx0 = imax(x0 + inset, Rrx);
      int gx1 = imin(x0 + w - inset, Rrx + g.w);
      if (gx1 <= gx0) continue;

      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (gx0 - Rrx))*3;
      for (int gx=gx0; gx<gx1; ++gx){ row[0]=rr; row[1]=gg; row[2]=bb; row+=3; }
    }
  }

  static void drawText(GfxRGB888& g, int Rrx,int Rry,
                       int x,int y, const char* txt, int scale,
                       bool bold=false){
    const int glyphH = 7*scale;
    const int xLoc   = x - Rrx;
    const int yBase  = (y - Rry) + glyphH;
    if (bold){
      SimpleFont::drawTextStyled(g, xLoc+1, yBase, txt, 255,255,255, scale, 1);
    }
    SimpleFont::drawTextStyled(g, xLoc,   yBase, txt, 255,255,255, scale, 1);
  }

  static const char* fixTypeStr(uint8_t ft){
    switch (ft){
      case 0: return "No fix";
      case 1: return "DR only";
      case 2: return "2D fix";
      case 3: return "3D fix";
      case 4: return "GNSS+DR";
      case 5: return "Time only";
      default: return "Unknown";
    }
  }

  static const char* yesNo(bool v){ return v ? "Yes" : "No"; }

  static const char* headingCardinal(float deg){
    if (!isfinite(deg)) return "--";
    static const char* names[8] = {"N","NE","E","SE","S","SW","W","NW"};
    deg = fmodf(deg, 360.0f); if (deg < 0) deg += 360.0f;
    int idx = (int)floorf((deg + 22.5f) / 45.0f) & 7;
    return names[idx];
  }

  static const char* pdrModeStr(uint8_t m){
    switch (m){
      case 1: return "GPS (locked)";
      case 2: return "DR (GNSS+IMU)";
      case 0:
      default: return "No origin yet";
    }
  }

  static const char* okWait(bool v){ return v ? "OK" : "WAIT"; }

  // === Cards ===
  void drawStatusCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "GNSS Status", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 6;
    int y = baseY;
    char ln[64];

    bool connected = GNSSCfg::connected;
    bool hasFix    = GNSSCfg::hasFix;

    snprintf(ln, sizeof(ln), "Module : %s", connected ? "Connected" : "No data");
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Fix    : %s (%s)",
             fixTypeStr(GNSSCfg::fixType),
             yesNo(GNSSCfg::gnssFixOK));
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "#SV    : %u", GNSSCfg::numSV);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    if (isfinite(GNSSCfg::hAcc_m)) {
      snprintf(ln, sizeof(ln), "hAcc  : %.1f m", GNSSCfg::hAcc_m);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }

    uint32_t lu = GNSSCfg::lastUpdateMs;
    if (lu != 0) {
      uint32_t ageMs = millis() - lu;
      snprintf(ln, sizeof(ln), "Last update: %.1f s ago", ageMs / 1000.0f);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false);
    }
  }

  void drawPositionCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Position (WGS84)", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 6;
    int y = baseY;
    char ln[80];

    if (!GNSSCfg::hasFix || !isfinite(GNSSCfg::latDeg)) {
      drawText(g, Rrx,Rry, card.x+pad, y, "No valid fix yet", 1, true);
      return;
    }

    snprintf(ln, sizeof(ln), "Lat : %.7f deg", GNSSCfg::latDeg);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Lon : %.7f deg", GNSSCfg::lonDeg);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    if (isfinite(GNSSCfg::altMSL_m)) {
      snprintf(ln, sizeof(ln), "Alt : %.2f m (MSL)", GNSSCfg::altMSL_m);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }

    if (isfinite(GNSSCfg::hAcc_m)) {
      snprintf(ln, sizeof(ln), "Est. horizontal accuracy: %.1f m", GNSSCfg::hAcc_m);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }
  }

  void drawVelocityCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Velocity", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 6;
    int y = baseY;
    char ln[80];

    if (!GNSSCfg::hasFix) {
      drawText(g, Rrx,Rry, card.x+pad, y, "No fix -> velocity invalid", 1, true);
      return;
    }

    float v2d = GNSSCfg::speed2D_mps;
    float v3d = GNSSCfg::speed3D_mps;

    snprintf(ln, sizeof(ln), "Speed 2D : %.2f m/s (%.1f km/h)",
             v2d, v2d * 3.6f);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Speed 3D : %.2f m/s", v3d);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Vel N/E/D: %.2f / %.2f / %.2f m/s",
             _velN_mps, _velE_mps, _velD_mps);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    if (isfinite(GNSSCfg::groundTrackDeg)) {
      float hd  = GNSSCfg::groundTrackDeg;
      const char* cardDir = headingCardinal(hd);
      snprintf(ln, sizeof(ln), "Track   : %.1f deg (%s)", hd, cardDir);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }
  }

  void drawPDRCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "PDR / Hybrid GNSS+IMU", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 6;
    int y = baseY;
    char ln[96];

    if (!_pdrOriginSet) {
      drawText(g, Rrx,Rry, card.x+pad, y,
               "Origin: waiting first GNSS fix...", 1, true);
      return;
    }

    // Mode + origin
    snprintf(ln, sizeof(ln), "Mode   : %s", pdrModeStr(_pdrMode));
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Origin : %.6f / %.6f",
             _pdrLat0Deg, _pdrLon0Deg);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    // Posisi PDR (lat/lon approx)
    if (isfinite(_pdrLatDeg) && isfinite(_pdrLonDeg)) {
      snprintf(ln, sizeof(ln), "PDR pos: %.6f / %.6f",
               _pdrLatDeg, _pdrLonDeg);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }

    // Offset di frame lokal
    snprintf(ln, sizeof(ln), "Offset N/E : %+0.1f / %+0.1f m",
             _pdrNorth_m, _pdrEast_m);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, true); y += 12;

    snprintf(ln, sizeof(ln), "Distance   : %.1f m", _pdrHorizDist_m);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    if (isfinite(_pdrBearingDeg)) {
      const char* dir = headingCardinal(_pdrBearingDeg);
      snprintf(ln, sizeof(ln), "Bearing    : %.1f deg (%s)",
               _pdrBearingDeg, dir);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }

    if (isfinite(_pdrHAcc_m)) {
      snprintf(ln, sizeof(ln), "hAcc (PDR): %.1f m", _pdrHAcc_m);
      drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;
    }

    // Info IMU + heading fused + forward velocity
    snprintf(ln, sizeof(ln), "IMU align : %s  off: %.1f deg",
             okWait(_imuAlignValid), _imuYawOffsetDeg);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "v_fwd,hdg: %.2f m/s, %.1f deg",
             _pdrForwardVel_mps,
             isfinite(_pdrLastHeadingDeg) ? _pdrLastHeadingDeg : NAN);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false);
  }

  void drawDebugCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Debug / NMEA", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 6;
    int y = baseY;
    char ln[96];

    snprintf(ln, sizeof(ln), "Bytes in : %lu", (unsigned long)_bytes_in);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "NMEA ok : %lu", (unsigned long)_valid_nmea);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "GGA sats: %d  HDOP: %.1f", _gga_sats, _gga_hdop);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "Alt (GGA): %.1f m", _gga_alt_m);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    uint32_t now = millis();
    snprintf(ln, sizeof(ln), "Age last fix: %lu ms",
             (unsigned long)(now - _last_fix_ms));
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false); y += 12;

    snprintf(ln, sizeof(ln), "GSV sats in view: %d", _gsv_sats_view_total);
    drawText(g, Rrx,Rry, card.x+pad, y, ln, 1, false);
  }

  // === NMEA helpers (static) ===
  static uint8_t hex2nibble(char c){
    if (c>='0' && c<='9') return (uint8_t)(c-'0');
    if (c>='A' && c<='F') return (uint8_t)(c-'A'+10);
    if (c>='a' && c<='f') return (uint8_t)(c-'a'+10);
    return 0xFF;
  }

  static bool nmea_checksum_ok(const char* s){
    if (!s || s[0] != '$') return false;
    const char* star = strchr(s, '*');
    if (!star || star - s < 1) return false;
    uint8_t sum = 0;
    for (const char* p = s+1; p < star; ++p) sum ^= (uint8_t)(*p);
    if (!isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) return false;
    uint8_t chk = (hex2nibble(star[1]) << 4) | hex2nibble(star[2]);
    return sum == chk;
  }

  static bool nmea_is_type(const char* s, const char* suffix3){
    if (!s || s[0] != '$') return false;
    const char* comma = strchr(s, ','); if (!comma) return false;
    size_t len = (size_t)(comma - (s+1)); if (len == 0 || len > 6) len = 6;
    char type[8]; memcpy(type, s+1, len); type[len] = '\0';
    size_t L = strlen(type); size_t S = strlen(suffix3);
    if (L < S) return false;
    return (0 == strcmp(type + (L - S), suffix3));
  }

  static bool nmea_dm_to_deg(const char* dm, double& out_deg){
    if (!dm || !*dm) { out_deg = 0; return false; }
    double v   = atof(dm);
    double deg = floor(v/100.0), min = v - deg*100.0;
    out_deg = deg + min/60.0;
    return true;
  }

  static int split_csv(char* s, char* fields[], int maxf){
    const char* comma = strchr(s, ','); if (!comma) return 0;
    int n = 0; char* cur = (char*)comma + 1;
    while (n < maxf && cur && *cur){
      char* c = strchr(cur, ',');
      if (c) { *c = '\0'; fields[n++] = cur; cur = c+1; }
      else {
        char* star = strchr(cur, '*');
        if (star) *star = '\0';
        fields[n++] = cur;
        break;
      }
    }
    return n;
  }

  // === NMEA parsers ===
  void parse_rmc(char* s){
    if (!nmea_is_type(s, "RMC")) return;
    char* f[16]; int n = split_csv(s, f, 16);
    if (n < 10) return;
    // --- GNSS time (UTC) dari RMC ---
    // RMC fields:
    //   f[0] = hhmmss[.sss]
    //   f[8] = ddmmyy
    _rmc_time_valid = false;

    if (f[0] && f[8] && strlen(f[0]) >= 6 && strlen(f[8]) >= 6){
      int hh = (f[0][0]-'0')*10 + (f[0][1]-'0');
      int mm = (f[0][2]-'0')*10 + (f[0][3]-'0');
      int ss = (f[0][4]-'0')*10 + (f[0][5]-'0');

      int dd = (f[8][0]-'0')*10 + (f[8][1]-'0');
      int mo = (f[8][2]-'0')*10 + (f[8][3]-'0');
      int yy = (f[8][4]-'0')*10 + (f[8][5]-'0');
      int year = 2000 + yy; // asumsi GNSS modern (20xx)

      if (hh>=0 && hh<=23 && mm>=0 && mm<=59 && ss>=0 && ss<=60 &&
          dd>=1 && dd<=31 && mo>=1 && mo<=12){

        // days_from_civil (H. Hinnant) -> hari sejak 1970-01-01
        auto days_from_civil = [](int y, unsigned m, unsigned d)->int64_t{
          y -= (m <= 2);
          const int era = (y >= 0 ? y : y-399) / 400;
          const unsigned yoe = (unsigned)(y - era * 400);
          const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
          const unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
          return (int64_t)era*146097 + (int64_t)doe - 719468;
        };

        int64_t days  = days_from_civil(year, (unsigned)mo, (unsigned)dd);
        int64_t epoch = days*86400LL + (int64_t)hh*3600 + (int64_t)mm*60 + (int64_t)ss;

        if (epoch >= 0 && epoch <= 0xFFFFFFFFLL){
          _rmc_epoch_utc  = (uint32_t)epoch;
          _rmc_time_valid = true;
        }
      }
    }

    bool   active = (f[1] && f[1][0]=='A');
    double lat=0,lon=0;
    if (f[2]&&f[3]&&f[4]&&f[5]){
      nmea_dm_to_deg(f[2], lat); if (f[3][0]=='S') lat = -lat;
      nmea_dm_to_deg(f[4], lon); if (f[5][0]=='W') lon = -lon;
    }
    _rmc_speed_kn  = (f[6]&&*f[6]) ? atof(f[6]) : 0.0;
    _rmc_track_deg = (f[7]&&*f[7]) ? atof(f[7]) : 0.0;

    if (active){
      _rmc_has_fix = true;
      _rmc_lat_deg = lat;
      _rmc_lon_deg = lon;
      _last_fix_ms = millis();
    } else {
      _rmc_has_fix = false;
    }
  }

  void parse_gga(char* s){
    if (!nmea_is_type(s, "GGA")) return;
    char* f[16]; int n = split_csv(s, f, 16);
    if (n < 9) return;
    _gga_fix_quality = (f[5]&&*f[5]) ? atoi(f[5]) : 0;
    _gga_sats        = (f[6]&&*f[6]) ? atoi(f[6]) : 0;
    _gga_hdop        = (f[7]&&*f[7]) ? atof(f[7]) : 0.0;
    _gga_alt_m       = (f[8]&&*f[8]) ? atof(f[8]) : 0.0;
    if (_gga_fix_quality > 0) _last_fix_ms = millis();
  }

  void parse_gsv(char* s){
    if (!nmea_is_type(s, "GSV")) return;
    char* f[20]; int n = split_csv(s, f, 20);
    if (n < 3) return;
    int satsView = (f[2] && *f[2]) ? atoi(f[2]) : 0;
    if (satsView >= 0){
      _gsv_sats_view_total = satsView;
      _gsv_last_seen_ms    = millis();
    }
  }

  // ===== NMEA command helper (for $PUBX,xx) =====
  void sendNMEACommand(const char* body){
    if (!_gps) return;
    uint8_t cs = 0;
    for (const char* p = body; *p; ++p) cs ^= (uint8_t)(*p);
    char buf[128];
    snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, cs);
    _gps->print(buf);
  }

  // ===== Auto-baud detect (prefers 115200) =====
  bool try_baud(long baud, unsigned timeout_ms=1200){
    if (!_gps) return false;
    _gps->updateBaudRate(baud);
    char           buf[160];
    size_t         n    = 0;
    unsigned       good = 0;
    unsigned long  t0   = millis();
    while (millis() - t0 < timeout_ms){
      while (_gps->available()){
        int c = _gps->read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n'){
          if (n>0){
            buf[n]='\0';
            if (buf[0]=='$' && nmea_checksum_ok(buf)) {
              if (++good >= 3) return true;
            }
          }
          n=0;
        } else {
          if (n+1 < sizeof(buf)) buf[n++] = (char)c;
          else n=0;
        }
      }
      delay(1);
    }
    return false;
  }

  long auto_baud(){
    if (!_gps) return 9600;
    long candidates[] = {GNSSCfg::BAUD, 38400, 9600};
    for (unsigned i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i){
      if (try_baud(candidates[i])){
        return candidates[i];
      }
    }
    _gps->updateBaudRate(9600);
    return 9600;
  }

  // ===== Force 115200 + enable key NMEA on UART1 (u-blox proprietary NMEA) =====
  void configureUbxTo115200AndMessages(){
    if (!_gps) return;
    // Set UART1 to 115200, in/out protocols: UBX+NMEA+RTCM (0007/0007). Port=1 (UART1)
    sendNMEACommand("PUBX,41,1,0007,0007,115200,0");
    delay(250);
    _gps->updateBaudRate(115200);
    delay(150);

    // Ensure key NMEA messages ON on UART1 (rate=1 on UART1, off others)
    sendNMEACommand("PUBX,40,RMC,0,1,0,0,0,0");
    sendNMEACommand("PUBX,40,GGA,0,1,0,0,0,0");
    sendNMEACommand("PUBX,40,GSV,0,1,0,0,0,0");
    sendNMEACommand("PUBX,40,GSA,0,1,0,0,0,0");
    sendNMEACommand("PUBX,40,GNS,0,1,0,0,0,0"); // multi-GNSS fix sentence
    delay(100);
  }
};
