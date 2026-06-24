#pragma once
#include "PageBase.h"
#include "pins_and_config.h"
#include <Wire.h>
#include <Preferences.h>
#include "RP_SHT30.h"
#include <math.h>

// Alias theme biar konsisten dengan page lain
using Theme = PinsAndConfig::Theme;

class PageSensorSHT30 : public PageBase {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("SENSOR SHT30");

    // I2C & SHT30 init
    Wire.begin(PinsAndConfig::IMU::SDA, PinsAndConfig::IMU::SCL);
    _shtOk = _sht.begin(Wire, 0x45, 100000);   // addr 0x45 @100kHz

    _loadPrefs(); // load _tOffsetC & _refTempC

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

  // Backend ringan: dipanggil dari loop utama untuk update data SHT30
  // tanpa perlu membuka page. Hanya baca sensor & isi PinsAndConfig::SHT30.
  void backendTick(uint32_t nowMs) {
    _updateSHT(nowMs, false);
  }

  // Paksa 1x baca sensor sekarang (bypass throttle)
  void forceSample(uint32_t nowMs){
    _updateSHT(nowMs, true);
  }

  int contentHeight() const { return _contentH; }

protected:
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    _updateSHT(millis(), false);

    const int RryEff = Rry + _scrollY;

    drawTempBarCard (g, Rrx, RryEff, _cardTemp);
    drawHumBarCard  (g, Rrx, RryEff, _cardHum);
    drawIntTempCard (g, Rrx, RryEff, _cardInt);
    drawCalibCard   (g, Rrx, RryEff, _cardCalib);
  }

  void handleContentInput(bool pressed, int x, int y) override {
    const int cy = y + _scrollY;

    const bool hitMinus1    = inRect(_btnMinus1, x, cy);
    const bool hitMinus01   = inRect(_btnMinus01, x, cy);
    const bool hitPlus01    = inRect(_btnPlus01, x, cy);
    const bool hitPlus1     = inRect(_btnPlus1, x, cy);
    const bool hitApply     = inRect(_btnApply, x, cy);
    const bool hitZero      = inRect(_btnZero,  x, cy);

    if (pressed) {
      if      (hitMinus1)  { _refTempC -= 1.0f;  _refTempC = clampf(_refTempC, -40.f, 125.f); return; }
      else if (hitMinus01) { _refTempC -= 0.1f;  _refTempC = clampf(_refTempC, -40.f, 125.f); return; }
      else if (hitPlus01)  { _refTempC += 0.1f;  _refTempC = clampf(_refTempC, -40.f, 125.f); return; }
      else if (hitPlus1)   { _refTempC += 1.0f;  _refTempC = clampf(_refTempC, -40.f, 125.f); return; }
      else if (hitApply) {
        if (_shtOk && isfinite(_tRawC)) {
          // Terapkan PROPOSED offset = RefTemp - T_raw
          _tOffsetC = _refTempC - _tRawC;
          _savePrefs();
          _applyFlashMs = millis();
        }
        return;
      }
      else if (hitZero) {
        _tOffsetC = 0.0f;
        _savePrefs();
        _applyFlashMs = millis();
        return;
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
  struct Rect { int x=0,y=0,w=0,h=0; };

  static inline bool inRect(const Rect& r, int x, int y){
    return (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h);
  }
  static inline int   imax(int a,int b){ return (a>b)?a:b; }
  static inline int   imin(int a,int b){ return (a<b)?a:b; }
  static inline float clampf(float v,float lo,float hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
  static inline uint8_t clamp8(int v){ if(v<0) return 0; if(v>255) return 255; return (uint8_t)v; }

  static constexpr int     RADIUS = 12;
  static constexpr uint8_t CR = Theme::UI_R, CG = Theme::UI_G, CB = Theme::UI_B;

  static constexpr int EDGE       = Theme::EDGE_BUF;
  static constexpr int GAPX       = 10;
  static constexpr int GAPY12     = 6;
  static constexpr int GAPY23     = 6;
  static constexpr int TITLE2CARD = 10;
  static constexpr int BOT_PAD    = 20;

  int  _cardW = 0;
  int  _cardH = 100;   // bar cards (ringan)
  int  _intH  = 100;   // internal temp card
  int  _calH  = 210;   // calibration card

  Rect _cardTemp, _cardHum, _cardInt, _cardCalib;

  RP_SHT30  _sht;
  bool      _shtOk = false;
  uint32_t  _lastReadMs = 0;

  float _tRawC = NAN;      // temperature raw dari SHT30
  float _rhPct = NAN;      // humidity %
  float _tCorrC = NAN;     // temperature corrected (raw + offset)

  float _tOffsetC = 0.0f;  // offset TERAPLIKASI saat ini (tersimpan)
  float _refTempC = 25.0f; // target referensi yang disetel user
  uint32_t _applyFlashMs = 0;

  int  _contentH = 0;
  int  _scrollY  = 0;
  int  _scrollMax= 0;
  bool _dragging = false;
  int  _dragY0   = 0;
  int  _scrollY0 = 0;

  Rect _btnMinus1, _btnMinus01, _btnPlus01, _btnPlus1, _btnApply, _btnZero;

  void recomputeLayout(){
    const int cardsTop = _viewportY0 + TITLE2CARD;

    _cardW = (TFT_WIDTH - 2*EDGE - GAPX) / 2;
    if (_cardW < 60) _cardW = 60;

    _cardTemp = { EDGE,                 cardsTop, _cardW, _cardH };
    _cardHum  = { EDGE + _cardW + GAPX, cardsTop, _cardW, _cardH };

    const int row2Top = cardsTop + _cardH + GAPY12;
    const int w2      = _cardW;
    const int x2      = (TFT_WIDTH - w2) / 2;
    _cardInt  = { x2, row2Top, w2, _intH };

    const int row3Top = row2Top + _intH + GAPY23;
    const int fullW   = TFT_WIDTH - 2*EDGE;
    _cardCalib = { EDGE, row3Top, fullW, _calH };
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

  // ======= super-light fill rect (row spans) =======
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

  // ======= Lightweight bar gauge =======
  void drawBarGauge(GfxRGB888& g, int Rrx,int Rry,
                    int x,int y,int w,int h,
                    float frac,
                    uint8_t tr,uint8_t tg,uint8_t tb,
                    uint8_t fr,uint8_t fg,uint8_t fb)
  {
    if (w<=0 || h<=0) return;
    frac = clampf(frac, 0.f, 1.f);

    // Track
    fillRectClipped(g, Rrx,Rry, x,y,w,h, tr,tg,tb);

    // Fill
    int fw = (int)floorf(frac * w + 0.5f);
    if (fw > 0) fillRectClipped(g, Rrx,Rry, x,y,fw,h, fr,fg,fb);

    // Border tipis (1px)
    fillRectClipped(g, Rrx,Rry, x,     y,      w,1,  255,255,255);
    fillRectClipped(g, Rrx,Rry, x,     y+h-1,  w,1,  255,255,255);
    fillRectClipped(g, Rrx,Rry, x,     y,      1,h,  255,255,255);
    fillRectClipped(g, Rrx,Rry, x+w-1, y,      1,h,  255,255,255);
  }

  // ======= Word wrap =======
  int drawWrapped(GfxRGB888& g, int Rrx,int Rry, int x,int y,int w,
                  const char* text, int scale, bool bold=false)
  {
    const int lineH = 7*scale;
    const int charW = (scale==1 ? 6 : 12);

    const char* p = text;
    char buf[180];
    while (*p){
      const char* lineStart = p;
      int usedW=0;
      const char* lastBreakP = p;
      while (*p && *p!='\n'){
        const char* wordStart = p;
        while (*p && *p!=' ' && *p!='\n') ++p;
        int wordLen = (int)(p - wordStart);
        int addW = (wordLen>0 ? wordLen*charW : 0);
        int nextW = (usedW==0 ? addW : usedW + charW + addW);
        if (nextW <= w){ usedW = nextW; lastBreakP = p; }
        else{
          if (usedW==0){ lastBreakP = wordStart + wordLen; usedW = addW; }
          break;
        }
        if (*p==' ') ++p;
      }
      int n = (int)(lastBreakP - lineStart);
      if (n>0){
        if (n > (int)sizeof(buf)-1) n = (int)sizeof(buf)-1;
        memcpy(buf, lineStart, n); buf[n]=0;
        drawText(g, Rrx,Rry, x, y, buf, scale, bold);
        y += lineH;
      }
      if (*p=='\n'){ ++p; }
      else if (*p) { p = (*p==' ')? lastBreakP+1 : lastBreakP; }
    }
    return y;
  }

  int drawNumberedWrap(GfxRGB888& g, int Rrx,int Rry,
                       int left,int top,int width,
                       const char* const* items, int nItems,
                       int scale=1, int numW=16, int pad=4)
  {
    int y = top;
    for (int i=0;i<nItems;++i){
      char num[8]; snprintf(num,sizeof(num), "%d.", i+1);
      drawText(g, Rrx,Rry, left, y, num, scale, true);
      int contentX = left + numW + pad;
      int contentW = width - numW - pad;
      y = drawWrapped(g, Rrx,Rry, contentX, y, contentW, items[i], scale, false);
    }
    return y;
  }

  // ======= Cards (ringan) =======
  void drawTempBarCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, 6, CR,CG,CB);
    const int pad=10, scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Temperature (corrected)", scaleT, true);

    // area dalam
    int areaX = card.x + pad, areaW = card.w - 2*pad;
    int areaY = card.y + pad + 7*scaleT + 6;
    int areaH = card.h - (areaY - card.y) - pad;
    fillRectClipped(g, Rrx,Rry, areaX, areaY, areaW, areaH, 26,28,38);

    float val = isfinite(_tCorrC) ? _tCorrC : 0.0f;
    float frac = clampf((val - 0.0f) / 50.0f, 0.f, 1.f);
    int barH = 18;
    int barY = areaY + (areaH - barH)/2 + 8;
    drawBarGauge(g, Rrx,Rry, areaX, barY, areaW, barH, frac,
                 60,62,70,  235,235,240);

    char txt[48];
    snprintf(txt,sizeof(txt), "%0.2f C", isfinite(_tCorrC)?_tCorrC:0.0f);
    drawText(g, Rrx,Rry, areaX, barY - 10, txt, 1, true);
  }

  void drawHumBarCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, 6, CR,CG,CB);
    const int pad=10, scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Humidity", scaleT, true);

    int areaX = card.x + pad, areaW = card.w - 2*pad;
    int areaY = card.y + pad + 7*scaleT + 6;
    int areaH = card.h - (areaY - card.y) - pad;
    fillRectClipped(g, Rrx,Rry, areaX, areaY, areaW, areaH, 26,28,38);

    float val = isfinite(_rhPct) ? _rhPct : 0.0f;
    float frac = clampf(val/100.0f, 0.f, 1.f);
    int barH = 18;
    int barY = areaY + (areaH - barH)/2 + 8;
    drawBarGauge(g, Rrx,Rry, areaX, barY, areaW, barH, frac,
                 60,62,70,  200,240,255);

    char txt[48];
    snprintf(txt,sizeof(txt), "%0.1f %%", isfinite(_rhPct)?_rhPct:0.0f);
    drawText(g, Rrx,Rry, areaX, barY - 10, txt, 1, true);
  }

  void drawIntTempCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, 6, CR,CG,CB);
    const int pad=10, scaleT=2;
    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Internal Temp (raw)", scaleT, true);

    int areaX = card.x + pad, areaW = card.w - 2*pad;
    int areaY = card.y + pad + 7*scaleT + 6;
    int areaH = card.h - (areaY - card.y) - pad;
    fillRectClipped(g, Rrx,Rry, areaX, areaY, areaW, areaH, 26,28,38);

    float val = isfinite(_tRawC) ? _tRawC : 0.0f;
    float frac = clampf((val - 0.0f)/50.0f, 0.f, 1.f);
    int barH = 18;
    int barY = areaY + (areaH - barH)/2 + 8;
    drawBarGauge(g, Rrx,Rry, areaX, barY, areaW, barH, frac,
                 60,62,70,  255,200,200);

    char txt[48];
    snprintf(txt,sizeof(txt), "%0.2f C", isfinite(_tRawC)?_tRawC:0.0f);
    drawText(g, Rrx,Rry, areaX, barY - 10, txt, 1, true);
  }

  void drawCalibCard(GfxRGB888& g, int Rrx,int Rry, const Rect& card){
    if (card.y >= Rry + g.h || (card.y+card.h) <= Rry) return;
    fillRoundedClipped(g, Rrx,Rry, card, 6, CR,CG,CB);
    const int pad=10, scaleT=2;

    drawText(g, Rrx,Rry, card.x+pad, card.y+pad, "Calibration", scaleT, true);

    const int areaLeft = card.x + pad;
    const int areaTop  = card.y + pad + 7*scaleT + 8;
    const int areaW    = card.w - 2*pad;
    const int areaH    = card.h - (areaTop - card.y) - pad;

    fillRectClipped(g, Rrx,Rry, areaLeft, areaTop, areaW, areaH, 26,28,38);

    const char* steps[] = {
      "Siapkan termometer referensi (external temperature) yang akurat.",
      "Biarkan perangkat & referensi stabil dalam lingkungan yang sama.",
      "Setel 'Ref Temp' dengan tombol ± sesuai bacaan referensi.",
      "Tekan 'Apply Offset' untuk menyimpan offset = (RefTemp - T_raw).",
      "Tekan 'Zero Offset' untuk kembali ke offset 0."
    };
    int yCursor = drawNumberedWrap(g, Rrx,Rry, areaLeft, areaTop, areaW, steps, 5, 1, 16, 4);
    yCursor += 4;

    // Hitung PROPOSED offset (sebelum diterapkan)
    float proposedOff = (isfinite(_tRawC) ? (_refTempC - _tRawC) : NAN);
    float previewCorr = (isfinite(_tRawC) && isfinite(proposedOff)) ? (_tRawC + proposedOff) : NAN;

    // Info ringkas
    char ln[96];
    snprintf(ln,sizeof(ln), "T_raw=%0.2f C   RefTemp=%0.2f C   Offset(applied)=%0.2f C",
             isfinite(_tRawC)?_tRawC:0.0f, _refTempC, _tOffsetC);
    drawText(g, Rrx,Rry, areaLeft, yCursor, ln, 1, true);
    yCursor += 12;

    // Baris “Proposed Offset” + “Preview Corrected”
    char ln2[96];
    if (isfinite(proposedOff)) {
      snprintf(ln2,sizeof(ln2), "Proposed Offset=%0.2f C   Preview Corrected=%0.2f C",
               proposedOff, isfinite(previewCorr)?previewCorr:0.0f);
    } else {
      snprintf(ln2,sizeof(ln2), "Proposed Offset=--        Preview Corrected=--");
    }
    drawText(g, Rrx,Rry, areaLeft, yCursor, ln2, 1, false);
    yCursor += 16;

    // Buttons row: [-1] [-0.1] [+0.1] [+1]
    const int btnH = 24, btnW = 48, gap = 8;
    int bx = areaLeft, by = yCursor;

    _btnMinus1  = { bx, by, btnW, btnH }; bx += btnW + gap;
    _btnMinus01 = { bx, by, btnW, btnH }; bx += btnW + gap;
    _btnPlus01  = { bx, by, btnW, btnH }; bx += btnW + gap;
    _btnPlus1   = { bx, by, btnW, btnH };

    auto drawBtn = [&](const Rect& r, const char* label){
      fillRoundedClipped(g, Rrx,Rry, r, 6, 120,120,120);
      drawText(g, Rrx,Rry, r.x + (r.w/2 - (int)strlen(label)*6/2), r.y + 6, label, 1, true);
    };
    drawBtn(_btnMinus1,  "-1");
    drawBtn(_btnMinus01, "-0.1");
    drawBtn(_btnPlus01,  "+0.1");
    drawBtn(_btnPlus1,   "+1");

    yCursor += btnH + 8;

    // Apply & Zero buttons
    const int bigW = 120, bigH = 26;
    const int cx = areaLeft + areaW/2;
    _btnApply = { cx - bigW - 6, yCursor, bigW, bigH };
    _btnZero  = { cx + 6,        yCursor, bigW, bigH };

    bool flash = (millis() - _applyFlashMs) < 250u;
    uint8_t ar = flash ? 220 : 140, ag = flash ? 40 : 140, ab = flash ? 40 : 140;
    fillRoundedClipped(g, Rrx,Rry, _btnApply, 8, ar,ag,ab);
    drawText(g, Rrx,Rry, _btnApply.x + 10, _btnApply.y + 7, "Apply Offset", 1, true);

    fillRoundedClipped(g, Rrx,Rry, _btnZero, 8, 90,90,90);
    drawText(g, Rrx,Rry, _btnZero.x + 14, _btnZero.y + 7, "Zero Offset", 1, true);
  }

  // ======= Sensor update (dibatasi agar ringan) =======
  void _updateSHT(uint32_t now, bool force){
    if (!_shtOk) return;
    if (!force){
      if (now - _lastReadMs < 500u) return; // ~2 Hz
    }
    _lastReadMs = now;

    float T, RH;
    if (_sht.read(T, RH)){
      _tRawC  = T;
      _rhPct  = RH;
      _tCorrC = _tRawC + _tOffsetC;

      PinsAndConfig::SHT30::tempC        = _tCorrC;
      PinsAndConfig::SHT30::humPct       = _rhPct;
      PinsAndConfig::SHT30::tempValid    = isfinite(_tCorrC);
      PinsAndConfig::SHT30::humValid     = isfinite(_rhPct);
      PinsAndConfig::SHT30::lastUpdateMs = now;
    }
  }

  void _loadPrefs(){
    Preferences p;
    if (p.begin("sht_cal", true)) { // read-only
      _tOffsetC = p.getFloat("t_off", 0.0f);
      _refTempC = p.getFloat("refT",  25.0f);
      p.end();
    }
  }
  void _savePrefs(){
    Preferences p;
    if (p.begin("sht_cal", false)) { // read-write
      p.putFloat("t_off", _tOffsetC);
      p.putFloat("refT",  _refTempC);
      p.end();
    }
  }
};
