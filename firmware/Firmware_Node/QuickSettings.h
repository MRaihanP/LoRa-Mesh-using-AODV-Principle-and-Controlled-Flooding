#pragma once

#include <RP_TFTDisplay.h>
#include "OS.h"
#include "pins_and_config.h"
#include "IRouter.h"

// Catatan: SosProps/BleProps/NetworkProps/WifiProps/MenuIconProps,
// UiGestureQuickSettings, UiBrightnessSlider, serta painter
// sosIcon/bleIcon/networkIcon/wifiIcon/drawMenuIconRGB berasal dari RP_TFTDisplay.

class QuickSettingsPanel {
public:
  void begin(AppOS* os){
    _os = os;
    _qs.begin(&_os->ui, &_os->bg, &_os->blit);

    UiGestureQuickSettings::Config qcfg;
    qcfg.TOP_ZONE = 20;
    qcfg.MAX_PULL = QS_H;
    qcfg.ANIM_MS  = 200;
    qcfg.PADDING  = 14;
    qcfg.BOTTOM_RADIUS = QS_R;
    qcfg.OPEN_RELEASE_TRIGGER_PX  = 8;
    qcfg.CLOSE_ZONE_PX            = 16;
    qcfg.CLOSE_RELEASE_TRIGGER_PX = 40;
    qcfg.PANEL_R = Theme::UI_R; qcfg.PANEL_G = Theme::UI_G; qcfg.PANEL_B = Theme::UI_B;
    qcfg.HANDLE_R=200; qcfg.HANDLE_G=200; qcfg.HANDLE_B=200;
    qcfg.FOOTER_R=Theme::SEP_R; qcfg.FOOTER_G=Theme::SEP_G; qcfg.FOOTER_B=Theme::SEP_B;
    qcfg.USE_PRECOMPOSE = true;
    qcfg.PANEL_Y_OFFSET = Theme::BAR_H;
    qcfg.PEEK_ENABLED   = false;

    _qs.setConfig(qcfg);
    _qs.setUnderlay(&_underlay, this);
    _qs.setContent(&_contentPaint, &_contentInput, this);

    UiBrightnessSlider::Config sc;
    sc.valuePct = 80;
    sc.minPct   = 5;
    sc.maxPct   = 100;
    sc.corner=16; sc.iconRadius=8; sc.rayLen=5;
    sc.trackR=26; sc.trackG=28; sc.trackB=38;
    sc.fillR=255; sc.fillG=255; sc.fillB=255;
    sc.iconR=0; sc.iconG=0; sc.iconB=0;

    // Ambil brightness terakhir dari app (NVS/RAM)
    if (_brGet){
      uint8_t v = _brGet(_brCtx);
      if (v < sc.minPct) v = sc.minPct;
      if (v > sc.maxPct) v = sc.maxPct;
      sc.valuePct = v;
    }

    _slider.setConfig(sc);

    // Apply brightness awal via app (biar sinkron dengan state sleep/dim)
    if (_brSet) _brSet((uint8_t)_slider.value(), _brCtx);
    else        _os->ui.tft().setBacklightPercent(_slider.value());

    // ---- Slider rect (GLOBAL) ----
    // Slider di sisi kanan QS, full height panel (minus padding).
    const int pad = 14;
    _sW = 64;                 // lebar slider
    _sH = 160;                // tinggi slider (samakan dengan plateH biar rapi)
    _sX = TFT_WIDTH - pad - _sW;
    _sY = 12;

    // IMPORTANT: UiBrightnessSlider pakai koordinat GLOBAL (y sudah termasuk Theme::BAR_H)
    _slider.setRect(_sX, Theme::BAR_H + _sY, _sW, _sH);

    // ---- Plates (panel-local) ----
    _plateX=14; _plateY=12; _plateW=160; _plateH=160; _plateRound=14;

    const int gapX = 8;
    _sqX  = _plateX + _plateW + gapX;
    _sqTY = _plateY; _sqTW = 60; _sqTH = 60; _sqTR = 10;

    _sqBX = _sqX; _sqBY=_sqTY + _sqTH + 8; _sqBW=60; _sqBH=90; _sqBR=10;

    // Batasi area plate supaya tidak tabrakan dengan slider
    const int gapToSlider = 10;
    const int maxRight = max(0, _sX - gapToSlider); // sekarang _sX sudah valid

    if (_plateX + _plateW > maxRight) _plateW = max(0, maxRight - _plateX);

    // Recompute posisi plate kanan setelah _plateW mungkin berubah
    _sqX  = _plateX + _plateW + gapX;
    _sqBX = _sqX;

    if (_sqX >= maxRight) {
      _sqTW = 0; _sqBW = 0;
    } else {
      int allowW = max(0, maxRight - _sqX);
      if (_sqTW > allowW) _sqTW = allowW;
      if (_sqBW > allowW) _sqBW = allowW;
    }

    // ==== Default icon cluster (GLOBAL coords dihitung dari panel-local) ====
    computeIconDefaults(4, 6, 20);

    // ==== Menu icon di TOP-RIGHT plate ====
    if (_sqTW > 0 && _sqTH > 0){
      _menu.cx = _sqX + _sqTW/2;
      _menu.cy = Theme::BAR_H + _sqTY + _sqTH/2;
      _menu.barThick = 4; _menu.gap=10; _menu.dotR=3; _menu.spacing=6; _menu.barLen=26;
      _menu.R=255; _menu.G=255; _menu.B=255;
    }

    _prevHeight = _qs.currentHeightPx();
    
    // init AutoSleep state dari app (kalau ada)
    if (_autoGet) _auto.on = _autoGet(_autoCtx);
  }

  // Router untuk navigasi (menu/Wi-Fi)
  void attachRouter(IRouter* r){ _router = r; }

  // ===== Binding BLE (QS ↔ controller) =====
  using BleSetFn = void(*)(bool on, void* ctx);
  using BleGetFn = bool(*)(void* ctx);

  void bindBle(void* ctx, BleSetFn setFn, BleGetFn getFn){
    _bleCtx = ctx; _bleSet = setFn; _bleGet = getFn;
    if (_bleGet) { _ble.on = _bleGet(_bleCtx); }
  }

  void setBleIcon(bool on){
    if (_ble.on != on){ _ble.on = on; _qs.requestRefresh(); }
  }

  // ===== Binding Wi-Fi (QS ↔ controller/halaman) =====
  using WifiSetFn = void(*)(bool on, void* ctx);
  using WifiGetFn = bool(*)(void* ctx);

  void bindWifi(void* ctx, WifiSetFn setFn, WifiGetFn getFn){
    _wifiCtx = ctx; _wifiSet = setFn; _wifiGet = getFn;
    if (_wifiGet) { _wifi.on = _wifiGet(_wifiCtx); }
  }

  // dipanggil controller kalau status berubah supaya ikon update instan
  void setWifiIcon(bool on){
    if (_wifi.on != on){ _wifi.on = on; _qs.requestRefresh(); }
  }

  // ===== Binding Brightness (QS slider ↔ app) =====
  using BrightSetFn = void(*)(uint8_t pct, void* ctx);
  using BrightGetFn = uint8_t(*)(void* ctx);

  void bindBrightness(void* ctx, BrightSetFn setFn, BrightGetFn getFn){
    _brCtx = ctx; _brSet = setFn; _brGet = getFn;
  }

  // ===== Binding Auto Sleep (QS toggle ↔ app) =====
  using AutoSleepSetFn = void(*)(bool on, void* ctx);
  using AutoSleepGetFn = bool(*)(void* ctx);

  void bindAutoSleep(void* ctx, AutoSleepSetFn setFn, AutoSleepGetFn getFn){
    _autoCtx = ctx; _autoSet = setFn; _autoGet = getFn;
    if (_autoGet) _auto.on = _autoGet(_autoCtx);
  }

  bool autoSleepEnabled() const { return _auto.on; }

  // ===== lifecycle =====
  void update(){
    _qs.update();
    const int h = _qs.currentHeightPx();
    _justClosed = (_prevHeight > 0 && h == 0);
    _prevHeight = h;

    // Sinkron status ikon dengan sumber kebenaran (controller)
    if (_wifiGet){
      bool s = _wifiGet(_wifiCtx);
      if (s != _wifi.on){ _wifi.on = s; _qs.requestRefresh(); }
    }
    if (_bleGet){
      bool s = _bleGet(_bleCtx);
      if (s != _ble.on){ _ble.on = s; _qs.requestRefresh(); }
    }
    if (_autoGet){
      bool s = _autoGet(_autoCtx);
      if (s != _auto.on){ _auto.on = s; _qs.requestRefresh(); }
    }
  }


  bool isOpen() const { return _qs.currentHeightPx() > 0; }
  bool consumeJustClosed(){ bool c=_justClosed; _justClosed=false; return c; }
  int  currentHeight() const { return _qs.currentHeightPx(); }

  void handleOutsideTapToClose(bool touching, int /*tx*/, int ty){
    const int currH = _qs.currentHeightPx();
    if (currH <= 0) { _prevTouching = touching; return; }
    const bool tappedOutside = (touching && !_prevTouching && ty >= Theme::BAR_H + currH);
    if (tappedOutside){ _qs.animateClose(); }
    _prevTouching = touching;
  }

  // Paksa QS menutup (dipakai saat masuk UI sleep)
  void requestClose(){
    if (_qs.currentHeightPx() <= 0) {
      _prevTouching = false;
      return;
    }
    _qs.animateClose();
    _prevTouching = false;
  }

  // ===== Icon accessors =====
  SosProps&       sos()   { return _sos;  }
  BleProps&       ble()   { return _ble;  }
  NetworkProps&   net()   { return _net;  }
  WifiProps&      wifi()  { return _wifi; }
  MenuIconProps&  menu()  { return _menu; }

  void computeIconDefaults(int innerPad=4, int gap=6, int /*forceR*/=0){
    int cx0 = _plateX + innerPad;
    int cy0 = _plateY + innerPad;
    int cw  = max(0, _plateW - innerPad - innerPad);
    int ch  = max(0, _plateH - innerPad - innerPad);

    int cellW = (cw - gap) / 2;
    int cellH = (ch - gap) / 2;
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;

    int cL = cx0 + cellW/2;
    int cR = cx0 + cellW + gap + cellW/2;
    int rT = cy0 + cellH/2;
    int rB = cy0 + cellH + gap + cellH/2;

    int outerR = 22;

    int gLx = cL;
    int gRx = cR;
    int gTy = Theme::BAR_H + rT;
    int gBy = Theme::BAR_H + rB;

    _sos.cx  = gLx; _sos.cy  = gTy; _sos.outerR = outerR; _sos.on  = false;
    _ble.cx  = gRx; _ble.cy  = gTy; _ble.outerR = outerR; _ble.on  = false;
    _net.cx  = gLx; _net.cy  = gBy; _net.outerR = outerR; _net.on  = false;
    _wifi.cx = gRx; _wifi.cy = gBy; _wifi.outerR= outerR; _wifi.on = false;
  }

private:
  AppOS* _os=nullptr;
  IRouter* _router=nullptr;
  UiGestureQuickSettings _qs;
  UiBrightnessSlider     _slider;
  bool _justClosed=false;
  int  _prevHeight=0;
  bool _prevTouching=false;

  static constexpr int QS_H=210;
  static constexpr int QS_R=16;

  // ---- Slider rect (panel-local) ----
  int _sX=0,_sY=0,_sW=0,_sH=0;

  // ---- Plate color ----
  static constexpr uint8_t PLATE_R=26, PLATE_G=28, PLATE_B=38;

  // ---- Main plate (panel-local) ----
  int _plateX=0,_plateY=0,_plateW=0,_plateH=0,_plateRound=14;

  // ---- Right plates (panel-local) ----
  int _sqX=0,_sqTY=0,_sqTW=0,_sqTH=0,_sqTR=10;      // TOP-RIGHT (menu)
  int _sqBX=0,_sqBY=0,_sqBW=0,_sqBH=0,_sqBR=10;     // bottom plate

  // ---- Icon props (GLOBAL) ----
  WifiProps     _wifi{};
  NetworkProps  _net{};
  BleProps      _ble{};
  SosProps      _sos{};
  MenuIconProps _menu{};

  // ====== Tap/hold handling ======
  enum IconId : uint8_t { IconNone=0, IconSOS, IconBLE, IconNET, IconWIFI };
  bool   _inputWasDown = false;
  IconId _pressedIcon  = IconNone;
  // ===== SOS (tap ON, hold 3s OFF) =====
  uint32_t _sosPressMs = 0;
  bool     _sosInside = false;
  bool     _sosSuppressToggle = false;
  bool     _sosHoldFired = false;

  // Menu button state
  bool _menuHeld   = false;
  bool _menuInside = false;

  // Wi-Fi hold-to-open Page_Wifi (immediate)
  static constexpr uint16_t WIFI_HOLD_MS = 1000; // 2 detik
  uint32_t _wifiPressMs     = 0;
  bool     _wifiHeld        = false;   // sudah melewati ambang hold
  bool     _wifiInside      = false;   // masih di dalam ikon saat tracking
  bool     _wifiNavigateFired = false; // sudah trigger navigasi
  bool     _wifiSuppressToggle = false;// cegah toggle saat release setelah navigate

  // Binding Wi-Fi
  void*     _wifiCtx = nullptr;
  WifiSetFn _wifiSet = nullptr;
  WifiGetFn _wifiGet = nullptr;

  // Binding BLE
  void*    _bleCtx = nullptr;
  BleSetFn _bleSet = nullptr;
  BleGetFn _bleGet = nullptr;

  // Binding Brightness
  void*      _brCtx = nullptr;
  BrightSetFn _brSet = nullptr;
  BrightGetFn _brGet = nullptr;

  // Auto Sleep toggle state
  struct AutoSleepProps { bool on=false; } _auto{};
  void*          _autoCtx = nullptr;
  AutoSleepSetFn _autoSet = nullptr;
  AutoSleepGetFn _autoGet = nullptr;

  bool _autoHeld   = false;
  bool _autoInside = false;

  static inline int clampi(int v,int a,int b){ return v<a?a:(v>b?b:v); }

  IconId hitIcon(int tx,int ty) const {
    auto inside = [&](int cx,int cy,int r)->bool{
      int dx = tx - cx; int dy = ty - cy; int rr = r + 2;
      return (dx*dx + dy*dy) <= rr*rr;
    };
    if (inside(_sos.cx,  _sos.cy,  _sos.outerR))  return IconSOS;
    if (inside(_ble.cx,  _ble.cy,  _ble.outerR))  return IconBLE;
    if (inside(_net.cx,  _net.cy,  _net.outerR))  return IconNET;
    if (inside(_wifi.cx, _wifi.cy, _wifi.outerR)) return IconWIFI;
    return IconNone;
  }

  void menuRectG(int& x,int& y,int& w,int& h) const {
    x = _sqX;
    y = Theme::BAR_H + _sqTY;
    w = _sqTW;
    h = _sqTH;
  }
  bool hitMenu(int tx,int ty) const {
    int x,y,w,h; menuRectG(x,y,w,h);
    if (w<=0 || h<=0) return false;
    return (tx>=x && tx<x+w && ty>=y && ty<y+h);
  }

  void autoRectG(int& x,int& y,int& w,int& h) const {
    x = _sqBX;
    y = Theme::BAR_H + _sqBY;
    w = _sqBW;
    h = _sqBH;
  }
  bool hitAuto(int tx,int ty) const {
    int x,y,w,h; autoRectG(x,y,w,h);
    if (w<=0 || h<=0) return false;
    return (tx>=x && tx<x+w && ty>=y && ty<y+h);
  }

  void toggleIcon(IconId id){
    switch(id){
      case IconSOS:
        if (!_sos.on) {
          _sos.on = true;
          PinsAndConfig::Power::sosActive = true;
          _qs.requestRefresh();
        }
        break;

      case IconBLE:
        if (_bleSet){
          bool target = !_ble.on;
          _bleSet(target, _bleCtx);  // minta controller ubah radio BLE
          _ble.on = target;          // optimistik
        } else {
          _ble.on = !_ble.on;
        }
        break;

      case IconNET:
        _net.on  = !_net.on;
        break;

      case IconWIFI:
        if (_wifiSet){
          bool target = !_wifi.on;
          _wifiSet(target, _wifiCtx);  // minta controller ubah state
          _wifi.on = target;           // optimistik; sinkron final di update()
        } else {
          _wifi.on = !_wifi.on;
        }
        break;

      default:
        break;
    }
    _qs.requestRefresh();
  }

  // ===== Draw helpers =====
  static void drawRoundedPlateColored(GfxRGB888& g, int Rrx, int Rry, int currH,
                                      int px,int py,int pw,int ph,int rad,
                                      uint8_t r,uint8_t gg,uint8_t b){
    if (pw <= 0 || ph <= 0) return;

    int visH = ph;
    int maxVis = currH - py;
    if (maxVis <= 0) return;
    if (visH > maxVis) visH = maxVis;

    const int gx = px;
    const int gy = Theme::BAR_H + py;

    const int tileTopG    = Rry;
    const int tileBottomG = Rry + g.h;
    const int plateTopG   = gy;
    const int plateBotG   = gy + visH;

    const int drawTopG    = max(tileTopG, plateTopG);
    const int drawBotG    = min(tileBottomG, plateBotG);
    if (drawBotG <= drawTopG) return;

    const int startYInTile = drawTopG - Rry;
    const int rows         = drawBotG - drawTopG;

    for (int yOff=0; yOff<rows; ++yOff){
      const int yInTile = startYInTile + yOff;
      const int yGlobal = Rry + yInTile;
      const int yy      = yGlobal - gy;

      int inset=0;
      if (rad>0){
        if (yy < rad){
          float dy = (float)(rad - 1 - yy);
          float dx = sqrtf((float)rad*rad - dy*dy);
          inset = (int)(rad - floorf(dx));
        }
        int y2 = (visH - 1 - yy);
        if (y2 < rad){
          float dy = (float)(rad - 1 - y2);
          float dx = sqrtf((float)rad*rad - dy*dy);
          int inset2 = (int)(rad - floorf(dx));
          if (inset2 > inset) inset = inset2;
        }
      }

      int x0g = gx + inset;
      int x1g = gx + pw - inset;

      int lx0 = max(0, x0g - Rrx);
      int lx1 = min(g.w, x1g - Rrx);
      if (lx1 <= lx0) continue;

      uint8_t* row = g.pix + ((size_t)yInTile * g.w + lx0) * 3;
      const int count = lx1 - lx0;
      for (int i=0; i<count; ++i){
        row[0]=r; row[1]=gg; row[2]=b;
        row += 3;
      }
    }
  }

  static void drawRoundedPlate(GfxRGB888& g, int Rrx, int Rry, int currH,
                               int px,int py,int pw,int ph,int rad){
    drawRoundedPlateColored(g, Rrx, Rry, currH, px,py,pw,ph,rad, PLATE_R,PLATE_G,PLATE_B);
  }

  // ===== Painters =====
  static void _underlay(GfxRGB888& /*g*/,int /*rx*/,int ry,int /*rw*/,int rh, void* user){
    auto* self = reinterpret_cast<QuickSettingsPanel*>(user);
    const int y0 = ry, y1 = ry + rh;
    if (y0 >= Theme::BAR_H) return;
    const int oy0 = max(y0, 0);
    const int oy1 = min(y1, Theme::BAR_H);
    if (oy1 <= oy0) return;

    int ox=0; int ow=TFT_WIDTH; int oh=(oy1-oy0);
    self->_os->blit.blit(self->_os->ui.tft(), self->_os->bg, ox,oy0,ow,oh,
      [&](GfxRGB888& gg,int,int,int Rw,int Rh){
        uint8_t* p=gg.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){ *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B; }
        if (Rh>0){
          uint8_t* row = gg.pix + ((size_t)(Rh-1)*Rw)*3;
          for (int i=0; i<Rw; ++i){ row[3*i+0]=Theme::SEP_R; row[3*i+1]=Theme::SEP_G; row[3*i+2]=Theme::SEP_B; }
        }
      }
    );
  }

  static void _contentPaint(GfxRGB888& g, int Rrx, int Rry, int currH, void* user){
    auto* self = reinterpret_cast<QuickSettingsPanel*>(user);

    // 1) Plates
    drawRoundedPlate(g, Rrx, Rry, currH, self->_plateX, self->_plateY, self->_plateW, self->_plateH, self->_plateRound);

    // TOP-RIGHT menu plate: highlight jika held & inside
    if (self->_sqTW > 0 && self->_sqTH > 0){
      const bool hi = (self->_menuHeld && self->_menuInside);
      const uint8_t hR = (uint8_t)min(255, (int)PLATE_R + (hi ? 26 : 0));
      const uint8_t hG = (uint8_t)min(255, (int)PLATE_G + (hi ? 26 : 0));
      const uint8_t hB = (uint8_t)min(255, (int)PLATE_B + (hi ? 26 : 0));
      drawRoundedPlateColored(g, Rrx, Rry, currH, self->_sqX, self->_sqTY, self->_sqTW, self->_sqTH, self->_sqTR, hR,hG,hB);
    }

    // Bottom-right: AutoSleep toggle plate
    if (self->_sqBW > 0 && self->_sqBH > 0){
      const bool on = self->_auto.on;
      const bool hi = (self->_autoHeld && self->_autoInside);

      uint8_t pr, pg, pb;
      if (on){
        // ON: putih (sedikit dim kalau sedang ditekan)
        pr = pg = pb = (uint8_t)(hi ? 220 : 255);
      } else {
        // OFF: warna plate normal (sedikit lebih terang kalau ditekan)
        pr = (uint8_t)min(255, (int)PLATE_R + (hi ? 26 : 0));
        pg = (uint8_t)min(255, (int)PLATE_G + (hi ? 26 : 0));
        pb = (uint8_t)min(255, (int)PLATE_B + (hi ? 26 : 0));
      }

      drawRoundedPlateColored(g, Rrx, Rry, currH,
                              self->_sqBX, self->_sqBY, self->_sqBW, self->_sqBH, self->_sqBR,
                              pr, pg, pb);

      // Icon "A" di tengah (GLOBAL coords)
      const int cx = self->_sqBX + self->_sqBW/2;
      const int cy = Theme::BAR_H + self->_sqBY + self->_sqBH/2;

      const int scale = 3;
      const char* txt = "A";
      const int tw = SimpleFont::textWidth(txt, scale, 1);
      const int glyphH = 7 * scale;

      const int x = cx - tw/2;
      const int yBase = cy + glyphH/2 - 1;

      const uint8_t tr = on ? 0   : 255;
      const uint8_t tg = on ? 0   : 255;
      const uint8_t tb = on ? 0   : 255;

      SimpleFont::drawTextStyled(g, x - Rrx, yBase - Rry, txt,
                                 tr,tg,tb, scale, 1,
                                 SimpleFont::AlignLeft, -1, SimpleFont::Normal);
    }

    // 2) Icons
    sosIcon    (g, Rrx, Rry, self->_sos);
    bleIcon    (g, Rrx, Rry, self->_ble);
    networkIcon(g, Rrx, Rry, self->_net);
    wifiIcon   (g, Rrx, Rry, self->_wifi);

    // 3) Menu icon
    if (self->_sqTW > 0 && self->_sqTH > 0){
      drawMenuIconRGB(g, Rrx, Rry, self->_menu);
    }

    // 4) Brightness slider
    self->_slider.paint(g, Rrx, Rry);
  }

  // Prioritas input: menu button > slider > icons (dengan hold Wi-Fi immediate)
  static bool _contentInput(bool touching, int tx,int ty, int currH, void* user){
    auto* self = reinterpret_cast<QuickSettingsPanel*>(user);

    // Block touches di bawah panel
    if (touching && ty >= Theme::BAR_H + currH) {
      self->_inputWasDown = touching;
      return false;
    }

    // 0) Menu button handling
    if (!self->_inputWasDown && touching){
      if (self->hitMenu(tx,ty)){
        self->_menuHeld   = true;
        self->_menuInside = true;
        self->_qs.requestRefresh();
        self->_inputWasDown = true;
        return true;
      }
    }
    else if (self->_menuHeld && touching){
      bool nowInside = self->hitMenu(tx,ty);
      if (nowInside != self->_menuInside){
        self->_menuInside = nowInside;
        self->_qs.requestRefresh();
      }
      self->_inputWasDown = true;
      return true;
    }
    else if (self->_menuHeld && !touching){
      const bool clicked = self->_menuInside;
      self->_menuHeld = false;
      self->_menuInside = false;
      self->_qs.requestRefresh();
      self->_inputWasDown = false;

      if (clicked){
        if (self->_router) self->_router->requestMenu();
        self->_qs.animateClose();
      }
      return true;
    }

    // 0.5) AutoSleep button handling (toggle)
    if (!self->_inputWasDown && touching){
      if (self->hitAuto(tx,ty)){
        self->_autoHeld   = true;
        self->_autoInside = true;
        self->_qs.requestRefresh();
        self->_inputWasDown = true;
        return true;
      }
    }
    else if (self->_autoHeld && touching){
      bool nowInside = self->hitAuto(tx,ty);
      if (nowInside != self->_autoInside){
        self->_autoInside = nowInside;
        self->_qs.requestRefresh();
      }
      self->_inputWasDown = true;
      return true;
    }
    else if (self->_autoHeld && !touching){
      const bool clicked = self->_autoInside;

      self->_autoHeld = false;
      self->_autoInside = false;
      self->_qs.requestRefresh();
      self->_inputWasDown = false;

      if (clicked){
        bool target = !self->_auto.on;
        if (self->_autoSet) self->_autoSet(target, self->_autoCtx);
        self->_auto.on = target; // optimistik; final disinkronkan di update()
        self->_qs.requestRefresh();
      }
      return true;
    }

    // 1) Slider
    bool changed = self->_slider.handleTouch(touching, tx, ty);
    if (changed) {
      uint8_t v = (uint8_t)self->_slider.value();
      if (self->_brSet) self->_brSet(v, self->_brCtx);
      else              self->_os->ui.tft().setBacklightPercent(v);

      self->_inputWasDown = touching;
      return true;
    }

    // 2) Icons (dengan hold khusus Wi-Fi)
    if (!self->_inputWasDown && touching){
      self->_pressedIcon = self->hitIcon(tx, ty);
      self->_inputWasDown = touching;

      // ===== SOS HOLD 3s to OFF =====
      if (self->_pressedIcon == IconSOS){
        self->_sosPressMs = millis();
        self->_sosHoldFired = false;
        self->_sosSuppressToggle = false;
        self->_sosInside = true; // start di dalam icon
        return true; // consume untuk tracking
      }

      if (self->_pressedIcon == IconWIFI){
        self->_wifiPressMs = millis();
        self->_wifiHeld = false;
        self->_wifiInside = true;
        self->_wifiNavigateFired = false;
        self->_wifiSuppressToggle = false;
        return true; // consume untuk tracking
      }
      return (self->_pressedIcon != IconNone);
    }

    // ===== WiFI =====
    if (self->_inputWasDown && touching){
      // ===== SOS tracking (hold 3s to OFF) =====
      if (self->_pressedIcon == IconSOS){
        auto inside = [&](int cx,int cy,int r)->bool{
          int dx = tx - cx; int dy = ty - cy; int rr = r + 2;
          return (dx*dx + dy*dy) <= rr*rr;
        };
        self->_sosInside = inside(self->_sos.cx, self->_sos.cy, self->_sos.outerR);

        // Hold 3 detik untuk mematikan SOS (hanya jika sedang ON dan jari masih di dalam ikon)
        if (self->_sos.on &&
            self->_sosInside &&
            !self->_sosHoldFired &&
            (uint32_t)(millis() - self->_sosPressMs) >= 3000u){

          self->_sos.on = false;
          PinsAndConfig::Power::sosActive = false;

          self->_sosHoldFired = true;
          self->_sosSuppressToggle = true; // cegah tap logic saat release
          self->_qs.requestRefresh();
        }
        return true;
      }

      // ===== WiFI =====
      // tracking hold Wi-Fi
      if (self->_pressedIcon == IconWIFI){
        // masih sentuh: cek apakah masih di dalam ikon
        auto inside = [&](int cx,int cy,int r)->bool{
          int dx = tx - cx; int dy = ty - cy; int rr = r + 2;
          return (dx*dx + dy*dy) <= rr*rr;
        };
        self->_wifiInside = inside(self->_wifi.cx, self->_wifi.cy, self->_wifi.outerR);

        // Jika belum fire navigate, dan masih di dalam, cek ambang hold
        if (!self->_wifiNavigateFired && self->_wifiInside){
          if ((uint32_t)(millis() - self->_wifiPressMs) >= WIFI_HOLD_MS){
            // HOLD terpenuhi → langsung navigasi tanpa menunggu release
            self->_wifiHeld = true;
            self->_wifiNavigateFired = true;
            self->_wifiSuppressToggle = true; // cegah toggle saat dilepas
            if (self->_router) self->_router->requestWifi();
            self->_qs.animateClose();
            // opsional: self->_qs.requestRefresh(); // kalau mau highlight sesaat
          }
        }
        // Saat sudah navigateFired, tetap consume input agar event tidak bocor
        return true;
      }
      // ikon lain: tetap consume jika sedang menekan salah satu
      return (self->_pressedIcon != IconNone);
    }

    if (self->_inputWasDown && !touching){

      if (self->_pressedIcon == IconSOS){
        bool suppress = self->_sosSuppressToggle;
        bool inside   = self->_sosInside;

        // reset tracking
        self->_pressedIcon = IconNone;
        self->_inputWasDown = false;
        self->_sosInside = false;

        // kalau barusan OFF via hold, jangan lakukan apa-apa saat release
        if (suppress){
          self->_sosSuppressToggle = false;
          self->_sosHoldFired = false;
          return true;
        }

        // TAP: hanya boleh menyalakan
        if (inside && !self->_sos.on){
          self->_sos.on = true;
          PinsAndConfig::Power::sosActive = true;
          self->_qs.requestRefresh();
        }

        self->_sosHoldFired = false;
        return true;
      }

      // release
      if (self->_pressedIcon == IconWIFI){
        IconId id = self->_pressedIcon;
        bool suppress = self->_wifiSuppressToggle;
        bool inside   = self->_wifiInside;

        // reset state
        self->_pressedIcon = IconNone;
        self->_inputWasDown = false;
        self->_wifiHeld = false;
        self->_wifiInside = false;

        if (suppress){
          // baru saja navigate via hold → tidak melakukan toggle
          self->_wifiSuppressToggle = false;
          self->_wifiNavigateFired  = false;
          return true;
        } else if (inside){
          // TAP normal → toggle Wi-Fi
          self->toggleIcon(id);
          return true;
        }
        return false;
      }

      if (self->_pressedIcon != IconNone){
        IconId id = self->_pressedIcon;
        self->_pressedIcon = IconNone;
        self->_inputWasDown = false;
        self->toggleIcon(id);
        return true;
      }

      self->_inputWasDown = false;
      return false;
    }

    self->_inputWasDown = touching;
    return (self->_pressedIcon != IconNone);
  }
};
