#pragma once
#include "PageBase.h"
#include "pins_and_config.h"
#include <Wire.h>
#include "RP_MPU9250.h"
#include <Preferences.h>
#include <math.h>

// ============================================================================
// PageSensorMPU9250  (scroll + 3D cube dari sensor + tabs kalibrasi + kompas)
//   - Kalibrasi gyro (auto 3 detik, diam di meja)
//   - Kalibrasi accel (manual start/stop, figur-8 / semua sisi)
//   - Kalibrasi mag   (manual start/stop, figur-8 besar)
//   - Heading offset card + tombol "SET NORTH" (disimpan ke Preferences)
// ============================================================================

class PageSensorMPU9250 : public PageBase {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("SENSOR MPU9250");

    // ===== IMU init =====
    Wire.begin(PinsAndConfig::IMU::SDA, PinsAndConfig::IMU::SCL);
    _imuOk = _imu.begin(PinsAndConfig::IMU::I2C_HZ);
    _lastReadMs = 0;

    // ===== Preferences (load cal) =====
    // Param kedua = readOnly (true = hanya baca, false = boleh tulis)
    _prefs.begin("imu_cal", /*readOnly=*/true);
    _loadCalibrationFromPrefs();
    _prefs.end();

    recomputeLayout();
    updateContentHeight();
    if (!_t0Init){ _t0 = millis(); _t0Init = true; }
  }

  void onEnter() override {
    PageBase::onEnter();
    updateContentHeight();
    if (!_t0Init){ _t0 = millis(); _t0Init = true; }
  }

  void onQSClosed() override {
    if (!isActive()) return;
    recomputeLayout();
    updateContentHeight();
    PageBase::onQSClosed();
  }

  // Backend ringan: dipanggil dari loop utama supaya IMU selalu update
  // tanpa perlu membuka page (dan tanpa menjalankan proses kalibrasi).
  void backendTick(uint32_t /*nowMs*/) {
    _updateIMU_backend(false);   // false = jangan jalankan proses kalibrasi
  }

  int contentHeight() const { return _contentH; }

protected:
  // ====== Data Readers ke UI sliders ========================================
  void readGyro (float& x,float& y,float& z){ x=_gx_dps; y=_gy_dps; z=_gz_dps; }
  void readAccel(float& x,float& y,float& z){ x=_ax_g;  y=_ay_g;  z=_az_g;  }
  void readMag  (float& x,float& y,float& z){ x=_mx_uT; y=_my_uT; z=_mz_uT; }

  // ====== Rendering ==========================================================
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    // Update IMU (poll @ ~100 Hz) + heading
    _updateIMU();

    const int RryEff = Rry + _scrollY;

    // Row-1
    drawCard(g, Rrx, RryEff, _cardGyro,  "Gyro",
             _rangeGyro,  readFnGyro(),  Units::Dps);
    drawCard(g, Rrx, RryEff, _cardAccel, "Accel",
             _rangeAccel, readFnAccel(), Units::G);

    // Row-2
    drawCard(g, Rrx, RryEff, _cardMag,   "Mag",
             _rangeMag,   readFnMag(),   Units::uT);

    // Row-3 (3D Cube + Euler)
    drawCubeCard(g, Rrx, RryEff, _cardCube);

    // Row-4 (Tabs Kalibrasi)
    drawTabsCard(g, Rrx, RryEff, _cardTabs);

    // Row-5 (Heading offset)
    drawHeadingOffsetCard(g, Rrx, RryEff, _cardHeading);

    // Row-6 (Kompas)
    drawCompassCard(g, Rrx, RryEff, _cardCompass);
  }

  // ====== Input: scroll + tap tabs/button ===================================
  void handleContentInput(bool pressed, int x, int y) override {
    const int cy = y + _scrollY;   // content coords
    int  hitTab       = -1;
    const bool onTabsBar    = tabsHitTestContent(x, cy, hitTab);
    const bool onBtnCalib   = btnHitTestContent(x, cy);
    const bool onHeadingBtn = headingBtnHitTestContent(x, cy);

    if (pressed) {
      // Prioritas: tombol kalibrasi → heading offset → tab bar → scroll
      if (onBtnCalib) {
        _handleCalibButtonPress(); // toggle start/stop sesuai tab
        return;
      }
      if (onHeadingBtn){
        _onHeadingOffsetButtonPress();
        return;
      }
      if (onTabsBar) {
        if (hitTab >= 0 && hitTab <= 2 && _activeTab != hitTab) {
          _activeTab = hitTab;
        }
        return;
      }
      // scroll
      if (!_dragging) {
        _dragging   = true;
        _dragY0     = y;
        _scrollY0   = _scrollY;
      } else {
        const int dy   = y - _dragY0;
        int newScroll  = _scrollY0 - dy;
        if (newScroll < 0) newScroll = 0;
        if (newScroll > _scrollMax) newScroll = _scrollMax;
        _scrollY = newScroll;
      }
    } else {
      _dragging = false;
    }
  }

private:
  // ====== Types & helpers ====================================================
  struct Rect { int x=0,y=0,w=0,h=0; };
  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }
  static inline uint8_t clamp8(int v){ if(v<0) return 0; if(v>255) return 255; return (uint8_t)v; }
  static inline float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }

  enum class Units { Dps, G, uT };

  // Theme
  using Theme = PinsAndConfig::Theme;
  static constexpr int     RADIUS = 12;
  static constexpr uint8_t CR = Theme::UI_R, CG = Theme::UI_G, CB = Theme::UI_B;

  // Grid & spacing
  static constexpr int EDGE       = Theme::EDGE_BUF;
  static constexpr int GAPX       = 10;
  static constexpr int GAPY12     = 4;
  static constexpr int GAPY23     = 8;
  static constexpr int GAPY34     = 8;
  static constexpr int GAPY45     = 8;  // Tabs -> Heading
  static constexpr int GAPY56     = 8;  // Heading -> Compass
  static constexpr int TITLE2CARD = 10;
  static constexpr int BOT_PAD    = 24;

  // Card geometry
  int  _cardW = 0;
  int  _cardH = 120;
  int  _cubeH = 280;
  int  _tabsH = 200;
  int  _headingH = 120;
  int  _compassH = 280;

  Rect _cardGyro, _cardAccel, _cardMag, _cardCube, _cardTabs, _cardHeading, _cardCompass;

  // ====== IMU ======
  RP_MPU9250 _imu { Wire, PinsAndConfig::IMU::MPU_ADDR, PinsAndConfig::IMU::AK_ADDR };
  bool       _imuOk = false;
  uint32_t   _lastReadMs = 0;

  // Skala asumsi default
  static constexpr float SCALE_ACC_G    = 16384.0f; // LSB per g (±2g)
  static constexpr float SCALE_GYRO_DPS = 131.0f;   // LSB per dps (±250 dps)

  // Data mapped ke frame perangkat (setelah mapping + kalibrasi)
  float _gx_dps=0, _gy_dps=0, _gz_dps=0;
  float _ax_g=0,  _ay_g=0,  _az_g=0;
  float _mx_uT=0, _my_uT=0, _mz_uT=0;

  // Slider full-scale
  float _rangeGyro  = 250.0f; // ±250 dps
  float _rangeAccel = 2.0f;   // ±2 g
  float _rangeMag   = 120.0f; // ±120 uT

  // Euler (radian) tilt-comp’ed
  float _roll=0, _pitch=0, _yaw=0;      // LPF untuk cube
  float _yaw360=0;                      // 0..2π untuk kompas & UI

  // Heading mentah + offset (radian)
  float _headingRaw       = 0.0f;       // 0..2π, belum di-offset
  float _headingOffsetRad = 0.0f;       // 0..2π, disimpan di Preferences

  // ====== Calibration store (Preferences) ===================================
  Preferences _prefs;
  // Gyro bias (dps)
  float _gBiasX=0, _gBiasY=0, _gBiasZ=0; bool _hasGyro=false;
  // Accel bias & scale (unit g)
  float _aBiasX=0,_aBiasY=0,_aBiasZ=0; float _aScaleX=1,_aScaleY=1,_aScaleZ=1; bool _hasAccel=false;
  // Mag bias & scale (uT)
  float _mBiasX=0,_mBiasY=0,_mBiasZ=0; float _mScaleX=1,_mScaleY=1,_mScaleZ=1; bool _hasMag=false;

  void _loadCalibrationFromPrefs(){
    // Di sini _prefs SUDAH di-begin() oleh begin(), mode readOnly=true
    auto getF = [&](const char* k, float defv)->float{
      return _prefs.isKey(k) ? _prefs.getFloat(k, defv) : defv;
    };

    // Flag "ok" disimpan sebagai int 0/1
    _hasGyro  = (_prefs.isKey("g_ok") && _prefs.getInt("g_ok", 0) == 1);
    _hasAccel = (_prefs.isKey("a_ok") && _prefs.getInt("a_ok", 0) == 1);
    _hasMag   = (_prefs.isKey("m_ok") && _prefs.getInt("m_ok", 0) == 1);

    if (_hasGyro){
      _gBiasX = getF("g_bx",0); _gBiasY = getF("g_by",0); _gBiasZ = getF("g_bz",0);
    }
    if (_hasAccel){
      _aBiasX = getF("a_bx",0); _aBiasY = getF("a_by",0); _aBiasZ = getF("a_bz",0);
      _aScaleX= getF("a_sx",1); _aScaleY= getF("a_sy",1); _aScaleZ= getF("a_sz",1);
    }
    if (_hasMag){
      _mBiasX = getF("m_bx",0); _mBiasY = getF("m_by",0); _mBiasZ = getF("m_bz",0);
      _mScaleX= getF("m_sx",1); _mScaleY= getF("m_sy",1); _mScaleZ= getF("m_sz",1);
    }

    // Heading offset (opsional). Default 0 = tidak ada offset.
    _headingOffsetRad = getF("h_off", 0.0f);
    // jaga tetap di 0..2π
    _headingOffsetRad = wrap01(_headingOffsetRad);
  }

  void _saveCalibrationToPrefs(){
    // Tulis ke NVS (readOnly = false)
    _prefs.begin("imu_cal", /*readOnly=*/false);

    if (_hasGyro){
      _prefs.putFloat("g_bx", _gBiasX);
      _prefs.putFloat("g_by", _gBiasY);
      _prefs.putFloat("g_bz", _gBiasZ);
      _prefs.putInt("g_ok", 1);
    }

    if (_hasAccel){
      _prefs.putFloat("a_bx", _aBiasX);
      _prefs.putFloat("a_by", _aBiasY);
      _prefs.putFloat("a_bz", _aBiasZ);
      _prefs.putFloat("a_sx", _aScaleX);
      _prefs.putFloat("a_sy", _aScaleY);
      _prefs.putFloat("a_sz", _aScaleZ);
      _prefs.putInt("a_ok", 1);
    }

    if (_hasMag){
      _prefs.putFloat("m_bx", _mBiasX);
      _prefs.putFloat("m_by", _mBiasY);
      _prefs.putFloat("m_bz", _mBiasZ);
      _prefs.putFloat("m_sx", _mScaleX);
      _prefs.putFloat("m_sy", _mScaleY);
      _prefs.putFloat("m_sz", _mScaleZ);
      _prefs.putInt("m_ok", 1);
    }

    // Heading offset disimpan selalu (boleh 0)
    _prefs.putFloat("h_off", _headingOffsetRad);

    _prefs.end();
  }

  // ====== Scroll state =======================================================
  int  _contentH = 0;
  int  _scrollY  = 0;
  int  _scrollMax= 0;
  bool _dragging = false;
  int  _dragY0   = 0;
  int  _scrollY0 = 0;

  // ====== Tabs state & calib =================================================
  int   _activeTab = 0; // 0=Gyro,1=Accel,2=Mag
  Rect  _btnRect{0,0,0,0};        // calibrate button rect
  Rect  _headingBtnRect{0,0,0,0}; // heading offset button rect

  enum CalibMode { Idle=0, GyroRun, AccelRun, MagRun } _calMode = Idle;
  uint32_t _calStartMs=0;

  // Gyro accum
  double _gSumX=0,_gSumY=0,_gSumZ=0; uint32_t _gSamples=0;

  // Accel min/max
  float _aMinX= 1e9f,_aMaxX=-1e9f,_aMinY= 1e9f,_aMaxY=-1e9f,_aMinZ= 1e9f,_aMaxZ=-1e9f;

  // Mag min/max
  float _mMinX= 1e9f,_mMaxX=-1e9f,_mMinY= 1e9f,_mMaxY=-1e9f,_mMinZ= 1e9f,_mMaxZ=-1e9f;

  // ====== Reader functors ====================================================
  struct Reader {
    PageSensorMPU9250* self;
    explicit Reader(PageSensorMPU9250* s=nullptr): self(s) {}
    void operator()(float& x,float& y,float& z){ x=y=z=0; }
  };
  struct GyroReader  : Reader { explicit GyroReader (PageSensorMPU9250* s=nullptr): Reader(s) {}
    void operator()(float& x,float& y,float& z){ self->readGyro(x,y,z); } };
  struct AccelReader : Reader { explicit AccelReader(PageSensorMPU9250* s=nullptr): Reader(s) {}
    void operator()(float& x,float& y,float& z){ self->readAccel(x,y,z); } };
  struct MagReader   : Reader { explicit MagReader  (PageSensorMPU9250* s=nullptr): Reader(s) {}
    void operator()(float& x,float& y,float& z){ self->readMag(x,y,z); } };

  GyroReader  readFnGyro () { return GyroReader (this); }
  AccelReader readFnAccel() { return AccelReader(this); }
  MagReader   readFnMag  () { return MagReader  (this); }

  // ====== Layout =============================================================
  void recomputeLayout(){
    const int cardsTop = _viewportY0 + TITLE2CARD;

    // Row-1
    _cardW = (TFT_WIDTH - 2*EDGE - GAPX) / 2;
    if (_cardW < 40) _cardW = 40;
    _cardGyro  = { EDGE,                 cardsTop, _cardW, _cardH };
    _cardAccel = { EDGE + _cardW + GAPX, cardsTop, _cardW, _cardH };

    // Row-2
    const int row2Top = cardsTop + _cardH + GAPY12;
    const int magX    = (TFT_WIDTH - _cardW) / 2;
    _cardMag = { magX, row2Top, _cardW, _cardH };

    // Row-3 (Cube)
    const int row3Top = row2Top + _cardH + GAPY23;
    const int cubeW   = TFT_WIDTH - 2*EDGE;
    _cardCube = { EDGE, row3Top, cubeW, _cubeH };

    // Row-4 (Tabs kalibrasi)
    const int row4Top = row3Top + _cubeH + GAPY34;
    const int tabsW   = TFT_WIDTH - 2*EDGE;
    _cardTabs = { EDGE, row4Top, tabsW, _tabsH };

    // Row-5 (Heading offset)
    const int row5Top   = row4Top + _tabsH + GAPY45;
    const int headW     = TFT_WIDTH - 2*EDGE;
    _cardHeading = { EDGE, row5Top, headW, _headingH };

    // Row-6 (Kompas)
    const int row6Top   = row5Top + _headingH + GAPY56;
    const int compW     = TFT_WIDTH - 2*EDGE;
    _cardCompass = { EDGE, row6Top, compW, _compassH };
  }

  void updateContentHeight(){
    const int bottom = _cardCompass.y + _cardCompass.h;
    _contentH = bottom + BOT_PAD;

    const int vpH = _viewportH;
    int maxY = _contentH - vpH;
    if (maxY < 0) maxY = 0;
    _scrollMax = maxY;
    if (_scrollY > _scrollMax) _scrollY = _scrollMax;
  }

  // ====== Drawing utils ======================================================
  void fillRoundedClipped(GfxRGB888& g, int Rrx, int Rry,
                          const Rect& r, int rad,
                          uint8_t rr,uint8_t gg,uint8_t bb){
    if (r.w<=0 || r.h<=0) return;
    const int x0=r.x, y0=r.y, w=r.w, h=r.h;

    const int drawTop = imax(y0, Rry);
    const int drawBot = imin(y0+h, Rry+g.h);
    if (drawBot <= drawTop) return;

    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yLoc = gy - Rry;
      const int yy = gy - y0;
      int inset=0;
      if (rad>0){
        if (yy < rad){
          float dy = float(rad - 1 - yy);
          float dx = sqrtf(float(rad)*rad - dy*dy);
          inset = int(rad - floorf(dx));
        }
        int y2 = (h - 1 - yy);
        if (y2 < rad){
          float dy = float(rad - 1 - y2);
          float dx = sqrtf(float(rad)*rad - dy*dy);
          int inset2 = int(rad - floorf(dx));
          if (inset2 > inset) inset = inset2;
        }
      }
      int gx0 = imax(x0 + inset, Rrx);
      int gx1 = imin(x0 + w - inset, Rrx + g.w);
      if (gx1 <= gx0) continue;

      uint8_t* row = g.pix + ((size_t)yLoc*g.w + (gx0 - Rrx))*3;
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

  static void fmtAxis(char* out, size_t n, char axis, float v, Units u){
    switch(u){
      case Units::Dps: snprintf(out, n, "%c = %0.2f dps", axis, v); break;
      case Units::G:   snprintf(out, n, "%c = %0.3f g",   axis, v); break;
      case Units::uT:  snprintf(out, n, "%c = %0.1f uT",  axis, v); break;
    }
  }

  template <typename ReaderFn>
  void drawCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card, const char* title,
                float fullScale, ReaderFn reader, Units u){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);

    const int pad     = 10;
    const int scaleT  = 2;
    const int glyphH2 = 7*scaleT;
    drawText(g, Rrx,Rry, card.x + pad, card.y + pad, title, scaleT, true);

    float x=0,y=0,z=0;
    reader(x,y,z);

    const int scaleLbl = 1;
    const int glyphH1  = 7*scaleLbl;
    const int labelGap = 2;
    const int axisGap  = 8;
    const int trackH   = 10;
    const int left     = card.x + pad;
    const int barW     = card.w - 2*pad;

    int yCursor = card.y + pad + glyphH2 + 8;

    { char buf[32]; fmtAxis(buf, sizeof(buf), 'X', x, u);
      drawText(g, Rrx,Rry, left, yCursor, buf, scaleLbl, false);
      yCursor += glyphH1 + labelGap;
      Rect s{ left, yCursor, barW, trackH };
      drawAxisSlider(g, Rrx,Rry, s, x, fullScale);
      yCursor += trackH + axisGap; }

    { char buf[32]; fmtAxis(buf, sizeof(buf), 'Y', y, u);
      drawText(g, Rrx,Rry, left, yCursor, buf, scaleLbl, false);
      yCursor += glyphH1 + labelGap;
      Rect s{ left, yCursor, barW, trackH };
      drawAxisSlider(g, Rrx,Rry, s, y, fullScale);
      yCursor += trackH + axisGap; }

    { char buf[32]; fmtAxis(buf, sizeof(buf), 'Z', z, u);
      drawText(g, Rrx,Rry, left, yCursor, buf, scaleLbl, false);
      yCursor += glyphH1 + labelGap;
      Rect s{ left, yCursor, barW, trackH };
      drawAxisSlider(g, Rrx,Rry, s, z, fullScale);
    }
  }

  void drawAxisSlider(GfxRGB888& g, int Rrx,int Rry, const Rect& s,
                      float value, float fullScaleAbs){
    if (s.w<=0 || s.h<=0) return;

    const int drawTop = imax(s.y, Rry);
    const int drawBot = imin(s.y + s.h, Rry + g.h);
    if (drawBot <= drawTop) return;

    const int cx = s.x + s.w/2;
    float norm = 0.0f;
    if (fullScaleAbs > 0.0f) norm = value / fullScaleAbs;
    norm = clampf(norm, -1.0f, 1.0f);

    const int fillW = (int)floorf( (s.w/2.0f) * fabsf(norm) + 0.5f );
    const bool pos = (norm >= 0.0f);

    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yLoc = gy - Rry;

      int x0 = imax(s.x, Rrx);
      int x1 = imin(s.x + s.w, Rrx + g.w);
      if (x1 > x0){
        uint8_t* row = g.pix + ((size_t)yLoc*g.w + (x0 - Rrx))*3;
        for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
      }

      int ml0 = imax(cx-1, Rrx);
      int ml1 = imin(cx+1, Rrx + g.w);
      if (ml1 > ml0){
        uint8_t* row = g.pix + ((size_t)yLoc*g.w + (ml0 - Rrx))*3;
        for (int gx=ml0; gx<ml1; ++gx){ row[0]=120; row[1]=120; row[2]=120; row+=3; }
      }

      if (fillW > 0){
        int fx0, fx1;
        if (pos){ fx0 = cx;                    fx1 = imin(cx + fillW, s.x + s.w); }
        else    { fx0 = imax(cx - fillW, s.x); fx1 = cx; }
        fx0 = imax(fx0, Rrx);
        fx1 = imin(fx1, Rrx + g.w);
        if (fx1 > fx0){
          uint8_t* row = g.pix + ((size_t)yLoc*g.w + (fx0 - Rrx))*3;
          for (int gx=fx0; gx<fx1; ++gx){ row[0]=255; row[1]=255; row[2]=255; row+=3; }
        }
      }
    }
  }

  // ====== 3D Cube data & renderer ===========================================
  static constexpr Vec3 V[8] = {
    {-1,-1,-1}, {+1,-1,-1}, {+1,+1,-1}, {-1,+1,-1},
    {-1,-1,+1}, {+1,-1,+1}, {+1,+1,+1}, {-1,+1,+1}
  };
  static constexpr QuadFace F[6] = {
    {4,5,6,7,{255, 80,  0},0}, // FRONT  (z+)
    {0,1,2,3,{ 50,235, 20},1}, // BACK   (z-)
    {1,2,6,5,{  0, 10,255},2}, // RIGHT  (x+)
    {0,3,7,4,{  0, 80,225},3}, // LEFT   (x-)
    {3,2,6,7,{  0,180,220},4}, // TOP    (y+)
    {0,1,5,4,{235,235,240},5}, // BOTTOM (y-)
  };

  OrthoProjector _prj{};
  bool     _t0Init=false;
  uint32_t _t0=0;

  const float _modelScale = 0.68f;  // ubah di sini jika perlu

  void drawCubeCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);

    const int pad    = 10;
    const int scaleT = 2;
    drawText(g, Rrx,Rry, card.x + pad, card.y + pad, "3D Cube", scaleT, true);

    // Area cube + Euler
    const int titleH = 7*scaleT;
    const int top    = card.y + pad + titleH + 8;
    const int left   = card.x + pad;
    const int w      = card.w - 2*pad;

    const int eulerLines = 3;
    const int eulerH     = eulerLines * 7 + 8;
    const int hCube      = card.h - (top - card.y) - eulerH - pad;
    if (w<=0 || hCube<=0) return;

    // Clear box cube
    {
      const int drawTop = imax(top, Rry);
      const int drawBot = imin(top + hCube, Rry + g.h);
      for (int gy=drawTop; gy<drawBot; ++gy){
        int x0 = imax(left, Rrx);
        int x1 = imin(left + w, Rrx + g.w);
        if (x1 <= x0) continue;
        uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
        for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
      }
    }

    _prj.cx    = left + w/2;
    _prj.cy    = top  + hCube/2;
    _prj.scale = (float)imin(w,hCube) * 0.42f;

    // Rotasi dari sensor (LPF’d)
    Mat3 Rm = Mat3::rotXYZ(_roll, _pitch, _yaw);

    FaceRenderer3D::drawFacesOrtho(
      g, Rrx, Rry, g.w, g.h,
      _prj,
      V, (int)(sizeof(V)/sizeof(V[0])),
      F, (int)(sizeof(F)/sizeof(F[0])),
      Rm,
      _modelScale
    );

    // EULER (deg)
    const float RAD2DEG = 57.2957795f;
    char line[48];
    int tx = left;
    int ty = top + hCube + 8;

    snprintf(line, sizeof(line), "Roll  : %0.2f deg",  _roll  * RAD2DEG);
    drawText(g, Rrx, Rry, tx, ty, line, 1, true);  ty += 7;
    snprintf(line, sizeof(line), "Pitch : %0.2f deg",  _pitch * RAD2DEG);
    drawText(g, Rrx, Rry, tx, ty, line, 1, true);  ty += 7;
    snprintf(line, sizeof(line), "Yaw   : %0.2f deg",  _yaw360 * RAD2DEG);
    drawText(g, Rrx, Rry, tx, ty, line, 1, true);
  }

  // ====== Tabs Kalibrasi =====================================================
  static constexpr int TAB_BAR_H   = 34;
  static constexpr int TAB_PAD     = 10;
  static constexpr int TAB_RADIUS  = 8;

  // Wrap numbered steps (konten dibatasi kiri-kanan, nomor tersendiri)
  void drawNumberedSteps(GfxRGB888& g,int Rrx,int Rry,int x,int y,int w,
                         const char* const* lines, int nLines, int scale=1){
    const int cw = 6 * scale;        // approx char width
    const int ch = 7 * scale;
    const int numW = 3 * cw;         // ruang "n. "
    const int textW = w - numW;
    int yy = y;

    for (int i=0;i<nLines;++i){
      // gambar nomor
      char num[8]; snprintf(num,sizeof(num), "%d.", i+1);
      drawText(g,Rrx,Rry, x, yy, num, scale, true);

      // wrap isi
      const char* s = lines[i];
      int sx = x + numW;
      int maxChars = textW / cw; if (maxChars<1) maxChars=1;

      while (*s){
        // ambil segmen <= maxChars, pecah di spasi terakhir jika ada
        int len=0, lastSpace=-1;
        while (s[len] && len<maxChars){ if (s[len]==' ') lastSpace=len; ++len; }
        int cut = (len==maxChars && lastSpace>0)? lastSpace : len;

        char buf[128];
        if (cut >= (int)sizeof(buf)) cut = sizeof(buf)-1;
        memcpy(buf, s, cut); buf[cut]=0;
        drawText(g,Rrx,Rry, sx, yy, buf, scale, false);
        yy += ch;

        s += cut;
        while (*s==' ') ++s; // skip space
      }
      yy += 2; // gap antar poin
    }
  }

  void drawTabsCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);

    const int barLeft = card.x + TAB_PAD;
    const int barTop  = card.y + TAB_PAD;
    const int barW    = card.w - 2*TAB_PAD;
    const int barH    = TAB_BAR_H;
    const int eachW   = barW / 3;

    // Tabs
    for (int i=0;i<3;++i){
      Rect tRect{ barLeft + i*eachW, barTop, (i==2)? (barW - 2*eachW) : eachW, barH };
      const bool active = (i == _activeTab);
      const int  k = active ? +22 : -12;
      uint8_t r = clamp8(CR + k), gcol = clamp8(CG + k), b = clamp8(CB + k);
      fillRoundedClipped(g, Rrx,Rry, tRect, TAB_RADIUS, r, gcol, b);

      const char* name = (i==0) ? "Gyro" : (i==1) ? "Accel" : "Mag";
      int tx = tRect.x + 10;
      int ty = tRect.y + (tRect.h - 7*2)/2 - 1;
      drawText(g, Rrx,Rry, tx, ty, name, 2, active);
    }

    // Content area
    const int contLeft = card.x + TAB_PAD;
    const int contTop  = barTop + barH + 8;
    const int contW    = card.w - 2*TAB_PAD;
    const int contH    = card.h - (contTop - card.y) - TAB_PAD;

    // Clear konten
    {
      const int drawTop = imax(contTop, Rry);
      const int drawBot = imin(contTop + contH, Rry + g.h);
      for (int gy=drawTop; gy<drawBot; ++gy){
        int x0 = imax(contLeft, Rrx);
        int x1 = imin(contLeft + contW, Rrx + g.w);
        if (x1 <= x0) continue;
        uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
        for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
      }
    }

    // Steps & status
    const int sx = contLeft + 6;
    int sy = contTop + 2;
    const int sw = contW - 12;

    const char* gyroSteps[] = {
      "Letakkan perangkat diam di meja.",
      "Tekan Calibrate, tunggu 3 detik.",
      "Jangan digerakkan sampai selesai.",
      "Bias gyro akan disimpan otomatis."
    };
    const char* accelSteps[] = {
      "Putar perangkat ke banyak orientasi (figur-8, semua sisi).",
      "Pastikan tiap sumbu mencapai nilai minimum dan maksimum.",
      "Tekan Calibrate untuk MULAI/SELESAI.",
      "Offset & skala akan disimpan otomatis (1g normalization)."
    };
    const char* magSteps[] = {
      "Putar perangkat 360 derajat semua arah (figur-8 besar).",
      "Jauhkan dari logam/arus besar saat kalibrasi.",
      "Tekan Calibrate untuk MULAI/SELESAI.",
      "Hard-iron bias & soft-iron scale akan disimpan."
    };

    switch (_activeTab){
      case 0:{ // Gyro
        if (_hasGyro){
          drawText(g, Rrx,Rry, sx, sy, "Gyro: CALIBRATED", 1, true); sy += 9;
          char buf[64];
          snprintf(buf, sizeof(buf),
                   "Bias [dps]: X=%0.2f Y=%0.2f Z=%0.2f",
                   _gBiasX, _gBiasY, _gBiasZ);
          drawText(g, Rrx,Rry, sx, sy, buf, 1, false); sy += 9;
        } else {
          drawText(g, Rrx,Rry, sx, sy, "Gyro: Not calibrated", 1, true); sy += 9;
        }
        drawNumberedSteps(g, Rrx,Rry, sx, sy, sw,
                          gyroSteps,
                          (int)(sizeof(gyroSteps)/sizeof(gyroSteps[0])),
                          1);
      } break;

      case 1:{ // Accel
        if (_hasAccel){
          drawText(g, Rrx,Rry, sx, sy, "Accel: CALIBRATED", 1, true); sy += 9;
          char buf[64];
          snprintf(buf, sizeof(buf),
                   "Bias [g]: X=%0.3f Y=%0.3f Z=%0.3f",
                   _aBiasX, _aBiasY, _aBiasZ);
          drawText(g, Rrx,Rry, sx, sy, buf, 1, false); sy += 9;
          snprintf(buf, sizeof(buf),
                   "Scale: X=%0.3f Y=%0.3f Z=%0.3f",
                   _aScaleX, _aScaleY, _aScaleZ);
          drawText(g, Rrx,Rry, sx, sy, buf, 1, false); sy += 9;
        } else {
          drawText(g, Rrx,Rry, sx, sy, "Accel: Not calibrated", 1, true); sy += 9;
        }
        drawNumberedSteps(g, Rrx,Rry, sx, sy, sw,
                          accelSteps,
                          (int)(sizeof(accelSteps)/sizeof(accelSteps[0])),
                          1);
      } break;

      case 2:{ // Mag
        if (_hasMag){
          drawText(g, Rrx,Rry, sx, sy, "Mag: CALIBRATED", 1, true); sy += 9;
          char buf[64];
          snprintf(buf, sizeof(buf),
                   "Bias [uT]: X=%0.1f Y=%0.1f Z=%0.1f",
                   _mBiasX, _mBiasY, _mBiasZ);
          drawText(g, Rrx,Rry, sx, sy, buf, 1, false); sy += 9;
          snprintf(buf, sizeof(buf),
                   "Scale: X=%0.3f Y=%0.3f Z=%0.3f",
                   _mScaleX, _mScaleY, _mScaleZ);
          drawText(g, Rrx,Rry, sx, sy, buf, 1, false); sy += 9;
        } else {
          drawText(g, Rrx,Rry, sx, sy, "Mag: Not calibrated", 1, true); sy += 9;
        }
        drawNumberedSteps(g, Rrx,Rry, sx, sy, sw,
                          magSteps,
                          (int)(sizeof(magSteps)/sizeof(magSteps[0])),
                          1);
      } break;
    }

    // Calibrate button (bottom area)
    const int btnW = 120;
    const int btnH = 30;
    const int btnX = contLeft + contW - btnW;
    const int btnY = contTop + contH - btnH;

    const bool runningThis =
      (_activeTab==0 && _calMode==GyroRun) ||
      (_activeTab==1 && _calMode==AccelRun) ||
      (_activeTab==2 && _calMode==MagRun);

    uint8_t br=90,bg=90,bb=95; // idle grey
    const char* btnTxt = "Calibrate";
    if (runningThis){ br=200; bg=40; bb=40; btnTxt="Calibrating…"; }

    Rect btn{ btnX, btnY, btnW, btnH };
    _btnRect = btn; // store for hit-test

    fillRoundedClipped(g, Rrx,Rry, btn, 10, br,bg,bb);
    int tx = btnX + 10;
    int ty = btnY + (btnH - 7*2)/2 - 1;
    drawText(g, Rrx,Rry, tx, ty, btnTxt, 2, true);
  }

  // ====== Heading Offset Card ================================================
  void drawHeadingOffsetCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y + card.h) <= Rry) return;

    fillRoundedClipped(g, Rrx, Rry, card, RADIUS, CR, CG, CB);

    const int pad    = 10;
    const int scaleT = 2;
    const int glyphH = 7 * scaleT;

    // Title
    drawText(g, Rrx, Rry, card.x + pad, card.y + pad, "Heading offset", scaleT, true);

    const float RAD2DEG = 57.2957795f;

    // Raw heading (tanpa offset, 0..360)
    float rawDeg = _headingRaw * RAD2DEG;
    while (rawDeg < 0.0f)   rawDeg += 360.0f;
    while (rawDeg >= 360.0f) rawDeg -= 360.0f;

    // Offset (0..360 untuk tampilan)
    float offDeg = _headingOffsetRad * RAD2DEG;
    while (offDeg < 0.0f)   offDeg += 360.0f;
    while (offDeg >= 360.0f) offDeg -= 360.0f;

    // Heading akhir (LPF, sudah pakai offset)
    float headDeg = _yaw360 * RAD2DEG;
    while (headDeg < 0.0f)   headDeg += 360.0f;
    while (headDeg >= 360.0f) headDeg -= 360.0f;

    int x = card.x + pad;
    int y = card.y + pad + glyphH + 6;

    char line[64];
    snprintf(line, sizeof(line), "Raw   : %6.1f deg", rawDeg);
    drawText(g, Rrx, Rry, x, y, line, 1, false); y += 9;

    snprintf(line, sizeof(line), "Offset: %6.1f deg", offDeg);
    drawText(g, Rrx, Rry, x, y, line, 1, false); y += 9;

    snprintf(line, sizeof(line), "Final : %6.1f deg", headDeg);
    drawText(g, Rrx, Rry, x, y, line, 1, true); y += 14;

    // Tombol "SET NORTH"
    const int btnW = 150;
    const int btnH = 30;
    const int btnX = card.x + card.w - btnW - pad;
    const int btnY = card.y + card.h - btnH - pad;

    _headingBtnRect = { btnX, btnY, btnW, btnH };

    uint8_t br = 90, bg = 90, bb = 95;
    Rect btn{ btnX, btnY, btnW, btnH };
    fillRoundedClipped(g, Rrx, Rry, btn, 10, br, bg, bb);

    int tx = btnX + 8;
    int ty = btnY + (btnH - 7*2)/2 - 1;
    drawText(g, Rrx, Rry, tx, ty, "SET NORTH", 2, true);
  }

  // ====== Hit-test helpers ===================================================
  bool tabsHitTestContent(int xScreen, int yContent, int& outIdx) const {
    outIdx = -1;
    const int barLeft = _cardTabs.x + TAB_PAD;
    const int barTop  = _cardTabs.y + TAB_PAD;
    const int barW    = _cardTabs.w - 2*TAB_PAD;
    const int barH    = TAB_BAR_H;

    if (yContent < barTop || yContent >= barTop + barH) return false;
    if (xScreen  < barLeft || xScreen  >= barLeft + barW) return false;

    const int eachW = barW / 3;
    int idx = (xScreen - barLeft) / eachW;
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    outIdx = idx;
    return true;
  }

  bool btnHitTestContent(int xScreen, int yContent) const {
    return (yContent >= _btnRect.y && yContent < _btnRect.y + _btnRect.h &&
            xScreen  >= _btnRect.x && xScreen  < _btnRect.x + _btnRect.w);
  }

  bool headingBtnHitTestContent(int xScreen, int yContent) const {
    return (yContent >= _headingBtnRect.y && yContent < _headingBtnRect.y + _headingBtnRect.h &&
            xScreen  >= _headingBtnRect.x && xScreen  < _headingBtnRect.x + _headingBtnRect.w);
  }

  // ====== Actions ============================================================
  void _handleCalibButtonPress(){
    const uint32_t now = millis();
    if (_calMode == Idle){
      // Start sesuai tab
      if (_activeTab==0){ // Gyro
        _calMode = GyroRun;
        _calStartMs = now;
        _gSumX=_gSumY=_gSumZ=0; _gSamples=0;
      } else if (_activeTab==1){ // Accel
        _calMode = AccelRun;
        _calStartMs = now;
        _aMinX=_aMinY=_aMinZ= 1e9f; _aMaxX=_aMaxY=_aMaxZ=-1e9f;
      } else { // Mag
        _calMode = MagRun;
        _calStartMs = now;
        _mMinX=_mMinY=_mMinZ= 1e9f; _mMaxX=_mMaxY=_mMaxZ=-1e9f;
      }
    } else {
      // Stop & finalize bila tab cocok, jika beda tab abaikan (sedang sibuk)
      if (_activeTab==0 && _calMode==GyroRun){
        _finalizeGyroCal();
        _calMode = Idle;
      } else if (_activeTab==1 && _calMode==AccelRun){
        _finalizeAccelCal();
        _calMode = Idle;
      } else if (_activeTab==2 && _calMode==MagRun){
        _finalizeMagCal();
        _calMode = Idle;
      }
    }
  }

  void _onHeadingOffsetButtonPress(){
    if (!_imuOk) return;
    // Set offset supaya headingView = 0 pada orientasi sekarang
    _headingOffsetRad = wrap01(-_headingRaw);
    _saveCalibrationToPrefs();
  }

  // ====== Kompas Card (kompas baru, lebih besar) ============================
  void drawCompassCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);

    const int pad = 10;
    const int cx  = card.x + card.w/2;
    const int cy  = card.y + card.h/2 + 8;
    // card lebih tinggi → radius bisa dimaksimalkan
    const int R   = imin(card.w, card.h) / 2 - pad - 2;
    if (R <= 10) return;

    // Bersihkan area dalam card
    const int clearTop = cy - R - 20;
    const int clearBot = cy + R + 20;
    const int drawTop  = imax(clearTop, Rry);
    const int drawBot  = imin(clearBot, Rry + g.h);
    for (int gy=drawTop; gy<drawBot; ++gy){
      int x0 = imax(card.x+pad,        Rrx);
      int x1 = imin(card.x+card.w-pad, Rrx+g.w);
      if (x1<=x0) continue;
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
    }

    // Helper pixel & garis
    auto putPix = [&](int px,int py,uint8_t r,uint8_t gg,uint8_t bb){
      if (px>=Rrx && px<Rrx+g.w && py>=Rry && py<Rry+g.h){
        uint8_t* p = g.pix + ((size_t)(py - Rry)*g.w + (px - Rrx))*3;
        p[0]=r; p[1]=gg; p[2]=bb;
      }
    };
    auto drawLine = [&](int x0,int y0,int x1,int y1,uint8_t r,uint8_t gg,uint8_t bb){
      int dx = abs(x1-x0), sx = x0<x1?1:-1;
      int dy = -abs(y1-y0), sy = y0<y1?1:-1;
      int err = dx+dy;
      for(;;){
        putPix(x0,y0,r,gg,bb);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2>=dy){ err+=dy; x0+=sx; }
        if (e2<=dx){ err+=dx; y0+=sy; }
      }
    };

    const float RAD2DEG = 57.2957795f;
    const float DEG2RAD = 1.0f / RAD2DEG;

    // Heading view (0..2π), 0 = North
    float headingRad = _yaw360;
    float headingDeg = headingRad * RAD2DEG;
    while (headingDeg < 0.0f)   headingDeg += 360.0f;
    while (headingDeg >= 360.0f) headingDeg -= 360.0f;

    // 1) Garis vertikal statis (heading perangkat)
    const int lineTop    = card.y + pad;
    const int circleTop  = cy - R;
    const int overlapH   = R / 4;
    const int lineBottom = circleTop + overlapH;
    for (int y=lineTop; y<=lineBottom; ++y){
      putPix(cx, y, 60, 60, 255);   // biru
    }

    // mapping sudut dunia → sudut layar
    auto worldToScreen = [&](int deg)->float{
      float w = deg * DEG2RAD;
      float d = w - headingRad;
      return d - 1.57079632679f; // -π/2
    };

    // 2) Ring tick (lebih panjang sedikit karena radius membesar)
    for (int deg=0; deg<360; deg+=5){
      bool major = (deg % 30) == 0;
      bool mid   = (!major && (deg % 10) == 0);

      int rOuter = R;
      int rInner = R - (major ? 18 : (mid ? 12 : 7));

      float a = worldToScreen(deg);
      float ca = cosf(a), sa = sinf(a);

      int x0 = cx + (int)roundf(ca * rInner);
      int y0 = cy + (int)roundf(sa * rInner);
      int x1 = cx + (int)roundf(ca * rOuter);
      int y1 = cy + (int)roundf(sa * rOuter);

      uint8_t cr = major ? 255 : 210;
      uint8_t cg = major ? 255 : 210;
      uint8_t cb = major ? 255 : 210;
      drawLine(x0,y0,x1,y1, cr,cg,cb);
    }

    // 3) Label derajat 0,30,...,330 di luar ring
    const int txtScale = 1;
    const int glyphH   = 7 * txtScale;
    auto approxTextW = [&](const char* s)->int{
      int n = strlen(s);
      return n * 6 * txtScale;
    };

    for (int deg=0; deg<360; deg+=30){
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", deg);

      float a = worldToScreen(deg);
      float ca = cosf(a), sa = sinf(a);
      int rLab = R + 16;         // sedikit lebih jauh dari versi sebelumnya
      int px   = cx + (int)roundf(ca * rLab);
      int py   = cy + (int)roundf(sa * rLab);

      int tw = approxTextW(buf);
      int tx = px - tw/2;
      int ty = py - glyphH;

      drawText(g, Rrx,Rry, tx, ty, buf, txtScale, false);
    }

    // 4) N / E / S / W di dalam ring (lebih ke dalam, N di bawah segitiga)
    struct CardLabel { int deg; const char* txt; };
    const CardLabel CL[4] = {
      {   0, "N" },
      {  90, "E" },
      { 180, "S" },
      { 270, "W" }
    };
    const int cardScale = 2;
    const int cardGlyphH= 7 * cardScale;

    for (int i=0;i<4;++i){
      int  deg = CL[i].deg;
      const char* txt = CL[i].txt;

      float a = worldToScreen(deg);
      float ca = cosf(a), sa = sinf(a);
      int rLab = R - 42;        // lebih ke dalam dari segitiga
      int px   = cx + (int)roundf(ca * rLab);
      int py   = cy + (int)roundf(sa * rLab);

      int tw = 6 * cardScale;   // 1 huruf
      int tx = px - tw/2;
      int ty = py - cardGlyphH/2;

      drawText(g, Rrx,Rry, tx, ty, txt, cardScale, true);
    }

    // 5) Segitiga arah North (filled)
    {
      float a = worldToScreen(0);
      float ca = cosf(a), sa = sinf(a);

      int rTip   = R - 18;
      int rBase  = R - 34;
      int halfW  = 8;

      int tipX  = cx + (int)roundf(ca * rTip);
      int tipY  = cy + (int)roundf(sa * rTip);
      int baseX = cx + (int)roundf(ca * rBase);
      int baseY = cy + (int)roundf(sa * rBase);

      float pxv = -sa;
      float pyv =  ca;

      int lx = baseX + (int)roundf(pxv * (-halfW));
      int ly = baseY + (int)roundf(pyv * (-halfW));
      int rx = baseX + (int)roundf(pxv * ( halfW));
      int ry = baseY + (int)roundf(pyv * ( halfW));

      auto edge = [](int x0,int y0,int x1,int y1,int x,int y){
        return (x - x0)*(y1 - y0) - (y - y0)*(x1 - x0);
      };

      int minX = imin(imin(tipX,lx),rx);
      int maxX = imax(imax(tipX,lx),rx);
      int minY = imin(imin(tipY,ly),ry);
      int maxY = imax(imax(tipY,ly),ry);

      for (int y=minY; y<=maxY; ++y){
        for (int x=minX; x<=maxX; ++x){
          int w0 = edge(lx,ly, rx,ry, x,y);
          int w1 = edge(rx,ry, tipX,tipY, x,y);
          int w2 = edge(tipX,tipY, lx,ly, x,y);
          if ((w0>=0 && w1>=0 && w2>=0) || (w0<=0 && w1<=0 && w2<=0)){
            putPix(x,y, 60,60,255);
          }
        }
      }

      drawLine(tipX, tipY, lx, ly, 60,60,255);
      drawLine(tipX, tipY, rx, ry, 60,60,255);
      drawLine(lx,   ly,  rx, ry, 60,60,255);
    }

    // 6) Angka heading besar di tengah
    {
      char buf[32];
      snprintf(buf, sizeof(buf), "%6.1f", headingDeg);

      const int scaleC  = 2;
      const int glyphHC = 7 * scaleC;
      int tw = (int)strlen(buf) * 6 * scaleC;

      int tx = cx - tw/2;
      int ty = cy - glyphHC/2;
      drawText(g, Rrx,Rry, tx, ty, buf, scaleC, true);
    }
  }

  // ====== IMU update + mapping + Euler/Yaw ==================================
  static inline void mapAccelGyroToDevice(float ax,float ay,float az,
                                          float gx,float gy,float gz,
                                          float& dax,float& day,float& daz,
                                          float& dgx,float& dgy,float& dgz)
  {
    // Rotasi 180° sekitar +Y (sensor membelakangi layar)
    // Mapping yang diinginkan:
    //   Sensor +Y -> Perangkat +Y
    //   Sensor +Z -> Perangkat -Z
    //   Sensor +X -> Perangkat -X
    dax = -ax;  day =  ay;  daz = -az;
    dgx = -gx;  dgy =  gy;  dgz = -gz;
  }

  static inline void mapMagToDevice(float mx_raw,float my_raw,float mz_raw,
                                    float& mx,float& my,float& mz)
  {
    // AK8963 sudah di-remap internal oleh MPU9250.
    // Mapping yang diinginkan:
    //   Sensor +X -> Perangkat +X
    //   Sensor +Z -> Perangkat -Z
    //   Sensor +Y -> Perangkat -Y
    mx =  mx_raw;
    my = -my_raw;
    mz = -mz_raw;
  }

  // helpers normalisasi ke 0..2π
  static inline float wrap01(float a){ // wrap 0..2π
    const float TW = 6.28318530718f;
    while (a < 0) a += TW;
    while (a >= TW) a -= TW;
    return a;
  }

  // Wrapper: dipakai saat page aktif (boleh jalankan kalibrasi)
  void _updateIMU() {
    _updateIMU_backend(true);
  }

  // Backend sesungguhnya: allowCal = true kalau dipanggil dari page,
  // allowCal = false kalau dipanggil dari loop backend.
  void _updateIMU_backend(bool allowCal){
    using IMURt = PinsAndConfig::IMU;

    if (!_imuOk) {
      IMURt::imuOk = false;
      return;
    }

    const uint32_t now = millis();
    if (now - _lastReadMs < 10) return;   // ~100 Hz
    _lastReadMs = now;

    const float RAD2DEG = 57.2957795f;

    // --- accel & gyro (raw counts) ---
    int16_t axc,ayc,azc,gxc,gyc,gzc;
    if (_imu.readAccelGyro(axc,ayc,azc,gxc,gyc,gzc)){
      float ax = (float)axc / SCALE_ACC_G;
      float ay = (float)ayc / SCALE_ACC_G;
      float az = (float)azc / SCALE_ACC_G;

      float gx = (float)gxc / SCALE_GYRO_DPS;
      float gy = (float)gyc / SCALE_GYRO_DPS;
      float gz = (float)gzc / SCALE_GYRO_DPS;

      float dax,day,daz,dgx,dgy,dgz;
      mapAccelGyroToDevice(ax,ay,az, gx,gy,gz, dax,day,daz, dgx,dgy,dgz);

      // apply accel cal (bias+scale) & gyro bias
      _ax_g = (_hasAccel)? ( (dax - _aBiasX) * _aScaleX ) : dax;
      _ay_g = (_hasAccel)? ( (day - _aBiasY) * _aScaleY ) : day;
      _az_g = (_hasAccel)? ( (daz - _aBiasZ) * _aScaleZ ) : daz;

      _gx_dps = dgx - (_hasGyro? _gBiasX:0);
      _gy_dps = dgy - (_hasGyro? _gBiasY:0);
      _gz_dps = dgz - (_hasGyro? _gBiasZ:0);

      // ====== PROSES KALIBRASI GYRO/ACCEL (hanya jika allowCal) ======
      if (allowCal){
        if (_calMode==GyroRun){
          _gSumX += dgx; _gSumY += dgy; _gSumZ += dgz; ++_gSamples;
          if (millis() - _calStartMs >= 3000){ // auto-stop 3s
            _finalizeGyroCal();
            _calMode = Idle;
          }
        }
        if (_calMode==AccelRun){
          _updMinMax(_aMinX,_aMaxX,_ax_g);
          _updMinMax(_aMinY,_aMaxY,_ay_g);
          _updMinMax(_aMinZ,_aMaxZ,_az_g);
        }
      }

      // ====== SNAPSHOT KE BACKEND: accel, gyro, accel magnitude ======
      IMURt::ax_g       = _ax_g;
      IMURt::ay_g       = _ay_g;
      IMURt::az_g       = _az_g;
      IMURt::gx_dps     = _gx_dps;
      IMURt::gy_dps     = _gy_dps;
      IMURt::gz_dps     = _gz_dps;
      IMURt::accelMag_g = sqrtf(_ax_g*_ax_g + _ay_g*_ay_g + _az_g*_az_g);
    }

    // --- magnetometer ---
    int16_t mxc,myc,mzc;
    uint8_t of=0;
    if (_imu.readMag(mxc,myc,mzc,&of)){
      float ux,uy,uz; _imu.magRawToMicroTesla(mxc,myc,mzc, ux,uy,uz);
      float mx,my,mz; mapMagToDevice(ux,uy,uz, mx,my,mz);

      // apply mag cal (bias+scale)
      float mxc2 = (_hasMag)? ( (mx - _mBiasX) * _mScaleX ) : mx;
      float myc2 = (_hasMag)? ( (my - _mBiasY) * _mScaleY ) : my;
      float mzc2 = (_hasMag)? ( (mz - _mBiasZ) * _mScaleZ ) : mz;

      _mx_uT = mxc2; _my_uT = myc2; _mz_uT = mzc2;

      // ====== PROSES KALIBRASI MAG (hanya jika allowCal) ======
      if (allowCal && _calMode==MagRun){
        _updMinMax(_mMinX,_mMaxX,mx);
        _updMinMax(_mMinY,_mMaxY,my);
        _updMinMax(_mMinZ,_mMaxZ,mz);
      }

      // snapshot mag
      IMURt::mx_uT = _mx_uT;
      IMURt::my_uT = _my_uT;
      IMURt::mz_uT = _mz_uT;
    }

    // --- Euler dari accel + mag (tilt-comp) ---
    const float ax = _ax_g, ay = _ay_g, az = _az_g;
    const float mx = _mx_uT, my = _my_uT, mz = _mz_uT;

    // roll & pitch dari accel
    float phi   = atan2f(ay, az);                          // roll
    float theta = atan2f(-ax, sqrtf(ay*ay + az*az));       // pitch

    // tilt compensation
    float cth = cosf(theta), sth = sinf(theta);
    float cph = cosf(phi),    sph = sinf(phi);
    float mx_c = mx*cth + mz*sth;
    float my_c = mx*sph*sth + my*cph - mz*sph*cth;

    // heading mentah (0 = North, CW)
    float heading = atan2f(mx_c, my_c);
    _headingRaw   = wrap01(heading);

    // apply offset untuk heading yang dipakai UI (kompas & cube)
    float headingView = wrap01(_headingRaw + _headingOffsetRad);

    // LPF roll/pitch; yaw pakai unwrapping pada versi -π..π lalu wrap 0..2π
    const float ALP = 0.15f;
    _roll  = (1-ALP)*_roll  + ALP*phi;
    _pitch = (1-ALP)*_pitch + ALP*theta;

    float dy = headingView - _yaw360;
    const float TW = 6.28318530718f;
    if (dy >  M_PI) dy -= TW;
    if (dy < -M_PI) dy += TW;
    _yaw360 = wrap01(_yaw360 + ALP*dy);
    _yaw    = _yaw360; // untuk cube

    // ====== SNAPSHOT KE BACKEND: Euler + heading ======
    IMURt::imuOk          = true;
    IMURt::rollRad        = _roll;
    IMURt::pitchRad       = _pitch;
    IMURt::yawRad         = _yaw360;
    IMURt::headingRawRad  = _headingRaw;
    IMURt::headingViewRad = headingView;
    IMURt::headingDeg     = headingView * RAD2DEG;

    IMURt::hasGyroCal     = _hasGyro;
    IMURt::hasAccelCal    = _hasAccel;
    IMURt::hasMagCal      = _hasMag;
    IMURt::lastUpdateMs   = now;
  }

  static inline void _updMinMax(float& mn, float& mx, float v){
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }

  void _finalizeGyroCal(){
    if (_gSamples > 10){
      _gBiasX = (float)(_gSumX / (double)_gSamples);
      _gBiasY = (float)(_gSumY / (double)_gSamples);
      _gBiasZ = (float)(_gSumZ / (double)_gSamples);
      _hasGyro = true;
      _saveCalibrationToPrefs();
    }
  }

  void _finalizeAccelCal(){
    // Min/Max → bias & scale agar radius ≈ 1g
    float rx = 0.5f * (_aMaxX - _aMinX);
    float ry = 0.5f * (_aMaxY - _aMinY);
    float rz = 0.5f * (_aMaxZ - _aMinZ);
    if (rx<=0 || ry<=0 || rz<=0) return;

    _aBiasX = (_aMaxX + _aMinX) * 0.5f;
    _aBiasY = (_aMaxY + _aMinY) * 0.5f;
    _aBiasZ = (_aMaxZ + _aMinZ) * 0.5f;

    float rAvg = (rx + ry + rz) / 3.0f;
    _aScaleX = rAvg / rx;
    _aScaleY = rAvg / ry;
    _aScaleZ = rAvg / rz;

    _hasAccel = true;
    _saveCalibrationToPrefs();
  }

  void _finalizeMagCal(){
    // Min/Max → hard-iron bias + soft-iron scale
    float rx = 0.5f * (_mMaxX - _mMinX);
    float ry = 0.5f * (_mMaxY - _mMinY);
    float rz = 0.5f * (_mMaxZ - _mMinZ);
    if (rx<=0 || ry<=0 || rz<=0) return;

    _mBiasX = (_mMaxX + _mMinX) * 0.5f;
    _mBiasY = (_mMaxY + _mMinY) * 0.5f;
    _mBiasZ = (_mMaxZ + _mMinZ) * 0.5f;

    float rAvg = (rx + ry + rz) / 3.0f;
    _mScaleX = rAvg / rx;
    _mScaleY = rAvg / ry;
    _mScaleZ = rAvg / rz;

    _hasMag = true;
    _saveCalibrationToPrefs();
  }
};
