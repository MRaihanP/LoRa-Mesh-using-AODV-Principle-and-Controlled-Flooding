#pragma once

#include <RP_TFTDisplay.h>
#include "OS.h"
#include "pins_and_config.h"
#include "IRouter.h"
#include <math.h>

// ====== MenuPage: judul di tengah + scrollable rounded cards ======
// - Card bisa ditekan (tap/hold tanpa geser) -> event klik (sementara: Serial log + highlight)
// - Jika hold & geser melewati ambang (vertikal) -> masuk mode scroll (highlight batal)
// - Tanpa bottom pad dan tanpa tombol Back; Back via ripple (edge-swipe)
class MenuPage {
public:
  void begin(AppOS* os, TopBar* top){
    _os  = os;
    _top = top;

    // --- Title layout (tetap) ---
    _titleScale  = 3;
    _titleStr    = "MENU";
    _titleW      = SimpleFont::textWidth(_titleStr, _titleScale, 1);
    _titlePadTop = 8;
    _titleYTop   = Theme::BAR_H + _titlePadTop;
    _titleGlyphH = 7 * _titleScale;
    _titleX      = (TFT_WIDTH - _titleW) / 2;

    // Viewport setelah title
    _viewportY0  = _titleYTop + _titleGlyphH + 12;
    _viewportH   = max(0, TFT_HEIGHT - _viewportY0);
    _scrollableH = _viewportH;

    // --- Card list (placeholder) ---
    static const char* kLabels[] = {
      "Simple Chat","Network","Log Path","Log Data","Sensor MPU9250",
      "Sensor BMP280","Sensor SHT30","GNSS M10","Clock DS3231",
      "Micro SD Card Reader","Battery", "WiFi"
    };
    _count = (int)(sizeof(kLabels)/sizeof(kLabels[0]));
    for (int i=0;i<_count;i++){ _labels[i] = kLabels[i]; }

    _cardPadX = 12;
    _cardR    = 14;
    _cardH    = 64;
    _gapY     = 10;

    _contentH = (_count>0) ? (_count * (_cardH + _gapY) - _gapY) : 0;

    _active = false;

    // Scroll state
    _scrollY       = 0.0f;
    _scrollTarget  = 0.0f;
    _velY          = 0.0f;
    _dragging      = false;
    _touchWasDown  = false;
    _lastTy        = 0;

    // Press (button) state
    _pressCandidate = false;
    _pressedIndex   = -1;
    _pressStartX    = 0;
    _pressStartY    = 0;
    _pressStartMs   = 0;
    _pressX=0; _pressY=0; _pressW=0; _pressH=0; _scrollAtPressI=0;
  }

  // Inject router
  void attachRouter(IRouter* r){ _router = r; }

  // Masuk Menu (setelah full flush)
  void onEnter(){
    _active = true;
    _velY = 0.0f;
    _scrollTarget = _scrollY;
    paintTitleSlices();
    paintViewportSlices();
  }

  // Keluar Menu
  void onExit(){
    _active = false;
    _dragging = false;
    _touchWasDown = false;
    _velY = 0.0f;
    _pressCandidate = false;
    _pressedIndex   = -1;
    clearRipple(true);
  }

  // QS menutup saat sudah di Menu → re-render rapi
  void onQSClosed(){
    if (!_active) return;
    if (!rippleActive()){
      paintTitleSlices();
      paintViewportSlices();
    }
  }

  bool isActive() const { return _active; }

  void tick(uint32_t nowMs, float dt){
    if (!_active) return;

    // ===== Baca touch sekali =====
    int tx=0, ty=0;
    const bool touching = _os->readTouch1(tx,ty);

    // ===== Ripple dulu & gate input lain bila aktif =====
    const bool rippleConsumes = updateRipple(nowMs, touching, tx, ty);
    if (rippleConsumes){
      _touchWasDown = touching;
      return; // freeze page saat ripple aktif/anim
    }

    // ===== Gesture: Tap/Press vs Scroll di dalam viewport =====
    const int  vpTop = _viewportY0;
    const int  vpBot = _viewportY0 + _viewportH;
    const bool inViewport = touching && (ty >= vpTop && ty < vpBot);

    const float dts = (dt > 0.00001f) ? dt : (1.0f/240.0f);
    const float maxScroll = (float)max(0, _contentH - _viewportH);

    // START touch
    if (inViewport && !_touchWasDown){
      _dragging = false;         // belum memutuskan scroll
      _velY     = 0.0f;
      _lastTy   = ty;

      // Calon press? (pakai PAD agar mudah kena)
      int idx = hitCardIndexGlobalPad(tx, ty, HIT_PAD_PX);
      if (idx >= 0){
        _pressCandidate = true;
        _pressedIndex   = idx;
        _pressStartX    = tx;
        _pressStartY    = ty;
        _pressStartMs   = nowMs;

        // Simpan rect card saat press (dibekukan)
        _scrollAtPressI = (int)lroundf(_scrollY);
        cardRectGlobal(idx, _scrollAtPressI, _pressX, _pressY, _pressW, _pressH);
      } else {
        _pressCandidate = false;
        _pressedIndex   = -1;
      }
    }
    // MOVE while touch down
    else if (touching && _touchWasDown && inViewport){
      // Jika belum scroll, cek jarak gerak VERTIKAL -> decide scroll
      if (!_dragging){
        const int dy = ty - _pressStartY;
        if (abs(dy) > TAP_SLOP_Y){
          // masuk mode scroll → batalkan press highlight
          _dragging = true;
          _velY = 0.0f;
          _pressCandidate = false;
          _pressedIndex   = -1;
          // Reset baseline untuk scroll
          _lastTy = ty;
        } else {
          // Masih calon press: jika jari keluar dari card (dengan toleransi)
          if (_pressedIndex >= 0){
            if (!pointInRectPad(tx, ty, _pressX, _pressY, _pressW, _pressH, LEAVE_TOL_PX)){
              _pressCandidate = false;
              _pressedIndex   = -1;
            }
          }
        }
      }

      // Scroll aktif → update target/velocity
      if (_dragging){
        float dty = (float)(ty - _lastTy);
        _lastTy = ty;

        float intended = -dty;
        float newTarget = _scrollTarget + intended;
        if (newTarget < 0.0f || newTarget > maxScroll){
          intended *= 0.4f;
          newTarget = _scrollTarget + intended;
        }
        _scrollTarget = newTarget;

        float instVel = intended / dts;
        _velY = _velY*(1.0f - VEL_LPF_ALPHA) + instVel*VEL_LPF_ALPHA;
        _velY = clampf(_velY, -MAX_VEL, +MAX_VEL);
      }
    }
    // RELEASE
    else if (!touching && _touchWasDown){
      if (_dragging){
        _dragging = false; // inertia lanjut di bawah
      } else {
        // Bukan scroll → bila masih pressCandidate, check posisi akhir dg toleransi
        if (_pressCandidate && _pressedIndex >= 0){
          const bool ok = pointInRectPad(_lastTx, _lastTy, _pressX, _pressY, _pressW, _pressH, RELEASE_TOL_PX);
          if (ok) onCardPressed(_pressedIndex);
        }
      }
      // Reset press state setelah release
      _pressCandidate = false;
      _pressedIndex   = -1;
    }

    // Simpan posisi terakhir buat cek release tolerance
    if (touching){ _lastTx = tx; _lastTy = ty; }
    _touchWasDown = touching;

    // ===== Update posisi scroll =====
    if (_dragging){
      const float a = oneMinusExp(dts, TRACK_TAU);
      _scrollY += a * (_scrollTarget - _scrollY);
    } else if (_pressCandidate){
      // Freeze inertia saat ada kandidat press
      _velY = 0.0f;
      _scrollTarget = _scrollY;
      // (tanpa perubahan _scrollY)
    } else {
      // Inertia bebas
      _scrollY += _velY * dts;
      _velY *= expf(-FRICTION * dts);

      if (_scrollY < 0.0f){
        const float a = oneMinusExp(dts, EDGE_TAU);
        _scrollY += a * (0.0f - _scrollY);
        if (fabsf(_scrollY) < 0.01f) _scrollY = 0.0f;
        if (fabsf(_velY)   < 1.0f)   _velY   = 0.0f;
      } else if (_scrollY > maxScroll){
        const float a = oneMinusExp(dts, EDGE_TAU);
        _scrollY += a * (maxScroll - _scrollY);
        if (fabsf(_scrollY - maxScroll) < 0.01f) _scrollY = maxScroll;
        if (fabsf(_velY)               < 1.0f)   _velY   = 0.0f;
      }
      _scrollTarget = _scrollY;
    }

    // ===== Render (multi-slice) =====
    paintTitleSlices();
    paintViewportSlices();
  }

private:
  AppOS*   _os   = nullptr;
  TopBar*  _top  = nullptr;
  IRouter* _router = nullptr;

  bool _active = false;

  // ----- Title -----
  const char* _titleStr = "MENU";
  int _titleScale = 3;
  int _titleW = 0;
  int _titleX = 0;
  int _titlePadTop = 8;
  int _titleGlyphH = 0;
  int _titleYTop = 0;

  // ----- Viewport -----
  int   _viewportY0   = 0;
  int   _viewportH    = 0;
  int   _scrollableH  = 0;

  // Smooth scroll state
  float _scrollY       = 0.0f;
  float _scrollTarget  = 0.0f;
  float _velY          = 0.0f;
  bool  _dragging      = false;
  bool  _touchWasDown  = false;
  int   _lastTy        = 0;
  int   _lastTx        = 0;

  // ----- Cards -----
  static constexpr int MAX_ITEMS = 16;
  const char* _labels[MAX_ITEMS]{};
  int   _count   = 0;
  int   _cardPadX= 12;
  int   _cardR   = 14;
  int   _cardH   = 64;
  int   _gapY    = 10;
  int   _contentH= 0;

  // ====== Tuning ======
  static constexpr float VEL_LPF_ALPHA = 0.25f;
  static constexpr float MAX_VEL       = 3500.0f;
  static constexpr float TRACK_TAU     = 0.08f;   // s
  static constexpr float EDGE_TAU      = 0.12f;   // s
  static constexpr float FRICTION      = 3.5f;    // 1/s

  // ====== Tap/Press thresholds ======
  static constexpr int TAP_SLOP_Y     = 12; // ambang geser vertikal utk masuk scroll
  static constexpr int HIT_PAD_PX     = 10; // perluas hitbox card
  static constexpr int LEAVE_TOL_PX   = 12; // toleransi keluar area saat move
  static constexpr int RELEASE_TOL_PX = 12; // toleransi saat rilis

  // Press state
  bool      _pressCandidate = false;
  int       _pressedIndex   = -1;
  int       _pressStartX    = 0;
  int       _pressStartY    = 0;
  uint32_t  _pressStartMs   = 0;

  // Rect card saat press (dibekukan)
  int _pressX=0,_pressY=0,_pressW=0,_pressH=0,_scrollAtPressI=0;

  // ===== Utilities =====
  static inline float clampf(float v, float a, float b){ return (v<a)?a:((v>b)?b:v); }
  static inline float oneMinusExp(float dt, float tau){
    if (tau <= 0.00001f) return 1.0f;
    float x = -dt / tau;
    if (x > -1e-4f && x < 1e-4f) return -x;
    return 1.0f - expf(x);
  }
  int tileH() const {
    int mh = _os->blit.maxH();
    if (mh <= 0) mh = Theme::STRIP_H;
    return (mh > 0) ? mh : 80;
  }

  // Area list (global X)
  void listAreaG(int& listX, int& listW) const {
    listX = _cardPadX;
    listW = TFT_WIDTH - 2*_cardPadX;
  }

  // Global rect untuk card index (menggunakan scroll integer yang dipilih)
  void cardRectGlobal(int idx, int scrollI, int& x,int& y,int& w,int& h) const {
    int listX, listW; listAreaG(listX, listW);
    x = listX;
    y = _viewportY0 + (idx * (_cardH + _gapY)) - scrollI;
    w = listW;
    h = _cardH;
  }

  // Hit test card index pada koordinat global + padding
  int hitCardIndexGlobalPad(int tx, int ty, int pad) const {
    if (ty < _viewportY0 || ty >= _viewportY0 + _viewportH) return -1;
    const int scrollI = (int)lroundf(_scrollY);
    for (int i=0;i<_count;i++){
      int x,y,w,h; cardRectGlobal(i, scrollI, x,y,w,h);
      if (pointInRectPad(tx, ty, x,y,w,h, pad)) return i;
    }
    return -1;
  }

  static inline bool pointInRectPad(int px,int py,int x,int y,int w,int h,int pad){
    return (px >= x - pad) && (px < x + w + pad) && (py >= y - pad) && (py < y + h + pad);
  }

  // Klik card: untuk sekarang log saja
  void onCardPressed(int idx){
    if (idx < 0 || idx >= _count) return;
    if (!_router) return;

    // Urutan label di begin():
    // 0: Simple Chat
    // 1: Network
    // 2: Log Path
    // 3: Log Data
    // 4: Sensor MPU9250
    // 5: Sensor BMP280
    // 6: Sensor SHT30
    // 7: GNSS M10
    // 8: Clock DS3231
    // 9: Micro SD Card Reader
    // 10: Battery
    onExit(); // rapikan state sebelum pindah

    switch (idx){
      case 0:  _router->requestSimpleChat();    break;
      case 1:  _router->requestNetwork();       break;
      case 2:  _router->requestLogPath();       break;
      case 3:  _router->requestLogData();       break;
      case 4:  _router->requestSensorMPU9250(); break;
      case 5:  _router->requestSensorBMP280();  break;
      case 6:  _router->requestSensorSHT30();   break;
      case 7:  _router->requestGNSS_M10();      break;
      case 8:  _router->requestClockDS3231();   break;
      case 9:  _router->requestMicroSD();       break;
      case 10: _router->requestBattery();       break;
      case 11: _router->requestWifi();          break;
      default:
        Serial.print("[Menu] Pressed: "); Serial.println(_labels[idx]);
        _router->requestMenu(); // tetap di Menu untuk item yang belum diimplement
        break;
    }
    // commit akan dilakukan di loop() via router.pumpAfterOverlayClosed()
  }

  // ====== Drawing: Title & Viewport ======
  void paintTitleSlices(){
    const int yTop = Theme::BAR_H;
    const int hAll = (_viewportY0 - Theme::BAR_H);
    if (hAll <= 0) return;

    int th = tileH();
    int y  = yTop;
    int remain = hAll;
    while (remain > 0){
      int h = min(remain, th);
      int rx = 0, ry = y, rw = TFT_WIDTH, rh = h;

      int ow=rw, oh=rh;
      _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,ow,oh,
        [&](GfxRGB888& g,int Rrx,int Rry,int /*Rw*/,int /*Rh*/){
          // Clear black
          uint8_t* p = g.pix; size_t N = (size_t)g.w*g.h;
          for (size_t i=0;i<N;++i){ *p++=0; *p++=0; *p++=0; }

          // Draw "MENU" (cek visibilitas slice)
          const int yBase  = (_titleYTop - Rry) + _titleGlyphH;
          const int xLocal = (_titleX    - Rrx);
          if (yBase >= 0 && yBase <= g.h + _titleGlyphH){
            SimpleFont::drawTextStyled(
              g, xLocal, yBase, _titleStr,
              255,255,255, _titleScale, 1,
              SimpleFont::AlignLeft, -1, SimpleFont::Bold
            );
          }
        }
      );

      y += h;
      remain -= h;
    }
  }

  void paintViewportSlices(){
    if (_viewportH <= 0) return;

    const int visTopG = _viewportY0;
    const int visBotG = _viewportY0 + _viewportH;
    const int scrollI = (int)lroundf(_scrollY);

    int th = tileH();
    int y  = _viewportY0;
    int remain = _viewportH;

    while (remain > 0){
      int h = min(remain, th);
      int rx = 0, ry = y, rw = TFT_WIDTH, rh = h;

      int ow=rw, oh=rh;
      _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,ow,oh,
        [&](GfxRGB888& g,int Rrx,int Rry,int /*Rw*/,int /*Rh*/){
          // Clear black
          uint8_t* p = g.pix; size_t N = (size_t)g.w*g.h;
          for (size_t i=0;i<N;++i){ *p++=0; *p++=0; *p++=0; }

          // Card metrics (global)
          int listX, listW; listAreaG(listX, listW);
          const int txtScale  = 2;
          const int txtGlyphH = 7 * txtScale;

          // Loop items
          for (int i=0;i<_count;i++){
            const int itemTopG = _viewportY0 + (i * (_cardH + _gapY)) - scrollI;
            const int itemBotG = itemTopG + _cardH;

            // Clip ke tile & viewport
            if (itemBotG <= Rry || itemTopG >= (Rry + g.h)) continue;
            if (itemTopG >= visBotG || itemBotG <= visTopG) continue;

            // --- Latar kartu (rounded), di-clip ---
            int cardX = listX;
            int cardY = itemTopG;
            int cardHdraw = _cardH;
            if (cardY + cardHdraw > visBotG) cardHdraw = max(0, visBotG - cardY);
            if (cardY < visTopG){
              int cut = visTopG - cardY;
              cardY += cut;
              cardHdraw -= cut;
            }
            if (cardHdraw <= 0) continue;

            // Highlight bila sedang ditekan & jari masih di kartu tsb
            bool highlight = (_pressCandidate && _pressedIndex == i && _touchWasDown);
            uint8_t r = Theme::UI_R, ggC = Theme::UI_G, b = Theme::UI_B;
            if (highlight){
              auto br = [](uint8_t c)->uint8_t { int v=c+26; return (uint8_t)(v>255?255:v); };
              r = br(r); ggC = br(ggC); b = br(b);
            }

            const int lx = cardX - Rrx;
            const int ly = cardY - Rry;
            const int lw = listW;
            const int lh = cardHdraw;

            const int drawY0 = max(0, ly);
            const int drawY1 = min(g.h, ly + lh);
            for (int yLoc = drawY0; yLoc < drawY1; ++yLoc){
              const int yy = yLoc - ly;
              int inset=0;
              if (_cardR>0){
                if (yy < _cardR){
                  float dy = (float)(_cardR - 1 - yy);
                  float dx = sqrtf((float)_cardR*_cardR - dy*dy);
                  inset = (int)(_cardR - floorf(dx));
                }
                int y2 = (lh - 1 - yy);
                if (y2 < _cardR){
                  float dy = (float)(_cardR - 1 - y2);
                  float dx = sqrtf((float)_cardR*_cardR - dy*dy);
                  int inset2 = (int)(_cardR - floorf(dx));
                  if (inset2 > inset) inset = inset2;
                }
              }
              const int x0 = max(0, lx + inset);
              const int x1 = min(g.w, lx + lw - inset);
              if (x1 <= x0) continue;

              uint8_t* row = g.pix + ((size_t)yLoc*g.w + x0)*3;
              for (int x=x0; x<x1; ++x){ row[0]=r; row[1]=ggC; row[2]=b; row+=3; }
            }

            // --- Teks center (vertikal anchor ke _cardH) ---
            const int textX     = listX + 16;
            const int textTopG  = itemTopG + (_cardH - txtGlyphH)/2;
            const int textBotG  = textTopG + txtGlyphH;

            if (textTopG >= visTopG && textBotG <= visBotG){
              const int yBase   = (textTopG - Rry) + txtGlyphH;
              const int xLocal  = (textX   - Rrx);
              if (yBase >= 0 && yBase <= g.h + txtGlyphH){
                SimpleFont::drawTextStyled(
                  g, xLocal, yBase, _labels[i],
                  255,255,255, txtScale, 1,
                  SimpleFont::AlignLeft, -1, SimpleFont::Bold
                );
              }
            }
          }
        }
      );

      y      += h;
      remain -= h;
    }
  }

  // ====== RIPPLE (grafik) — anti-flicker via freeze page ======
  struct Rect { int x=0,y=0,w=0,h=0; };
  struct GBConfig {
    int   EDGE_ZONE        = 22;
    int   MAX_DRAG         = 120;
    int   MAX_BULGE        = 30;
    int   TRIGGER          = 60;   // ambang trigger Back
    int   SPREAD_Y         = 60;
    int   ANIM_MS          = 150;
    float ARROW_LEN_K      = 0.20f;
    int   ARROW_THICK_DIV  = 9;
    int   ARROW_MAX_LEN    = 22;
    int   ARROW_MARGIN     = 8;
    bool  bothEdges        = true;
  } _gb;

  bool     _ripActive=false, _ripAnim=false, _ripLeft=false;
  int      _ripStartX=0, _ripCY=0;
  float    _ripD=0.f, _ripFrom=0.f, _ripTo=0.f;
  uint32_t _ripT0=0;
  Rect     _ripPrev{}; bool _ripPrevValid=false;

  static inline float easeOut(float t){ return 1.0f - (1.0f - t)*(1.0f - t); }
  static inline float smooth01(float t){ t=t<0?0:(t>1?1:t); return t*t*(3-2*t); }
  static inline void clampRectToScreen(int& rx,int& ry,int& rw,int& rh){
    if (rx < 0){ rw += rx; rx = 0; }
    if (ry < 0){ rh += ry; ry = 0; }
    if (rx + rw > TFT_WIDTH){ rw = TFT_WIDTH - rx; }
    if (ry + rh > TFT_HEIGHT){ rh = TFT_HEIGHT - ry; }
    if (rw < 0) rw = 0;
    if (rh < 0) rh = 0;
  }
  static Rect vUnion(const Rect& a, const Rect& b){
    Rect u; u.x=a.x; u.w=a.w;
    const int top = min(a.y, b.y);
    const int bot = max(a.y + a.h, b.y + b.h);
    u.y=top; u.h=bot-top; return u;
  }
  inline void edgeRect(bool left, int cy, Rect& r) const {
    const int EDGE_W = _gb.MAX_BULGE + 5; // overscan dikit
    r.x = left ? 0 : TFT_WIDTH - (EDGE_W + 1);
    r.y = max(0, cy - _gb.SPREAD_Y);
    r.h = min(TFT_HEIGHT - r.y, 2 * _gb.SPREAD_Y + 1);
    r.w = EDGE_W + 1;
    clampRectToScreen(r.x, r.y, r.w, r.h);
  }
  inline bool rippleActive() const { return _ripActive || _ripAnim; }

  void paintRegionForRipple(GfxRGB888& g, int Rrx, int Rry, int Rw, int Rh){
    // clear
    uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
    for (size_t i=0;i<N;++i){ *p++=0; *p++=0; *p++=0; }

    // judul (bila area menutup di atas viewport)
    const int yBase  = (_titleYTop - Rry) + _titleGlyphH;
    const int xLocal = (_titleX    - Rrx);
    if (Rry < _viewportY0 && yBase >= 0 && yBase <= g.h + _titleGlyphH){
      SimpleFont::drawTextStyled(g, xLocal, yBase, _titleStr,
                                 255,255,255, _titleScale, 1,
                                 SimpleFont::AlignLeft, -1, SimpleFont::Bold);
    }

    // cards (pakai _scrollY yang beku saat ripple)
    const int visTopG = _viewportY0;
    const int visBotG = _viewportY0 + _viewportH;
    const int scrollI = (int)lroundf(_scrollY);

    int listX, listW; listAreaG(listX, listW);
    const int txtScale  = 2;
    const int txtGlyphH = 7 * txtScale;

    for (int i=0;i<_count;i++){
      const int itemTopG = _viewportY0 + (i * (_cardH + _gapY)) - scrollI;
      const int itemBotG = itemTopG + _cardH;
      if (itemBotG <= visTopG || itemTopG >= visBotG) continue;
      if (itemBotG <= Rry     || itemTopG >= (Rry + g.h)) continue;

      int cardX = listX;
      int cardY = itemTopG;

      int cardHdraw = _cardH;
      if (cardY + cardHdraw > visBotG) cardHdraw = max(0, visBotG - cardY);
      if (cardY < visTopG){ int cut = visTopG - cardY; cardY += cut; cardHdraw -= cut; }
      if (cardHdraw <= 0) continue;

      uint8_t r=Theme::UI_R, ggC=Theme::UI_G, b=Theme::UI_B;

      const int lx = cardX - Rrx;
      const int ly = cardY - Rry;
      const int lw = listW;
      const int lh = cardHdraw;

      const int drawY0 = max(0, ly);
      const int drawY1 = min(g.h, ly + lh);
      for (int yLoc = drawY0; yLoc < drawY1; ++yLoc){
        const int yy = yLoc - ly;
        int inset=0;
        if (_cardR>0){
          if (yy < _cardR){
            float dy = (float)(_cardR - 1 - yy);
            float dx = sqrtf((float)_cardR*_cardR - dy*dy);
            inset = (int)(_cardR - floorf(dx));
          }
          int y2 = (lh - 1 - yy);
          if (y2 < _cardR){
            float dy = (float)(_cardR - 1 - y2);
            float dx = sqrtf((float)_cardR*_cardR - dy*dy);
            int inset2 = (int)(_cardR - floorf(dx));
            if (inset2 > inset) inset = inset2;
          }
        }
        const int x0 = max(0, lx + inset);
        const int x1 = min(g.w, lx + lw - inset);
        if (x1 <= x0) continue;

        uint8_t* row = g.pix + ((size_t)yLoc*g.w + x0)*3;
        for (int x=x0; x<x1; ++x){ row[0]=r; row[1]=ggC; row[2]=b; row+=3; }
      }

      const int textX     = listX + 16;
      const int textTopG  = itemTopG + (_cardH - txtGlyphH)/2;
      const int textBotG  = textTopG + txtGlyphH;
      if (textTopG >= visTopG && textBotG <= visBotG){
        const int yBase2 = (textTopG - Rry) + txtGlyphH;
        const int xLocal2= (textX   - Rrx);
        if (yBase2 >= 0 && yBase2 <= g.h + txtGlyphH){
          SimpleFont::drawTextStyled(
            g, xLocal2, yBase2, _labels[i],
            255,255,255, txtScale, 1,
            SimpleFont::AlignLeft, -1, SimpleFont::Bold
          );
        }
      }
    }
  }

  static void drawArrowLogo(GfxRGB888& g, int cx, int cy, int armLen, int thick, bool faceRight){
    armLen = max(5, armLen);
    thick  = max(2, thick);
    auto put = [&](int x,int y){ g.fillRect(x - thick/2, y - thick/2, thick, thick, 255,255,255); };
    if (faceRight){
      for (int i=0; i<armLen; ++i){ put(cx - i, cy - i); put(cx - i, cy + i); }
    } else {
      for (int i=0; i<armLen; ++i){ put(cx + i, cy - i); put(cx + i, cy + i); }
    }
  }

  void drawRippleFull(bool left, int cy, float d){
    Rect r; edgeRect(left, cy, r);
    if (r.w<=0 || r.h<=0) return;

    int rx=r.x, ry=r.y, rw=r.w, rh=r.h;
    _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,rw,rh,
      [&](GfxRGB888& g,int Rrx,int Rry,int Rw,int Rh){
        // restore backdrop (deterministik, page frozen)
        paintRegionForRipple(g, Rrx, Rry, Rw, Rh);

        const uint8_t cR=65, cG=65, cB=70;
        const float k = easeOut(min(1.f, d / (float)_gb.MAX_DRAG));
        const int bulge = (int)(k * _gb.MAX_BULGE);
        const int localCy = cy - Rry;

        // ripple
        for (int y=0; y<Rh; ++y){
          const int dy = abs(y - localCy);
          const float fall = 1.f - (float)dy / (float)_gb.SPREAD_Y;
          if (fall <= 0.f) continue;
          const int w = (int)(bulge * smooth01(fall));
          if (w <= 0) continue;
          if (left) g.fillRect(0, y, w, 1, cR,cG,cB);
          else      g.fillRect(Rw - w, y, w, 1, cR,cG,cB);
        }

        // cap line
        const int y0   = max(0, localCy - _gb.SPREAD_Y/2);
        const int y1   = min(Rh-1, localCy + _gb.SPREAD_Y/2);
        const int capH = y1 - y0 + 1;
        if (capH > 0){
          if (left) g.fillRect(0,    y0, 1, capH, cR,cG,cB);
          else      g.fillRect(Rw-1, y0, 1, capH, cR,cG,cB);
        }

        // chevron
        if (bulge >= (int)(0.40f * _gb.MAX_BULGE)) {
          const int wC = bulge;
          const int cx = left
            ? min(wC/2 + _gb.ARROW_MARGIN, Rw - 8)
            : max(Rw - (wC/2 + _gb.ARROW_MARGIN), 8);
          const int armLen = max(6, min((int)(wC * _gb.ARROW_LEN_K), _gb.ARROW_MAX_LEN));
          const int thick  = max(2, wC / _gb.ARROW_THICK_DIV);
          drawArrowLogo(g, cx, localCy, armLen, thick, /*faceRight*/ left);
        }
      }
    );
  }

  void restoreRippleRect(const Rect& r){
    if (r.w<=0 || r.h<=0) return;
    int rx=r.x, ry=r.y, rw=r.w, rh=r.h;
    _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,rw,rh,
      [&](GfxRGB888& g,int Rrx,int Rry,int Rw,int Rh){
        paintRegionForRipple(g, Rrx, Rry, Rw, Rh);
      }
    );
  }

  void clearRipple(bool forceAll=false){
    if (_ripPrevValid) restoreRippleRect(_ripPrev);
    _ripPrevValid = false;
    if (forceAll){
      _ripActive=_ripAnim=false;
      _ripD=0.f;
    }
  }

  // return true → ripple consume & freeze page
  bool updateRipple(uint32_t nowMs, bool touching, int tx, int ty){
    // START
    if (!_ripActive && !_ripAnim && touching){
      const bool hitLeft  = _gb.bothEdges && (tx <= _gb.EDGE_ZONE);
      const bool hitRight = _gb.bothEdges && (tx >= TFT_WIDTH - _gb.EDGE_ZONE);
      if (hitLeft || hitRight){
        _ripActive = true;
        _ripLeft   = hitLeft;
        _ripStartX = tx;
        // kunci Y, jangan menimpa topbar
        const int minY = Theme::BAR_H + 6;
        const int maxY = TFT_HEIGHT - 7;
        _ripCY = (ty < minY) ? minY : (ty > maxY ? maxY : ty);
        _ripD   = 0.f;
        _ripPrevValid = false;
      }
    }

    // DRAG (Y terkunci)
    if (_ripActive && touching){
      float raw = _ripLeft ? (tx - _ripStartX) : (_ripStartX - tx);
      raw = max(0.f, min((float)_gb.MAX_DRAG, raw));
      _ripD = 0.7f * _ripD + 0.3f * raw;

      Rect nowR; edgeRect(_ripLeft, _ripCY, nowR);
      Rect uni = _ripPrevValid ? vUnion(_ripPrev, nowR) : nowR;

      restoreRippleRect(uni);
      drawRippleFull(_ripLeft, _ripCY, _ripD);

      _ripPrev  = nowR;
      _ripPrevValid = true;
      return true;
    }

    // RELEASE
    if (_ripActive && !touching){
      const bool shouldBack = (_ripD >= (float)_gb.TRIGGER);
      _ripActive = false;

      if (shouldBack){
        clearRipple(true);
        goBackToHome();
        return true;
      }

      // tidak trigger → anim balik (grafik saja)
      _ripAnim     = true;
      _ripT0       = nowMs;
      _ripFrom     = _ripD;
      _ripTo       = 0.f;
    }

    // ANIM
    if (_ripAnim){
      uint32_t ms = nowMs - _ripT0;
      if (ms >= (uint32_t)_gb.ANIM_MS){
        clearRipple(true);
        _ripAnim = false;
      }else{
        float u = (float)ms / (float)_gb.ANIM_MS;
        float d = _ripFrom + (_ripTo - _ripFrom) * easeOut(u);
        Rect nowR; edgeRect(_ripLeft, _ripCY, nowR);
        Rect uni = _ripPrevValid ? vUnion(_ripPrev, nowR) : nowR;
        restoreRippleRect(uni);
        drawRippleFull(_ripLeft, _ripCY, d);
        _ripPrev = nowR; _ripPrevValid = true;
      }
      return true; // selama anim, gate input lain
    }

    return false; // ripple idle
  }

  // Back action helper (dipakai ripple trigger)
  void goBackToHome(){
    clearRipple(true);
    onExit();
    if (_router){
      _router->requestHome(); // defer; main loop will commit
    }
  }
};
