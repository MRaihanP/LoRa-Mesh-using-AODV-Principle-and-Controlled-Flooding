#pragma once
#include <math.h>
#include "OS.h"

// Popup overlay spesifik untuk tombol Power + SOS di dalam PopupWindow:
//
// - Pakai PopupWindow (multi-tile safe, auto layout).
// - Card rounded gelap di tengah.
// - Button SOS kiri (sosIcon bawaan library).
// - Button Power kanan (ikon power di lingkaran abu-abu terang).
// - Tap di luar card → close().
// - Tap Power → callback onPowerPressed().
// - Tap SOS  → callback onSosPressed() + sinkron dengan SOS di QS (via bindQSSos).
//
class PowerPopupOverlay {
public:
  using Callback0 = void(*)();

  void setSOS(bool on){
    _sosProps.on = on;
    if (_qsSos) _qsSos->on = on;
    PinsAndConfig::Power::sosActive = on;
    if (_onSosPressed) _onSosPressed();
  }

  void begin(AppOS* os) {
    _os = os;
    if (!_os) return;

    // Siapkan PopupWindow-nya (pakai ui, bg, blit dari OS)
    _popup.begin(&_os->ui, &_os->bg, &_os->blit);

    PopupWindow::Style st;
    st.marginX      = 30;
    st.marginY      = 30;
    st.bgR          = 24;
    st.bgG          = 26;
    st.bgB          = 34;
    st.maxHFracNum  = 3;   // ~60% tinggi layar
    st.maxHFracDen  = 5;
    st.minW         = 120;
    st.minH         = 120;
    st.radiusMin    = 12;
    st.radiusMax    = 24;
    _popup.setStyle(st);

    _popup.setContentPainter(&PowerPopupOverlay::paintContentThunk, this);
    _popup.recomputeLayout();

    recomputeButtons();
  }

  void open() {
    if (_popup.isOpen()) return;
    _popup.open();
    _justClosed  = false;
    _prevTouch   = false;
    _armedForTap = false;   // butuh 1 frame "no touch" dulu
  }

  void close() {
    if (!_popup.isOpen()) return;
    _popup.close();
    _justClosed  = true;
    _prevTouch   = false;
    _armedForTap = false;
  }

  bool isOpen() const { return _popup.isOpen(); }

  bool consumeJustClosed() {
    bool v = _justClosed;
    _justClosed = false;
    return v;
  }

  void onPowerPressed(Callback0 cb) { _onPowerPressed = cb; }
  void onSosPressed(Callback0 cb)   { _onSosPressed   = cb; }

  // Atur manual skala lingkaran dan icon power.
  // circleScale: 1.0 = default, >1 lebih besar, <1 lebih kecil.
  // iconScale  : 0.5 default, >1 icon mendekati tepi lingkaran, <1 lebih kecil.
  void setPowerButtonVisual(float circleScale, float iconScale) {
    _pwrCircleScale = circleScale;
    _pwrIconScale   = iconScale;
    recomputeButtons();
  }

  // Atur skala lingkaran SOS terpisah dari tombol Power
  // sosScale: 1.0 = sama dengan power, >1 lebih besar, <1 lebih kecil.
  void setSosButtonScale(float sosScale) {
    _sosCircleScale = sosScale;
    recomputeButtons();
  }

  // Bind ke SosProps milik QS (hanya sinkron field 'on').
  // QS tetap pakai cx/cy/outerR sendiri untuk icon di panel QS.
  void bindQSSos(SosProps* qsSos) {
    _qsSos = qsSos;
  }

  // Dipanggil dari loop utama ketika popup open.
  void update(uint32_t /*now*/, float /*dt*/) {
    if (!_popup.isOpen()) return;
    _popup.update();
  }

  // Routing input dari loop utama
  void handleTouch(bool touching, int tx, int ty) {
    if (!_popup.isOpen()) {
      _prevTouch = touching;
      return;
    }

    // Butuh 1 frame no-touch setelah open agar tidak auto-trigger
    if (!_armedForTap) {
      if (!touching) _armedForTap = true;
      _prevTouch = touching;
      return;
    }

    // --- TOUCH DOWN ---
    if (touching && !_prevTouch) {
      const bool insideCard = _popup.contains(tx, ty);

      if (!insideCard) {
        close();
        _prevTouch = touching;
        return;
      }

      // Hit-test tombol
      const bool inSos = pointInCircle(tx, ty, _btnSosCx, _btnSosCy, _sosProps.outerR);
      const bool inPwr = pointInCircle(tx, ty, _btnPwrCx, _btnPwrCy, _btnRadius);

      if (inPwr) {
        if (_onPowerPressed) _onPowerPressed();
      }

      if (inSos) {
        _sosPressed   = true;
        _sosInside    = true;
        _sosHoldFired = false;
        _sosPressMs   = millis();

        // TAP → langsung ON (kalau belum ON)
        // OFF tidak boleh lewat tap, hanya hold 3 detik.
        if (!_sosProps.on) {
          setSOS(true);
        }
      }

      _prevTouch = touching;
      return;
    }

    // --- TOUCH MOVE / HOLD ---
    if (touching && _prevTouch) {
      if (_sosPressed) {
        _sosInside = pointInCircle(tx, ty, _btnSosCx, _btnSosCy, _sosProps.outerR);

        // HOLD 3 detik → OFF (hanya kalau sedang ON & jari masih di dalam ikon)
        if (_sosProps.on && _sosInside && !_sosHoldFired) {
          if ((uint32_t)(millis() - _sosPressMs) >= 3000u) {
            _sosHoldFired = true;
            setSOS(false);
          }
        }
      }

      _prevTouch = touching;
      return;
    }

    // --- TOUCH UP ---
    if (!touching && _prevTouch) {
      _sosPressed   = false;
      _sosInside    = false;
      _sosHoldFired = false;
      _prevTouch    = touching;
      return;
    }

    _prevTouch = touching;
  }

private:
  // ==== helper kecil ====
  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }

  static bool pointInCircle(int px, int py, int cx, int cy, int r) {
    int dx = px - cx;
    int dy = py - cy;
    return (dx*dx + dy*dy) <= r*r;
  }

  // Callback untuk PopupWindow → delegasi ke method instance
  static void paintContentThunk(GfxRGB888& g,
                                int Rrx, int Rry,
                                const PopupWindow::Rect& card,
                                int radius,
                                void* user)
  {
    (void)radius;
    auto* self = static_cast<PowerPopupOverlay*>(user);
    if (!self) return;
    self->drawContent(g, Rrx, Rry, card);
  }

  // Hitung posisi & ukuran tombol berdasarkan rect popup
  void recomputeButtons() {
    const auto& card = _popup.cardRect();

    // Radius dasar berdasarkan ukuran card
    int minSide  = (card.w < card.h) ? card.w : card.h;
    int autoBase = minSide / 8;
    if (autoBase < 22) autoBase = 22;
    if (autoBase > 40) autoBase = 40;

    // Radius lingkaran background Power = autoBase * scale
    int rCircle = (int)floorf(autoBase * _pwrCircleScale);
    if (rCircle < 8)  rCircle = 8;
    _btnRadius = rCircle;

    // Posisi pusat tombol kiri/kanan
    _btnSosCx = card.x + card.w / 4;
    _btnPwrCx = card.x + (card.w * 3) / 4;
    _btnSosCy = _btnPwrCy = card.y + card.h / 2;

    // SosProps (global coords + outerR, dengan skala terpisah)
    _sosProps.cx = _btnSosCx;
    _sosProps.cy = _btnSosCy;

    int sosR = (int)floorf(_btnRadius * _sosCircleScale);
    if (sosR < 6) sosR = 6;
    _sosProps.outerR = sosR;
    // flag 'on' jangan di-reset di sini; dibiarkan mengikuti QS/binding.
  }

  // Gambar isi di dalam window (SOS + Power)
  void drawContent(GfxRGB888& g, int Rrx, int Rry,
                   const PopupWindow::Rect& /*card*/)
  {
    // Sinkron flag 'on' dari QS → popup (kalau ter-bind)
    if (_qsSos) {
      _sosProps.on = _qsSos->on;
      PinsAndConfig::Power::sosActive = _sosProps.on;
    }

    // 1) SOS kiri (library sudah multi-tile safe)
    sosIcon(g, Rrx, Rry, _sosProps);

    // 2) Tombol Power kanan

    // 2a) Background lingkaran abu-abu (diameternya = _btnRadius)
    const uint8_t bgR = 100;
    const uint8_t bgG = 100;
    const uint8_t bgB = 100;
    fillCircleClipped(g, Rrx, Rry,
                      _btnPwrCx, _btnPwrCy, _btnRadius,
                      bgR, bgG, bgB);

    // 2b) Icon power (glyph) putih di atas background
    const uint8_t glyphR = 255;
    const uint8_t glyphG = 255;
    const uint8_t glyphB = 255;

    // Radius glyph = skala dari radius lingkaran background
    int glyphRadius = (int)floorf(_btnRadius * _pwrIconScale);
    if (glyphRadius < 6) glyphRadius = 6;

    drawPowerIcon(g, Rrx, Rry,
                  _btnPwrCx, _btnPwrCy, glyphRadius,
                  glyphR, glyphG, glyphB);
  }

  // ==== PRIMITIF RENDERING – multi-tile safe ====

  // Lingkaran solid clip-aware
  static void fillCircleClipped(GfxRGB888& g, int Rrx, int Rry,
                                int cx, int cy, int r,
                                uint8_t R,uint8_t G,uint8_t B)
  {
    if (r <= 0) return;

    const int tileTop  = Rry;
    const int tileBot  = Rry + g.h - 1;

    int y0 = cy - r;
    int y1 = cy + r;

    if (y1 < tileTop || y0 > tileBot) return;
    if (y0 < tileTop) y0 = tileTop;
    if (y1 > tileBot) y1 = tileBot;

    const int r2 = r * r;

    for (int gy = y0; gy <= y1; ++gy) {
      int dy = gy - cy;
      int dx = (int)floorf(sqrtf((float)r2 - (float)dy*dy));
      int x0 = cx - dx;
      int x1 = cx + dx;

      int tileLeft  = Rrx;
      int tileRight = Rrx + g.w - 1;
      if (x1 < tileLeft || x0 > tileRight) continue;
      if (x0 < tileLeft)  x0 = tileLeft;
      if (x1 > tileRight) x1 = tileRight;

      const int yLoc = gy - Rry;
      const int lx0  = x0 - Rrx;
      const int lx1  = x1 - Rrx;

      uint8_t* row = g.pix + ((size_t)yLoc*g.w + lx0)*3;
      for (int gx = lx0; gx <= lx1; ++gx) {
        row[0] = R; row[1] = G; row[2] = B;
        row += 3;
      }
    }
  }

  // Rect solid clip-aware (dipakai untuk batang power)
  static void fillRectClipped(GfxRGB888& g, int Rrx,int Rry,
                              int x,int y,int w,int h,
                              uint8_t R,uint8_t G,uint8_t B)
  {
    int x0 = imax(x,     Rrx);
    int y0 = imax(y,     Rry);
    int x1 = imin(x+w,   Rrx+g.w);
    int y1 = imin(y+h,   Rry+g.h);
    if (x1 <= x0 || y1 <= y0) return;

    for (int gy = y0; gy < y1; ++gy) {
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx = x0; gx < x1; ++gx) {
        row[0] = R; row[1] = G; row[2] = B;
        row += 3;
      }
    }
  }

  // Ikon Power: cincin + gap atas + batang
  static void drawPowerIcon(GfxRGB888& g, int Rrx, int Rry,
                            int cx, int cy, int r,
                            uint8_t R, uint8_t G, uint8_t B)
  {
    if (r <= 0) return;

    const int tileLeft  = Rrx;
    const int tileTop   = Rry;
    const int tileRight = Rrx + g.w - 1;
    const int tileBot   = Rry + g.h - 1;

    const float ro  = (float)r;
    const float ri  = ro * 0.8f;
    const float ro2 = ro * ro;
    const float ri2 = ri * ri;

    int y0 = cy - (int)ro;
    int y1 = cy + (int)ro;

    if (y1 < tileTop || y0 > tileBot) return;
    if (y0 < tileTop) y0 = tileTop;
    if (y1 > tileBot) y1 = tileBot;

    for (int gy = y0; gy <= y1; ++gy) {
      for (int gx = tileLeft; gx <= tileRight; ++gx) {
        int dx = gx - cx;
        int dy = gy - cy;
        float d2 = (float)dx*dx + (float)dy*dy;
        if (d2 > ro2 || d2 < ri2) continue;

        // gap atas
        if (dy < 0 && abs(dx) < (int)(ro * 0.35f)) continue;

        const int lx = gx - Rrx;
        const int ly = gy - Rry;
        uint8_t* p = g.pix + ((size_t)ly*g.w + lx)*3;
        p[0] = R; p[1] = G; p[2] = B;
      }
    }

    // batang vertikal
    int barW = (int)(ro * 0.35f);
    if (barW < 3) barW = 3;
    int barH = (int)(ro * 1.0f);
    if (barH < 8) barH = 8;

    int barX = cx - barW/2;
    int barY = cy - (int)ro - barH/3;

    fillRectClipped(g, Rrx,Rry, barX, barY, barW, barH, R,G,B);
  }

  AppOS* _os = nullptr;

  PopupWindow _popup;

  bool _justClosed  = false;
  bool _prevTouch   = false;
  bool _armedForTap = false;

  // ===== SOS gesture state (tap ON, hold 3s to OFF) =====
  bool     _sosPressed     = false;
  bool     _sosInside      = false;
  bool     _sosHoldFired   = false;
  uint32_t _sosPressMs     = 0;

  int _btnRadius = 0;
  int _btnSosCx  = 0;
  int _btnSosCy  = 0;
  int _btnPwrCx  = 0;
  int _btnPwrCy  = 0;

  // Skala visual (boleh kamu ubah default-nya)
  float _pwrCircleScale = 1.2f;   // skala radius bg power
  float _pwrIconScale   = 0.50f;  // skala radius icon power
  float _sosCircleScale = 0.75f;  // skala radius lingkaran SOS

  SosProps  _sosProps{};   // untuk gambar SOS di popup
  SosProps* _qsSos   = nullptr;   // pointer ke SosProps di QS (hanya pakai field 'on')

  Callback0 _onPowerPressed = nullptr;
  Callback0 _onSosPressed   = nullptr;
};
