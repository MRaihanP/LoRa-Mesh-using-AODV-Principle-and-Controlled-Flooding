#pragma once
#include "PageBase.h"
#include "pins_and_config.h"
#include <Wire.h>
#include <Preferences.h>
#include "RP_BMP280.h"
#include <math.h>

// ============================================================================
// PageSensorBMP280
//   - Card 1: Pressure (T & P)
//   - Card 2: Altitude (mdpl) + vertical bar meter
//   - Card 3: Relative Altitude (dari baseline; bisa di-zero via tombol)
//   - Card 4: Calibration BMP (wrap numbering rapi + tombol Calibrate)
//   - Runtime snapshot: menulis altMSL_m & relAlt_m ke PinsAndConfig::BMP280
// ============================================================================

class PageSensorBMP280 : public PageBase {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("SENSOR BMP280");

    // I2C & BMP init
    Wire.begin(PinsAndConfig::IMU::SDA, PinsAndConfig::IMU::SCL);
    _bmpOk = _bmp.begin(PinsAndConfig::BMP280::ADDR, PinsAndConfig::BMP280::I2C_HZ);

    // Load sea-level pressure dari Preferences
    _loadPrefs();                   // set _seaLevelPa
    if (_bmpOk) _bmp.setSeaLevelPressure(_seaLevelPa);

    recomputeLayout();
    updateContentHeight();
  }

  void onEnter() override {
    PageBase::onEnter();
    updateContentHeight();
  }

  void onQSClosed() override {
    if (!isActive()) return;
    recomputeLayout();
    updateContentHeight();
    PageBase::onQSClosed();
  }

  // Backend ringan: dipanggil dari loop utama untuk update data BMP280
  // tanpa perlu membuka page (dan tanpa menjalankan proses kalibrasi).
  void backendTick(uint32_t nowMs) {
    _updateBMP_backend(false, nowMs, false);  // normal throttle
  }

  // Paksa 1x baca sensor sekarang (bypass throttle), tanpa kalibrasi
  void forceSample(uint32_t nowMs){
    _updateBMP_backend(false, nowMs, true);
  }

  int contentHeight() const { return _contentH; }

protected:
  // ========================================================================
  // RENDER
  // ========================================================================
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    _updateBMP();                   // refresh data periodik

    const int RryEff = Rry + _scrollY;

    drawPressureCard(g, Rrx, RryEff, _cardPressure);
    drawAltitudeCard(g, Rrx, RryEff, _cardAltitude);
    drawRelativeCard(g, Rrx, RryEff, _cardRelative); // <-- berisi tombol Zero
    drawCalibCard   (g, Rrx, RryEff, _cardCalib);
  }

  // ========================================================================
  // INPUT: Scroll + tombol Calibrate + tombol Zero baseline
  // ========================================================================
  void handleContentInput(bool pressed, int x, int y) override {
    const int cy = y + _scrollY;

    const bool onBtnZero = (cy >= _btnZero.y && cy < _btnZero.y + _btnZero.h &&
                            x  >= _btnZero.x && x  < _btnZero.x + _btnZero.w);

    const bool onBtnCal  = (cy >= _btnCalib.y && cy < _btnCalib.y + _btnCalib.h &&
                            x  >= _btnCalib.x && x  < _btnCalib.x + _btnCalib.w);

    if (pressed) {
      // Tombol Zero relative baseline (hanya jika ada altitude valid)
      if (onBtnZero) {
        if (isfinite(_alt_m)) {
          _relBaseAlt_m = _alt_m;
          _relBaseSet   = true;
          _zeroFlashUntil = millis() + 500; // highlight tombol 0.5s
        }
        return; // konsumsi input
      }

      // Tombol Calibrate SLP
      if (onBtnCal) {
        if (!_calRunning && _bmpOk) {
          _calRunning     = true;
          _calStartMs     = millis();
          _calSamples     = 0;
          _sumPressPa     = 0.0f;
          _sumTempC       = 0.0f;
          _calRequested   = true;   // flag supaya card tahu menggambar tombol merah
        }
        return; // konsumsi input
      }

      // Scroll
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
  using Theme = PinsAndConfig::Theme;

  // ========================================================================
  // Types & helpers
  // ========================================================================
  struct Rect { int x=0,y=0,w=0,h=0; };
  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }
  static inline uint8_t clamp8(int v){ if(v<0) return 0; if(v>255) return 255; return (uint8_t)v; }
  static inline int iClamp(int v,int lo,int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }

  // Theme
  static constexpr int     RADIUS = 12;
  static constexpr uint8_t CR = Theme::UI_R, CG = Theme::UI_G, CB = Theme::UI_B;

  // Grid & spacing
  static constexpr int EDGE       = Theme::EDGE_BUF;
  static constexpr int GAPX       = 10;
  static constexpr int GAPY12     = 6;   // antar card
  static constexpr int TITLE2CARD = 10;
  static constexpr int BOT_PAD    = 20;

  // Card geometry
  int  _cardW = 0;
  int  _cardH = 92;     // pressure / relative
  int  _altH  = 160;    // altitude (lebih tinggi)
  int  _calH  = 200;    // calibration

  Rect _cardPressure, _cardAltitude, _cardRelative, _cardCalib;

  // BMP & data
  RP_BMP280 _bmp;
  bool      _bmpOk = false;
  uint32_t  _lastReadMs = 0;

  float _tC = 0.0f;
  float _pPa = NAN;
  float _alt_m = NAN;       // mdpl (pakai sea-level Pa)
  bool  _relBaseSet = false;
  float _relBaseAlt_m = 0.0f;
  float _relAlt_m = 0.0f;

  // Sea-Level Pressure (kalibrasi) disimpan di Preferences
  float _seaLevelPa = PinsAndConfig::BMP280::SEA_LEVEL_PA_DEFAULT;

  // Scroll state
  int  _contentH = 0;
  int  _scrollY  = 0;
  int  _scrollMax= 0;
  bool _dragging = false;
  int  _dragY0   = 0;
  int  _scrollY0 = 0;

  // Buttons
  Rect _btnCalib;   // di kartu Calibration
  Rect _btnZero;    // di kartu Relative Altitude

  // Proses kalibrasi (average beberapa sample pressure)
  bool      _calRunning   = false;
  bool      _calRequested = false;  // state tombol jadi merah saat proses
  uint32_t  _calStartMs   = 0;
  int       _calSamples   = 0;
  float     _sumPressPa   = 0.0f;
  float     _sumTempC     = 0.0f;

  // Visual flash tombol Zero
  uint32_t  _zeroFlashUntil = 0;

  // Altitude bar range (mdpl)
  static constexpr float ALT_MIN_M = 0.0f;
  static constexpr float ALT_MAX_M = 4000.0f;

  // ========================================================================
  // Layout
  // ========================================================================
  void recomputeLayout(){
    const int cardsTop = _viewportY0 + TITLE2CARD;

    _cardW = (TFT_WIDTH - 2*EDGE - GAPX) / 2;
    if (_cardW < 40) _cardW = 40;

    // Row-1 : Pressure (kiri), Relative (kanan)
    _cardPressure = { EDGE,                 cardsTop, _cardW, _cardH };
    _cardRelative = { EDGE + _cardW + GAPX, cardsTop, _cardW, _cardH };

    // Row-2 : Altitude (full width)
    const int row2Top = cardsTop + _cardH + GAPY12;
    const int fullW   = TFT_WIDTH - 2*EDGE;
    _cardAltitude     = { EDGE, row2Top, fullW, _altH };

    // Row-3 : Calibration (full width)
    const int row3Top = row2Top + _altH + GAPY12;
    _cardCalib        = { EDGE, row3Top, fullW, _calH };
  }

  void updateContentHeight(){
    const int bottom = _cardCalib.y + _cardCalib.h;
    _contentH = bottom + BOT_PAD;

    const int vpH = _viewportH;
    int maxY = _contentH - vpH;
    if (maxY < 0) maxY = 0;
    _scrollMax = maxY;
    if (_scrollY > _scrollMax) _scrollY = _scrollMax;
  }

  // ========================================================================
  // Utils: draw fill rounded clipped
  // ========================================================================
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

  // Quick width estimator
  static int estTextW(const char* s, int scale){ return (int)(strlen(s)) * (scale==1 ? 6 : 12); }

  // Word wrap
  int drawWrapped(GfxRGB888& g, int Rrx,int Rry, int x,int y,int w,
                  const char* text, int scale, bool bold=false)
  {
    const int lineH = 7*scale;
    const int maxW  = w;

    const char* p = text;
    char buf[160];
    while (*p){
      int lenLine = 0;
      int wLine   = 0;
      const char* lineStart = p;
      const char* lastGood  = p;

      while (*p && *p!='\n'){
        const char* wordStart = p;
        while (*p && *p!=' ' && *p!='\n') ++p;
        int wordLen = (int)(p - wordStart);
        int addW    = wordLen * (scale==1 ? 6 : 12);
        int nextW   = (lenLine==0 ? addW : wLine + (scale==1?6:12) + addW); // +spasi

        if (nextW <= maxW){
          lastGood = p;
          wLine    = nextW;
          lenLine  = (int)(p - lineStart);
        } else {
          if (lenLine==0){
            lenLine = wordLen;
            wLine   = addW;
            lastGood= wordStart + wordLen;
          }
          break;
        }
        if (*p==' ') ++p;
      }

      if (lenLine > 0){
        int copyN = lenLine; if (copyN > (int)sizeof(buf)-1) copyN = (int)sizeof(buf)-1;
        memcpy(buf, lineStart, copyN);
        buf[copyN] = 0;
        drawText(g, Rrx,Rry, x, y, buf, scale, bold);
        y += lineH;
      }
      if (*p=='\n'){ ++p; }
      else if (*p && lastGood){
        p = (*p==' ') ? lastGood+1 : lastGood;
      }
    }
    return y;
  }

  // Numbered wrap: area nomor tetap, konten wrap terpisah
  int drawNumberedWrap(GfxRGB888& g, int Rrx,int Rry,
                       int left,int top,int width,
                       const char* const* items, int nItems,
                       int scale=1, int numW=16, int pad=4)
  {
    int y = top;
    for (int i=0;i<nItems;++i){
      char num[8];
      snprintf(num,sizeof(num), "%d.", i+1);
      drawText(g, Rrx,Rry, left, y, num, scale, true);
      int contentX = left + numW + pad;
      int contentW = width - numW - pad;
      y = drawWrapped(g, Rrx,Rry, contentX, y, contentW, items[i], scale, false);
    }
    return y;
  }

  // ========================================================================
  // Cards
  // ========================================================================
  void drawPressureCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);
    const int pad=10; const int scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Pressure", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 8;
    char ln[64];
    if (_bmpOk){
      snprintf(ln,sizeof(ln),"Temp : %0.2f C", _tC);
      drawText(g, Rrx,Rry, card.x+pad, baseY, ln, 1, false);
      snprintf(ln,sizeof(ln),"Press: %0.1f Pa", _pPa);
      drawText(g, Rrx,Rry, card.x+pad, baseY+12, ln, 1, false);
    } else {
      drawText(g, Rrx,Rry, card.x+pad, baseY, "Sensor not ready", 1, false);
    }
  }

  void drawAltitudeCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);
    const int pad=10; const int scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Altitude (mdpl)", scaleT, true);

    const int top  = card.y + pad + 7*scaleT + 8;
    const int left = card.x + pad;
    const int right= card.x + card.w - pad;
    const int meterW = 28;
    const int meterH = card.h - (top - card.y) - pad;

    // Clear area
    const int drawTop = imax(top, Rry);
    const int drawBot = imin(top + meterH, Rry + g.h);
    for (int gy=drawTop; gy<drawBot; ++gy){
      int x0 = imax(left, Rrx);
      int x1 = imin(right, Rrx + g.w);
      if (x1 <= x0) continue;
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
    }

    // Meter (bar vertikal)
    Rect m{ right - meterW, top, meterW, meterH };
    drawAltMeter(g, Rrx,Rry, m);

    // Nilai angka di sisi kiri
    int textX = left;
    int textY = top;
    char ln[64];
    if (isfinite(_alt_m)){
      snprintf(ln,sizeof(ln),"Alt : %0.2f m", _alt_m);
      drawText(g, Rrx,Rry, textX, textY, ln, 1, true); textY += 14;
      snprintf(ln,sizeof(ln),"Ref : %.0f Pa", _seaLevelPa);
      drawText(g, Rrx,Rry, textX, textY, ln, 1, false); textY += 12;
      snprintf(ln,sizeof(ln),"Range meter: 0..%.0f m", ALT_MAX_M);
      drawText(g, Rrx,Rry, textX, textY, ln, 1, false);
    } else {
      drawText(g, Rrx,Rry, textX, textY, "Alt: --", 1, true);
    }
  }

  void drawRelativeCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);
    const int pad=10; const int scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Relative Altitude", scaleT, true);

    const int baseY = card.y + pad + 7*scaleT + 8;
    char ln[64];
    if (isfinite(_relAlt_m)){
      snprintf(ln,sizeof(ln),"Rel : %+0.2f m", _relAlt_m);
      drawText(g, Rrx,Rry, card.x+pad, baseY, ln, 1, true);
      drawText(g, Rrx,Rry, card.x+pad, baseY+12, "(baseline = first valid alt or Zero)", 1, false);
    } else {
      drawText(g, Rrx,Rry, card.x+pad, baseY, "Rel : --", 1, true);
    }

    // Tombol ZERO baseline (center bawah)
    const int btnW = 110;
    const int btnH = 28;
    const int btnX = card.x + (card.w - btnW)/2;
    const int btnY = card.y + card.h - pad - btnH;
    _btnZero = { btnX, btnY, btnW, btnH };

    // Warna: flash hijau sebentar setelah ditekan
    bool flash = (millis() < _zeroFlashUntil);
    uint8_t br = flash ? 40  : 120;
    uint8_t bg = flash ? 200 : 120;
    uint8_t bb = flash ? 60  : 120;

    fillRoundedClipped(g, Rrx,Rry, _btnZero, 10, br,bg,bb);
    drawText(g, Rrx,Rry, btnX + 22, btnY + 7, "Zero", 2, true);
  }

  void drawCalibCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, RADIUS, CR,CG,CB);
    const int pad=10; const int scaleT=2;

    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Calibration BMP", scaleT, true);

    // Konten: langkah-langkah ter-wrap dengan numbering tetap rapi
    const int areaLeft = card.x + pad;
    const int areaTop  = card.y + pad + 7*scaleT + 8;
    const int areaW    = card.w - 2*pad;
    const int areaH    = card.h - (areaTop - card.y) - pad;

    // Clear konten
    const int drawTop = imax(areaTop, Rry);
    const int drawBot = imin(areaTop + areaH, Rry + g.h);
    for (int gy=drawTop; gy<drawBot; ++gy){
      int x0 = imax(areaLeft, Rrx);
      int x1 = imin(areaLeft + areaW, Rrx + g.w);
      if (x1 <= x0) continue;
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
    }

    const char* steps[] = {
      "Dapatkan ketinggian lokasi (mdpl) dari peta/aplikasi (misal Google Maps).",
      "Letakkan perangkat diam selama 5–10 detik agar pembacaan stabil.",
      "Tekan tombol Calibrate: modul mengambil beberapa sampel tekanan,\n"
      "menghitung Sea-Level Pressure agar altitude mdpl = ketinggian lokasi,\n"
      "menyimpan hasil ke Preferences, dan langsung dipakai."
    };
    (void)drawNumberedWrap(g, Rrx,Rry, areaLeft, areaTop, areaW, steps, 3, 1, 16, 4);

    // Tombol Calibrate (fix posisi di bawah area)
    const int btnW = 128;
    const int btnH = 30;
    const int btnX = areaLeft + (areaW - btnW)/2;
    const int btnY = card.y + card.h - pad - btnH;
    _btnCalib = { btnX, btnY, btnW, btnH };

    // Warna tombol: merah saat proses, abu-abu saat idle
    uint8_t br, bg, bb;
    if (_calRunning || _calRequested) { br=220; bg=40; bb=40; }   // merah
    else                              { br=120; bg=120; bb=120; } // abu

    fillRoundedClipped(g, Rrx,Rry, _btnCalib, 10, br,bg,bb);
    drawText(g, Rrx,Rry, btnX + 18, btnY + 8, "Calibrate", 2, true);

    // Status singkat
    char st[64];
    if (!_bmpOk) {
      snprintf(st,sizeof(st),"Status: sensor not ready");
    } else if (_calRunning) {
      snprintf(st,sizeof(st),"Calibrating... %d sampel", _calSamples);
    } else {
      snprintf(st,sizeof(st),"Sea-Level: %.0f Pa (site: %.0f m)", _seaLevelPa, PinsAndConfig::BMP280::SITE_ALT_M_DEFAULT);
    }
    drawText(g, Rrx,Rry, areaLeft, btnY - 14, st, 1, false);
  }

  // Meter: vertical bar 0..ALT_MAX_M, nilai di _alt_m (clamped)
  void drawAltMeter(GfxRGB888& g, int Rrx,int Rry, const Rect& m){
    // border 1px putih
    for (int gy=imax(m.y,Rry); gy<imin(m.y+m.h,Rry+g.h); ++gy){
      uint8_t* rowL = g.pix + ((size_t)(gy - Rry)*g.w + (imax(m.x,Rrx) - Rrx))*3;
      uint8_t* rowR = g.pix + ((size_t)(gy - Rry)*g.w + (imin(m.x+m.w-1, Rrx+g.w-1) - Rrx))*3;
      if (m.x>=Rrx && m.x<Rrx+g.w){ rowL[0]=255; rowL[1]=255; rowL[2]=255; }
      if (m.x+m.w-1>=Rrx && m.x+m.w-1<Rrx+g.w){ rowR[0]=255; rowR[1]=255; rowR[2]=255; }
    }
    for (int gx=imax(m.x,Rrx); gx<imin(m.x+m.w,Rrx+g.w); ++gx){
      uint8_t* top = g.pix + ((size_t)(imax(m.y,Rry) - Rry)*g.w + (gx - Rrx))*3;
      uint8_t* bot = g.pix + ((size_t)(imin(m.y+m.h-1,Rry+g.h-1) - Rry)*g.w + (gx - Rrx))*3;
      top[0]=255; top[1]=255; top[2]=255; bot[0]=255; bot[1]=255; bot[2]=255;
    }

    // Fill
    if (isfinite(_alt_m)){
      float t = (_alt_m - ALT_MIN_M) / (ALT_MAX_M - ALT_MIN_M);
      if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
      int fillH = (int)floorf(t * (m.h-2));
      int fy0   = m.y + (m.h-1) - 1 - fillH;
      int fy1   = m.y + m.h - 1 - 1;
      fy0 = iClamp(fy0, m.y+1, m.y+m.h-2);
      fy1 = iClamp(fy1, m.y+1, m.y+m.h-2);

      for (int gy=imax(fy0,Rry); gy<=imin(fy1,Rry+g.h-1); ++gy){
        int x0 = imax(m.x+1, Rrx);
        int x1 = imin(m.x+m.w-2, Rrx+g.w-1);
        if (x1 < x0) continue;
        uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
        for (int gx=x0; gx<=x1; ++gx){ row[0]=220; row[1]=240; row[2]=255; row+=3; }
      }
    }
  }

  // ========================================================================
  // BMP update & calibration
  // ========================================================================
  // Wrapper lama: dipakai saat page sedang aktif (grafik + kalibrasi OK)
  void _updateBMP() {
    _updateBMP_backend(true, millis(), false);
  }

  // Backend sesungguhnya: allowCal = true kalau dipanggil dari page,
  // allowCal = false kalau dipanggil dari loop backend.
  void _updateBMP_backend(bool allowCal, uint32_t now, bool force){
    if (!_bmpOk) return;

    const uint32_t interval = _calRunning ? 5u : 200u;
    if (!force){
      if (now - _lastReadMs < interval) return;
    }
    _lastReadMs = now;

    float tC, pPa;
    if (_bmp.read(tC, pPa)){
      _tC   = tC;
      _pPa  = pPa;
      _alt_m = _bmp.altitudeFromPressure(pPa);

      if (!_relBaseSet && isfinite(_alt_m)){
        _relBaseAlt_m = _alt_m;
        _relBaseSet   = true;
      }
      if (_relBaseSet) {
        _relAlt_m = _alt_m - _relBaseAlt_m;
      }

      PinsAndConfig::BMP280::altMSL_m     = _alt_m;
      PinsAndConfig::BMP280::relAlt_m     = _relBaseSet ? _relAlt_m : NAN;
      PinsAndConfig::BMP280::altValid     = isfinite(_alt_m);
      PinsAndConfig::BMP280::relAltValid  = _relBaseSet && isfinite(_relAlt_m);
      PinsAndConfig::BMP280::lastUpdateMs = now;

      if (allowCal){
        if (_calRunning){
          _sumPressPa += pPa;
          _sumTempC   += tC;
          _calSamples++;

          if (_calSamples >= 400){
            const float pAvg      = _sumPressPa / _calSamples;
            const float targetAlt = PinsAndConfig::BMP280::SITE_ALT_M_DEFAULT;
            const float oneMinus  = 1.0f - (targetAlt / 44330.0f);
            const float expo      = 1.0f / 0.1903f;
            const float p0        = pAvg / powf(oneMinus, expo);

            _seaLevelPa = p0;
            _bmp.setSeaLevelPressure(_seaLevelPa);
            _savePrefs();

            _calRunning   = false;
            _calRequested = false;
            _calSamples   = 0;
            _sumPressPa   = 0.0f;
            _sumTempC     = 0.0f;
          }
        } else {
          _calRequested = false;
        }
      }
    }
  }

  void _loadPrefs(){
    Preferences p;
    if (p.begin("bmp_cal", true)){  // read-only
      _seaLevelPa = p.getFloat("bmp_slp", PinsAndConfig::BMP280::SEA_LEVEL_PA_DEFAULT);
      p.end();
    }
  }
  void _savePrefs(){
    Preferences p;
    if (p.begin("bmp_cal", false)){ // read-write
      p.putFloat("bmp_slp", _seaLevelPa);
      p.end();
    }
  }
};
