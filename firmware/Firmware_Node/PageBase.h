#pragma once
#include <RP_TFTDisplay.h>
#include "OS.h"
#include "pins_and_config.h"
#include "IRouter.h"
#include <math.h>

class PageBase {
public:
  void begin(AppOS* os, TopBar* top){
    _os = os; _top = top;
    // Default title layout
    setTitle("PAGE");
  }

  void attachRouter(IRouter* r){ _router = r; }

  void setTitle(const char* t){
    _titleStr   = t ? t : "PAGE";
    _titleScale = 3;
    _titleW     = SimpleFont::textWidth(_titleStr, _titleScale, 1);
    _titlePadTop= 8;
    _titleGlyphH= 7 * _titleScale;
    _titleX     = (TFT_WIDTH - _titleW)/2;
    _titleYTop  = Theme::BAR_H + _titlePadTop;

    _viewportY0 = _titleYTop + _titleGlyphH + 12;
    _viewportH  = max(0, TFT_HEIGHT - _viewportY0);
  }

  // Router akan memanggil saat pindah ke page ini (setelah flush)
  virtual void onEnter(){
    _active = true;
    paintTitleSlices();
    paintContentSlices();
  }

  // Router / ripple-back memanggil saat keluar
  virtual void onExit(){
    _active = false;
    _touchWasDown = false;
    clearRipple(true);
  }

  // QS tutup → rapikan tampilan
  virtual void onQSClosed(){
    if (!_active || rippleActive()) return;
    paintTitleSlices();
    paintContentSlices();
  }

  bool isActive() const { return _active; }

  // Main loop
  void tick(uint32_t nowMs, float /*dt*/){
    if (!_active) return;

    int tx=0, ty=0; const bool touching = _os->readTouch1(tx,ty);

    // Ripple edge-back consume lebih dulu
    if (updateRipple(nowMs, touching, tx, ty)){
      _touchWasDown = touching;
      return;
    }

    // Forward input ke konten (opsional override)
    handleContentInput(touching, tx, ty);

    // Render slices
    paintTitleSlices();
    paintContentSlices();

    _touchWasDown = touching;
  }

protected:
  AppOS*   _os   = nullptr;
  TopBar*  _top  = nullptr;
  IRouter* _router = nullptr;

  // ----- Title layout -----
  const char* _titleStr = "PAGE";
  int _titleScale=3, _titleW=0, _titleX=0;
  int _titlePadTop=8, _titleGlyphH=0, _titleYTop=0;

  // ----- Content area (di bawah title) -----
  int _viewportY0=0, _viewportH=0;

  bool _active=false, _touchWasDown=false;

  enum BackDest : uint8_t { BackToHome, BackToMenu };
  BackDest _backDest = BackToMenu;

  // ==== Hooks konten ====
  // Catatan: menggambar latar hitam & judul sudah ditangani PageBase.
  // Turunan tinggal gambar isi konten di area [_viewportY0 .. _viewportY0+_viewportH)
  virtual void paintContentTile(GfxRGB888& /*g*/, int /*Rrx*/, int /*Rry*/){}
  virtual void handleContentInput(bool /*touching*/, int /*tx*/, int /*ty*/){}

  // ==== Slicer helper ====
  int tileH() const {
    int mh = _os->blit.maxH();
    if (mh <= 0) mh = Theme::STRIP_H;
    return (mh > 0) ? mh : 80;
  }

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

          // Title center
          const int yBase  = (_titleYTop - Rry) + _titleGlyphH;
          const int xLocal = (_titleX     - Rrx);
          if (yBase >= 0 && yBase <= g.h + _titleGlyphH){
            SimpleFont::drawTextStyled(g, xLocal, yBase, _titleStr,
              255,255,255, _titleScale, 1, SimpleFont::AlignLeft, -1, SimpleFont::Bold);
          }
        }
      );
      y += h; remain -= h;
    }
  }

  void paintContentSlices(){
    if (_viewportH <= 0) return;

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
          // Content hook
          paintContentTile(g, Rrx, Rry);
        }
      );
      y += h; remain -= h;
    }
  }

  // ====== Ripple Back (copy behaviour MenuPage) ======
  struct Rect { int x=0,y=0,w=0,h=0; };
  struct GBConfig {
    int   EDGE_ZONE        = 22;
    int   MAX_DRAG         = 120;
    int   MAX_BULGE        = 30;
    int   TRIGGER          = 60;
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
    if (rw < 0) rw = 0; if (rh < 0) rh = 0;
  }
  static Rect vUnion(const Rect& a, const Rect& b){
    Rect u; u.x=a.x; u.w=a.w;
    const int top = min(a.y, b.y);
    const int bot = max(a.y + a.h, b.y + b.h);
    u.y=top; u.h=bot-top; return u;
  }
  inline bool rippleActive() const { return _ripActive || _ripAnim; }

  inline void edgeRect(bool left, int cy, Rect& r) const {
    const int EDGE_W = _gb.MAX_BULGE + 5;
    r.x = left ? 0 : TFT_WIDTH - (EDGE_W + 1);
    r.y = max(0, cy - _gb.SPREAD_Y);
    r.h = min(TFT_HEIGHT - r.y, 2 * _gb.SPREAD_Y + 1);
    r.w = EDGE_W + 1;
    clampRectToScreen(r.x, r.y, r.w, r.h);
  }

  // Pulihkan area belakang ripple (judul + konten; page frozen)
  void paintRegionForRipple(GfxRGB888& g, int Rrx, int Rry, int Rw, int Rh){
    // clear
    uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
    for (size_t i=0;i<N;++i){ *p++=0; *p++=0; *p++=0; }

    // judul (bila tile di atas viewport)
    const int yBase  = (_titleYTop - Rry) + _titleGlyphH;
    const int xLocal = (_titleX     - Rrx);
    if (Rry < _viewportY0 && yBase >= 0 && yBase <= g.h + _titleGlyphH){
      SimpleFont::drawTextStyled(g, xLocal, yBase, _titleStr,
                                 255,255,255, _titleScale, 1,
                                 SimpleFont::AlignLeft, -1, SimpleFont::Bold);
    }
    // content hook (gunakan tile hook standar)
    paintContentTile(g, Rrx, Rry);
  }

  static void drawArrowLogo(GfxRGB888& g, int cx, int cy, int armLen, int thick, bool faceRight){
    armLen = max(5, armLen); thick = max(2, thick);
    auto put = [&](int x,int y){ g.fillRect(x - thick/2, y - thick/2, thick, thick, 255,255,255); };
    if (faceRight){ for (int i=0;i<armLen;++i){ put(cx - i, cy - i); put(cx - i, cy + i); } }
    else          { for (int i=0;i<armLen;++i){ put(cx + i, cy - i); put(cx + i, cy + i); } }
  }

  void drawRippleFull(bool left, int cy, float d){
    Rect r; edgeRect(left, cy, r);
    if (r.w<=0 || r.h<=0) return;
    int rx=r.x, ry=r.y, rw=r.w, rh=r.h;
    _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,rw,rh,
      [&](GfxRGB888& g,int Rrx,int Rry,int Rw,int Rh){
        paintRegionForRipple(g, Rrx, Rry, Rw, Rh); // restore backdrop

        const uint8_t cR=65, cG=65, cB=70;
        const float k = easeOut(min(1.f, d / (float)_gb.MAX_DRAG));
        const int bulge = (int)(k * _gb.MAX_BULGE);
        const int localCy = cy - Rry;

        for (int y=0; y<Rh; ++y){
          const int dy = abs(y - localCy);
          const float fall = 1.f - (float)dy / (float)_gb.SPREAD_Y;
          if (fall <= 0.f) continue;
          const int w = (int)(bulge * smooth01(fall));
          if (w <= 0) continue;
          if (left) g.fillRect(0, y, w, 1, cR,cG,cB);
          else      g.fillRect(Rw - w, y, w, 1, cR,cG,cB);
        }

        const int y0   = max(0, localCy - _gb.SPREAD_Y/2);
        const int y1   = min(Rh-1, localCy + _gb.SPREAD_Y/2);
        const int capH = y1 - y0 + 1;
        if (capH > 0){
          if (left) g.fillRect(0,    y0, 1, capH, cR,cG,cB);
          else      g.fillRect(Rw-1, y0, 1, capH, cR,cG,cB);
        }

        if (bulge >= (int)(0.40f * _gb.MAX_BULGE)) {
          const int wC = bulge;
          const int cx = left ? min(wC/2 + _gb.ARROW_MARGIN, Rw - 8)
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
    if (forceAll){ _ripActive=_ripAnim=false; _ripD=0.f; }
  }

  // return true → ripple consume & freeze page
  bool updateRipple(uint32_t nowMs, bool touching, int tx, int ty){
    // START
    if (!_ripActive && !_ripAnim && touching){
      const bool hitLeft  = _gb.bothEdges && (tx <= _gb.EDGE_ZONE);
      const bool hitRight = _gb.bothEdges && (tx >= TFT_WIDTH - _gb.EDGE_ZONE);
      if (hitLeft || hitRight){
        _ripActive = true; _ripLeft = hitLeft; _ripStartX = tx;
        const int minY = Theme::BAR_H + 6, maxY = TFT_HEIGHT - 7;
        _ripCY = (ty < minY) ? minY : (ty > maxY ? maxY : ty);
        _ripD  = 0.f; _ripPrevValid = false;
      }
    }

    // DRAG
    if (_ripActive && touching){
      float raw = _ripLeft ? (tx - _ripStartX) : (_ripStartX - tx);
      raw = max(0.f, min((float)_gb.MAX_DRAG, raw));
      _ripD = 0.7f * _ripD + 0.3f * raw;

      Rect nowR; edgeRect(_ripLeft, _ripCY, nowR);
      Rect uni = _ripPrevValid ? vUnion(_ripPrev, nowR) : nowR;
      restoreRippleRect(uni);
      drawRippleFull(_ripLeft, _ripCY, _ripD);
      _ripPrev = nowR; _ripPrevValid = true;
      return true;
    }

    // RELEASE
    if (_ripActive && !touching){
      const bool shouldBack = (_ripD >= (float)_gb.TRIGGER);
      _ripActive = false;
      if (shouldBack){
        clearRipple(true);
        onExit();
        if (_router){
          if (_backDest == BackToMenu) _router->requestMenu();
          else                         _router->requestHome();
        }
        return true;
      }
      // anim balik
      _ripAnim = true; _ripT0 = nowMs; _ripFrom = _ripD; _ripTo = 0.f;
    }

    // ANIM
    if (_ripAnim){
      uint32_t ms = nowMs - _ripT0;
      if (ms >= (uint32_t)_gb.ANIM_MS){ clearRipple(true); _ripAnim = false; }
      else{
        float u = (float)ms / (float)_gb.ANIM_MS;
        float d = _ripFrom + (_ripTo - _ripFrom) * easeOut(u);
        Rect nowR; edgeRect(_ripLeft, _ripCY, nowR);
        Rect uni = _ripPrevValid ? vUnion(_ripPrev, nowR) : nowR;
        restoreRippleRect(uni);
        drawRippleFull(_ripLeft, _ripCY, d);
        _ripPrev = nowR; _ripPrevValid = true;
      }
      return true;
    }
    return false;
  }
};
