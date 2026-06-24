#pragma once
#include "PageBase.h"
#include <math.h>
#include <string.h>
#include <WiFi.h>
#include <RP_TFTDisplay.h>
#include <Preferences.h>   // <- tambahan untuk menyimpan jaringan
extern "C" {
  #include "esp_wifi.h"
}

/*
  PageWifi – v4.5 (v4.3 + Preferences + AutoConnect)
  - Basis: v4.3 yang sudah stabil (scan delay, clean mode, VK, Cancel/Connect).
  - Tambahan:
    * Menyimpan jaringan yang berhasil connect ke Preferences (NVS).
    * Auto-connect ke SSID yang tersimpan dengan RSSI paling kuat setelah scan.
*/

class QuickSettingsPanel; // forward-declare

class PageWifi : public PageBase, public ITextSink {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("WiFi");

    // ==== Preferences (remembered networks) ====
    _prefs.begin("wifi_known", false);   // namespace NVS
    loadKnownNetworks();

    // ==== WiFi power state init ====
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK) {
      _wifiOn = (m != WIFI_MODE_NULL && m != WIFI_MODE_MAX && m != WIFI_MODE_AP);
    } else {
      _wifiOn = false;
    }

    _knobT = _wifiOn ? 1.f : 0.f;
    _animating=false; _animFrom=_knobT; _animTo=_knobT; _animStartMs=0;
    _pressing=false; _tgHold=false; _rfHold=false; _tgPressed=false; _rfPressed=false;
    _rfPulseTil=0;

    _scanning=false; _scanCount=-2;

    _listScrollY = 0; _listScrollMax = 0;
    _listDragging=false; _listDragY0=0; _listScrollY0=0;

    _wifiLastOnAt = _wifiOn ? millis() : 0;
    _scanStartAt  = _wifiOn ? (_wifiLastOnAt + 2000) : 0;   // sama seperti v4.3 (2 s)

    recomputeLayout();
    updateContentHeight();

    if (_qs) _qs->setWifiIcon(_wifiOn);

    // ==== Clean & SSID press state ====
    _cleanMode = false;
    _ssidPressing = false;
    _ssidPressedIdx = -1;

    // ==== PwCard state ====
    _showPwCard = false;
    _pwFocused  = false;
    _pwLen      = 0;
    _selectedSsid = "";
    _cursorBlinkMs = millis();

    // ==== Connect state ====
    _connecting       = false;
    _connectFailed    = false;
    _connectStartMs   = 0;
    _connectTimeoutMs = 15000; // 15 s
    _lastWL           = WL_IDLE_STATUS;
    _connectionShownSsid = "";
    _lastConnectPw    = "";

    // ==== VK init ====
    _vk.begin(&_os->ui, &_os->bg, &_os->blit);
    _vk.attachSink(this);
    _vk.setUnderlayPainter(&_vkUnderlay);
    _vk.setCommitOnDown(true);
    _vk.setRepeat(true, 300, 100);
    _vkWasVisible = false;
  }

  void linkQuickSettings(QuickSettingsPanel* qs){
    _qs = qs;
    if (_qs){
      // urutan: setFn dulu, lalu getFn (sudah benar di v4.3)
      _qs->bindWifi(this, &_qsSetWifi, &_qsGetWifi);
      _qs->setWifiIcon(_wifiOn);
    }
  }

  void onEnter() override {
    PageBase::onEnter();
    recomputeLayout();
    updateContentHeight();
    if (_wifiOn && _scanCount < 0 && !_scanning && _scanStartAt == 0) {
      _wifiLastOnAt = (_wifiLastOnAt ? _wifiLastOnAt : millis());
      _scanStartAt  = _wifiLastOnAt + 1000;   // sama seperti v4.3
    }
    if (_qs) _qs->setWifiIcon(_wifiOn);

    _cleanMode = _cleanMode && _showPwCard;
    _pwFocused = false;
    _vk.hide();
    _vkWasVisible = false;

    // reset connect visual
    _connecting=false; _connectFailed=false; _lastWL=WL_IDLE_STATUS;
  }

  void onQSClosed() override {
    if (!isActive()) return;
    recomputeLayout();
    updateContentHeight();
    PageBase::onQSClosed();
  }

  int contentHeight() const { return _contentH; }

  // ======= QS -> set wifi =======
  void requestSetWifi(bool on){
    if (_wifiOn == on) {
      if (_qs) _qs->setWifiIcon(_wifiOn);
      return;
    }
    _wifiOn = on;
    beginToggleAnim(on);
    applyWifiState(on);

    if (on){
      _listScrollY = 0;
      _wifiLastOnAt = millis();
      _scanStartAt  = _wifiLastOnAt + 1000;   // sama seperti v4.3
    } else {
      _listScrollY = 0;
      _scanStartAt = 0;
    }
    if (_qs) _qs->setWifiIcon(_wifiOn);
  }

  // ===== ITextSink (untuk VK) =====
  void insertChar(char c) override {
    if (_pwLen < PW_MAX) {
      _pwBuf[_pwLen++] = c;
      _pwBuf[_pwLen]   = 0;
      _cursorBlinkMs   = millis();
    }
  }
  void backspace() override {
    if (_pwLen > 0) {
      _pwLen--;
      _pwBuf[_pwLen] = 0;
      _cursorBlinkMs = millis();
    }
  }
  void enter() override {
    // biarkan tetap fokus; Connect via tombol khusus
    _pwFocused = true;
    _cursorBlinkMs = millis();
  }

protected:
  // ======================================================================
  // Render
  // ======================================================================
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const uint32_t now   = millis();
    const int      RryEff= Rry + _scrollY;

    // Step VK tiap frame
    _vk.step();

    // Poll koneksi bila sedang connecting (di mana pun, tapi utamanya saat clean mode)
    if (_connecting) {
      wl_status_t st = WiFi.status();
      _lastWL = st;

      if (st == WL_CONNECTED) {
        // ====== SUKSES CONNECT ======
        _connecting     = false;
        _connectFailed  = false;
        _vk.hide();
        _pwFocused      = false;
        _cleanMode      = false;   // kembali ke mode normal
        _showPwCard     = false;   // card password ditutup
        _connectPulseTil = millis() + 400;

        // simpan jaringan yang sukses
        rememberNetwork(_connectionShownSsid, _lastConnectPw.c_str());

        // sinkronkan ikon QS
        if (_qs) _qs->setWifiIcon(true);
      }
      else if ((int32_t)(now - _connectStartMs) > (int32_t)_connectTimeoutMs) {
        // ====== GAGAL / TIMEOUT ======
        _connecting    = false;
        _connectFailed = true;

        // tetap di clean mode dengan card password,
        // VK ditutup supaya user bisa koreksi password lalu buka VK lagi manual.
        _vk.hide();
        _pwFocused  = false;
        _cleanMode  = true;
        _showPwCard = true;
      }
    }

    // ==== CLEAN MODE: hanya PwCard + layar hitam di belakang ====
    if (_cleanMode) {
      // latar hitam
      fillRectClipped(g, Rrx, Rry, 0, _viewportY0, TFT_WIDTH, _viewportH, 0,0,0);

      if (_showPwCard) {
        drawPwCard(g, Rrx, Rry, now);
      }

      // Render VK terakhir pada tile ini
      _vk.paintIntoTile(g, Rrx, Rry, Rry, g.h);
      _vkWasVisible = _vk.visible();
      return;
    }

    // ==== MODE NORMAL ====
    if (_animating){
      float u = (now - _animStartMs) / (float)_animDurMs;
      if (u >= 1.f){ u=1.f; _animating=false; }
      _knobT = easeOutCubic(_animFrom, _animTo, u);
    }

    if (_wifiOn && !_scanning && _scanStartAt && (int32_t)(now - _scanStartAt) >= 0) {
      startScan();
      _scanStartAt = 0;
    }

    if (_scanning){
      int s = WiFi.scanComplete();
      if (s >= 0){
        _scanCount   = s;
        _scanning    = false;
        _listScrollY = 0;
        updateContentHeight();

        // === Tambahan: auto-connect ke jaringan yang sudah tersimpan ===
        tryAutoConnectFromScan();
      }
    }

    // Bar atas
    drawToggle (g, Rrx, RryEff, _tgX, _tgY, _tgW, _tgH);
    const bool pulsing = (_rfPulseTil && (int32_t)(now - _rfPulseTil) < 0);
    drawRefresh(g, Rrx, RryEff, _rfCX, _rfCY, _rfR, pulsing);

    // Main card
    drawMainCard(g, Rrx, RryEff, _card);
  }

  // ======================================================================
  // Input
  // ======================================================================
  void handleContentInput(bool pressed, int x, int y) override {
    PageBase::handleContentInput(pressed, x, y);

    // ===== CleanMode + PwCard =====
    if (_cleanMode && _showPwCard) {

      // Hit-test dasar untuk textbox dan tombol
      const bool inTextbox = ptInRect(_pwTextbox, x, y);
      const bool inButtons = ptInRect(_btnCancel, x, y) || ptInRect(_btnConnect, x, y);

      // Routing ke VK: VK hanya menerima event jika tap di area VK
      if (_vk.visible()) {
        int kx, ky, kw, kh;
        _vk.rect(kx, ky, kw, kh);
        const bool inVK = (x >= kx && x < kx + kw && y >= ky && y < ky + kh);

        if (inVK) {
          // Sentuhan di area keyboard → hanya VK yang menerima
          _vk.handleTouch(pressed, x, y);
          return;
        }

        if (pressed && !inTextbox && !inButtons) {
          // Tap di luar textbox, tombol, dan VK → tutup VK saja
          _vk.hide();
        }

        if (!pressed && _vk.visible()) {
          // Release dummy untuk menyelesaikan gesture di VK bila perlu
          _vk.handleTouch(false, 0, 0);
        }
      }

      // Semua area selain VK → ditangani oleh PwCard (textbox + Cancel/Connect)
      handlePwCardInput(pressed, x, y);
      return;
    }

    // ===== Mode normal =====
    const int cx = x;
    const int cy = y + _scrollY;

    const bool inToggle  = (cy >= _tgY && cy < _tgY + _tgH && cx >= _tgX && cx < _tgX + _tgW);
    const int  dx = cx - _rfCX, dy = cy - _rfCY;
    const bool inRefresh = (dx*dx + dy*dy) <= _rfR*_rfR;
    const bool inList    = ptInRect(_listViewport, cx, cy);

    if (pressed) {
      if (!_pressing){
        _pressing=true; _downX=cx; _downY=cy;
        _tgHold = inToggle; _rfHold=inRefresh;

        _ssidPressing   = false;
        _ssidPressedIdx = -1;

        if (!inToggle && !inRefresh && inList && _wifiOn && _scanCount>0){
          const int itemH = 60;
          const int gapY  = 10;
          const int relY  = (cy - _listViewport.y) + _listScrollY;
          if (relY >= 0) {
            const int step = itemH + gapY;
            const int idx  = relY / step;
            const int yIn  = relY - idx * step;
            if (idx >= 0 && idx < _scanCount && yIn >= 0 && yIn < itemH) {
              _ssidPressing   = true;
              _ssidPressedIdx = idx;
              _ssidDownX      = cx;
              _ssidDownY      = cy;
            }
          }
        }

        _listDragging=false;
        _dragging=false;
        if (!inToggle && !inRefresh && !inList){
          _dragging=true; _dragY0=cy; _scrollY0=_scrollY;
        }

      } else {
        // MOVE
        if (_ssidPressing){
          if (abs(cx - _ssidDownX) > TAP_SLOP || abs(cy - _ssidDownY) > TAP_SLOP){
            _ssidPressing = false;
            _ssidPressedIdx = -1;
            _listDragging = inList;
            _listDragY0   = cy;
            _listScrollY0 = _listScrollY;
          }
        } else if (!_listDragging && inList && _wifiOn && _scanCount>0) {
          if (abs(cy - _downY) > TAP_SLOP){
            _listDragging = true;
            _listDragY0   = cy;
            _listScrollY0 = _listScrollY;
          }
        }

        if (_listDragging){
          int dyDrag = cy - _listDragY0;
          int newScroll = _listScrollY0 - dyDrag;
          if (newScroll < 0) newScroll = 0;
          if (newScroll > _listScrollMax) newScroll = _listScrollMax;
          _listScrollY = newScroll;
        } else if (_dragging){
          int dyDrag = cy - _dragY0;
          int newScroll = _scrollY0 - dyDrag;
          if (newScroll < 0) newScroll = 0;
          if (newScroll > _scrollMax) newScroll = _scrollMax;
          _scrollY = newScroll;
        } else {
          _tgPressed = inToggle;
          _rfPressed = inRefresh;
        }
      }
    } else {
      // RELEASE
      bool tappedSSID = false;

      if (_ssidPressing && !_listDragging && !_dragging) {
        tappedSSID = true;
      }

      _ssidPressing   = false;
      _ssidPressedIdx = -1;

      if (_listDragging){ _listDragging=false; }
      else if (_dragging){ _dragging=false; }
      else {
        if (_tgHold){
          _wifiOn = !_wifiOn;
          beginToggleAnim(_wifiOn);
          applyWifiState(_wifiOn);

          if (_wifiOn) {
            _listScrollY = 0;
            _wifiLastOnAt = millis();
            _scanStartAt  = _wifiLastOnAt + 1000;   // sama seperti v4.3
          } else {
            _listScrollY = 0;
            _scanStartAt = 0;
          }

          if (_qs) _qs->setWifiIcon(_wifiOn);
          _tgHold=false;
        }
        if (_rfHold){
          _rfPulseTil = millis() + 220;
          if (_wifiOn) {
            uint32_t now = millis();
            if (_wifiLastOnAt && (int32_t)(now - _wifiLastOnAt) < 1000) {
              _scanStartAt = _wifiLastOnAt + 1000;
            } else {
              startScan();
              _scanStartAt = 0;
            }
            _listScrollY = 0;
          }
          _rfHold=false;
        }

        // === TAP SSID → Clean Mode + PwCard ===
        if (tappedSSID && _wifiOn && _scanCount>0) {
          int idx = pickIndexFromY(_downY + _scrollY); // index saat press
          if (idx >= 0 && idx < _scanCount) {
            String s = WiFi.SSID(idx);
            if (s.length()==0) s = "(hidden)";
            _selectedSsid = s;
          } else {
            _selectedSsid = "";
          }

          _cleanMode  = true;
          _showPwCard = true;
          _pwFocused  = false;
          _pwLen      = 0;
          _pwBuf[0]   = 0;
          _cursorBlinkMs = millis();

          // reset connect state
          _connecting=false; _connectFailed=false; _lastWL=WL_IDLE_STATUS;

          recomputeLayout();
        }
      }

      _pressing=false;
      _tgPressed=_rfPressed=false;
    }
  }

private:
  // ======================================================================
  // Types & utils
  // ======================================================================
  struct Rect { int x=0,y=0,w=0,h=0; };

  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }
  static inline int iClamp(int v,int lo,int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
  static inline uint8_t u8(int v){ if(v<0) return 0; if(v>255) return 255; return (uint8_t)v; }
  static inline bool ptInRect(const Rect& r,int x,int y){ return (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h); }

  static inline float easeOutCubic(float a, float b, float t){
    if (t<0.f) t=0.f; if (t>1.f) t=1.f;
    float u = 1.f - powf(1.f - t, 3.f);
    return a + (b - a)*u;
  }
  static inline uint8_t lerp8(uint8_t a, uint8_t b, float t){
    int v = (int)lroundf(a + (b - a)*t);
    return u8(v);
  }

  // ======================================================================
  // Theme & spacing
  // ======================================================================
  using Theme = PinsAndConfig::Theme;
  static constexpr int EDGE       = Theme::EDGE_BUF;
  static constexpr int TITLE2CARD = 10;
  static constexpr int BAR_GAPY   = 12;
  static constexpr int BOT_PAD    = 20;
  static constexpr int CARD_RADIUS= 14;
  static constexpr float PI_F32   = 3.14159265358979323846f;

  // Spacing khusus PwCard
  static constexpr int SP_CONNECT_TO__TO_SSID   = 4;
  static constexpr int SP_SSID__TO_PW_LABEL     = 4;
  static constexpr int SP_LABEL__TO_TEXTBOX     = 4;
  static constexpr int PW_TEXTBOX_RADIUS        = 10;

  // Toggle (mode normal)
  int _tgW = 92;
  int _tgH = 40;
  int _tgX = 0;
  int _tgY = 0;

  // Refresh (mode normal)
  int _rfR  = 24;
  int _rfCX = 0;
  int _rfCY = 0;

  // Main card + inner (mode normal)
  Rect _card;
  Rect _inner;
  Rect _listViewport;

  // Scroll (page-level)
  int _contentH = 0;
  int _scrollY  = 0;
  int _scrollMax= 0;

  // Touch latches
  bool _pressing=false, _tgHold=false, _rfHold=false;
  bool _tgPressed=false, _rfPressed=false;
  int  _downX=0, _downY=0;
  bool _dragging=false; int _dragY0=0, _scrollY0=0;

  // Inner list scroll
  int  _listScrollY = 0;
  int  _listScrollMax = 0;
  bool _listDragging = false; int _listDragY0=0; int _listScrollY0=0;

  // Toggle anim
  float    _knobT       = 0.0f;   // 0..1
  bool     _animating   = false;
  float    _animFrom    = 0.0f;
  float    _animTo      = 0.0f;
  uint32_t _animStartMs = 0;
  uint16_t _animDurMs   = 220;
  uint32_t _rfPulseTil  = 0;

  // WiFi state
  bool     _wifiOn       = false;
  bool     _scanning     = false;
  int      _scanCount    = -2;     // -2 none, -1 running, >=0 done
  uint32_t _wifiLastOnAt = 0;
  uint32_t _scanStartAt  = 0;

  // QS binding
  QuickSettingsPanel* _qs = nullptr;

  // CLEAN MODE + Password Card
  bool _cleanMode = false;
  bool _showPwCard = false;
  String _selectedSsid;

  // Textbox password buffer (plain text)
  static constexpr int PW_MAX = 64;
  char _pwBuf[PW_MAX+1]{0};
  int  _pwLen = 0;
  bool _pwFocused = false;
  uint32_t _cursorBlinkMs = 0;

  // SSID press/highlight
  static constexpr int TAP_SLOP = 12;
  bool _ssidPressing = false;
  int  _ssidPressedIdx = -1;
  int  _ssidDownX = 0, _ssidDownY = 0;

  // Geometry cached untuk PW card
  Rect _pwCard{};
  Rect _pwTextbox{};
  Rect _btnCancel{};
  Rect _btnConnect{};

  // VK
  VkOverlay _vk;
  bool      _vkWasVisible=false;
  static void _vkUnderlay(GfxRGB888& g,int Rrx,int Rry,int x,int y,int w,int h){
    g.fillRect(x - Rrx, y - Rry, w, h, 20,22,28);
  }

  // ===== Connect state =====
  bool        _connecting=false;
  bool        _connectFailed=false;
  uint32_t    _connectStartMs=0;
  uint16_t    _connectTimeoutMs=15000;
  wl_status_t _lastWL = WL_IDLE_STATUS;
  String      _connectionShownSsid;
  String      _lastConnectPw;      // password terakhir yang dipakai connect
  uint32_t    _connectPulseTil=0;

  // ==== Preferences: known networks ====
  Preferences _prefs;
  struct KnownNetwork {
    String ssid;
    String pw;
  };
  static constexpr int MAX_KNOWN = 8;
  KnownNetwork _known[MAX_KNOWN];
  int          _knownCount = 0;

  // PW card press state
  bool _pwPressing=false;
  int  _pwDownX=0,_pwDownY=0;
  bool _btnCancelHold=false, _btnConnectHold=false;

  // ======================================================================
  // Layout & geometry
  // ======================================================================
  void recomputeLayout() {
    const int barTop = _viewportY0 + TITLE2CARD;

    _tgX = TFT_WIDTH - EDGE - _tgW;
    _tgY = barTop;

    _rfCX = EDGE + _rfR;
    _rfCY = barTop + _rfR;

    const int barBottom = imax(_tgY + _tgH, _rfCY + _rfR);
    const int cardX = EDGE;
    const int cardY = barBottom + BAR_GAPY;
    const int cardW = TFT_WIDTH - 2*EDGE;
    const int cardH = imax(0, (_viewportY0 + _viewportH) - cardY - BOT_PAD);
    _card = { cardX, cardY, cardW, cardH };

    const int pad = 12;
    _inner = { _card.x+pad, _card.y+pad, _card.w-2*pad, _card.h-2*pad };

    const int pwW = TFT_WIDTH - 2*EDGE;
    const int pwX = EDGE;
    const int pwY = _viewportY0 + TITLE2CARD + 8;
    const int pwH = 186; // sedikit lebih tinggi untuk status teks
    _pwCard = { pwX, pwY, pwW, pwH };

    const int txPad = 14;
    const int textY0 = pwY + 18;
    const int tbH = 40;
    const int tbY = textY0 +
                    7*2 + SP_CONNECT_TO__TO_SSID +
                    7*2 + SP_SSID__TO_PW_LABEL +
                    7*1 + SP_LABEL__TO_TEXTBOX;

    _pwTextbox = { pwX + txPad, tbY, pwW - 2*txPad, tbH };

    const int btnW = (pwW - 3*txPad)/2;
    const int btnY = tbY + tbH + 16;
    _btnCancel  = { pwX + txPad, btnY, btnW, 36 };
    _btnConnect = { _btnCancel.x + btnW + txPad, btnY, btnW, 36 };
  }

  void updateContentHeight(){
    const int bottom = _card.y + _card.h + BOT_PAD;
    _contentH = bottom;

    const int vpH = _viewportH;
    int maxY = _contentH - vpH;
    if (maxY < 0) maxY = 0;
    _scrollMax = maxY;
    if (_scrollY > _scrollMax) _scrollY = _scrollMax;
  }

  // ======================================================================
  // WiFi power & scan
  // ======================================================================
  void safeStopWifi(){
    int sc = WiFi.scanComplete();
    if (sc == -1) {
      WiFi.scanDelete();
      delay(1); yield();
    }
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    _scanning   = false;
    _scanCount  = -2;
    _scanStartAt= 0;
    _wifiLastOnAt = 0;
  }

  void applyWifiState(bool on){
    if (on){
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(false);

      // --- FIX: jangan pakai modem-sleep saat GUI jalan ---
      WiFi.setSleep(false);
      esp_wifi_set_ps(WIFI_PS_NONE);
      // ---------------------------------------------------

      delay(1); 
      yield();
      _wifiLastOnAt = millis();
    } else {
      safeStopWifi();
    }
    if (_qs) _qs->setWifiIcon(_wifiOn);
  }

  void startScan(){
    if (!_wifiOn) return;
    if (WiFi.scanComplete() == -1) return;

    WiFi.scanDelete();
    WiFi.disconnect();
    (void)WiFi.scanNetworks(true);
    _scanning  = true;
    _scanCount = -1;
    _listScrollY = 0;
  }

  // ======================================================================
  // Known networks (Preferences) & auto-connect
  // ======================================================================
  void loadKnownNetworks(){
    _knownCount = 0;
    uint8_t cnt = _prefs.getUChar("cnt", 0);
    if (cnt > MAX_KNOWN) cnt = MAX_KNOWN;
    for (uint8_t i=0;i<cnt;++i){
      char keyS[8]; char keyP[8];
      snprintf(keyS, sizeof(keyS), "s%u", i);
      snprintf(keyP, sizeof(keyP), "p%u", i);
      String ssid = _prefs.getString(keyS, "");
      String pw   = _prefs.getString(keyP, "");
      if (!ssid.length()) continue;
      _known[_knownCount].ssid = ssid;
      _known[_knownCount].pw   = pw;
      _knownCount++;
    }
  }

  void saveKnownNetworks(){
    _prefs.putUChar("cnt", (uint8_t)_knownCount);
    for (int i=0;i<_knownCount;++i){
      char keyS[8]; char keyP[8];
      snprintf(keyS, sizeof(keyS), "s%d", i);
      snprintf(keyP, sizeof(keyP), "p%d", i);
      _prefs.putString(keyS, _known[i].ssid);
      _prefs.putString(keyP, _known[i].pw);
    }
  }

  int findKnownIndex(const String& ssid) const{
    for (int i=0;i<_knownCount;++i){
      if (_known[i].ssid == ssid) return i;
    }
    return -1;
  }

  void rememberNetwork(const String& ssid, const char* pw){
    if (!ssid.length()) return;

    int idx = findKnownIndex(ssid);
    if (idx < 0){
      if (_knownCount < MAX_KNOWN){
        idx = _knownCount++;
      } else {
        // FIFO: geser kiri, buang yang paling lama
        for (int i=1;i<MAX_KNOWN;++i){
          _known[i-1] = _known[i];
        }
        idx = MAX_KNOWN - 1;
      }
    }

    _known[idx].ssid = ssid;
    _known[idx].pw   = (pw ? String(pw) : String(""));
    saveKnownNetworks();
  }

  void tryAutoConnectFromScan(){
    // Jangan ganggu user kalau:
    if (!_wifiOn) return;
    if (_scanCount <= 0) return;
    if (_knownCount <= 0) return;
    if (_cleanMode) return;                   // lagi di PwCard
    if (_connecting) return;                 // sudah ada proses connect
    if (WiFi.status() == WL_CONNECTED) return;

    int    bestScanIdx = -1;
    int    bestRssi    = -1000;
    String bestSsid;
    String bestPw;

    for (int i=0;i<_scanCount;++i){
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      int ki = findKnownIndex(s);
      if (ki < 0) continue;                 // SSID ini belum tersimpan
      int rssi = WiFi.RSSI(i);
      if (bestScanIdx < 0 || rssi > bestRssi){
        bestScanIdx = i;
        bestRssi    = rssi;
        bestSsid    = s;
        bestPw      = _known[ki].pw;
      }
    }

    if (bestScanIdx < 0) return;

    // Mulai connect ke jaringan tersimpan terkuat (non-blocking)
    WiFi.disconnect();
    delay(1); yield();

    _lastConnectPw = bestPw;
    if (bestPw.length() > 0) {
      WiFi.begin(bestSsid.c_str(), bestPw.c_str());
    } else {
      // open network
      WiFi.begin(bestSsid.c_str());
    }

    _connecting          = true;
    _connectFailed       = false;
    _connectStartMs      = millis();
    _lastWL              = WL_IDLE_STATUS;
    _connectionShownSsid = bestSsid;
    _connectPulseTil     = millis() + 300;
  }

  // ======================================================================
  // Primitive drawing + scissor helpers
  // ======================================================================
  static inline void fillRectClipped(GfxRGB888& g, int Rrx,int Rry,
                                     int x,int y,int w,int h,
                                     uint8_t R,uint8_t G,uint8_t B){
    int x0=imax(x,Rrx), y0=imax(y,Rry);
    int x1=imin(x+w, Rrx+g.w), y1=imin(y+h, Rry+g.h);
    if (x1<=x0 || y1<=y0) return;
    for (int gy=y0; gy<y1; ++gy){
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=R; row[1]=G; row[2]=B; row+=3; }
    }
  }

  static inline void fillRectScissor(GfxRGB888& g, int Rrx,int Rry,
                                     const Rect& clip,
                                     int x,int y,int w,int h,
                                     uint8_t R,uint8_t G,uint8_t B)
  {
    int x0=imax(imax(x,Rrx), clip.x);
    int y0=imax(imax(y,Rry), clip.y);
    int x1=imin(imin(x+w, Rrx+g.w), clip.x+clip.w);
    int y1=imin(imin(y+h, Rry+g.h), clip.y+clip.h);
    if (x1<=x0 || y1<=y0) return;
    for (int gy=y0; gy<y1; ++gy){
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){ row[0]=R; row[1]=G; row[2]=B; row+=3; }
    }
  }

  static inline void fillRoundedScissor(GfxRGB888& g, int Rrx,int Rry,
                                        const Rect& clip,
                                        const Rect& r, int rad,
                                        uint8_t rr,uint8_t gg,uint8_t bb)
  {
    if (r.w<=0 || r.h<=0) return;
    const int x0=r.x, y0=r.y, w=r.w, h=r.h;

    int drawTop = imax(y0, Rry);
    drawTop     = imax(drawTop, clip.y);
    int drawBot = imin(y0+h, Rry+g.h);
    drawBot     = imin(drawBot, clip.y+clip.h);
    if (drawBot <= drawTop) return;

    rad = iClamp(rad, 0, (imin(w,h))/2);

    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yLoc = gy - Rry;
      const int yy   = gy - y0;
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
      gx0     = imax(gx0, clip.x);
      int gx1 = imin(x0 + w - inset, Rrx + g.w);
      gx1     = imin(gx1, clip.x + clip.w);
      if (gx1 <= gx0) continue;

      uint8_t* row = g.pix + ((size_t)yLoc*g.w + (gx0 - Rrx))*3;
      for (int gx=gx0; gx<gx1; ++gx){ row[0]=rr; row[1]=gg; row[2]=bb; row+=3; }
    }
  }

  static inline void fillRoundedClipped(GfxRGB888& g, int Rrx, int Rry,
                                        const Rect& r, int rad,
                                        uint8_t rr,uint8_t gg,uint8_t bb)
  {
    Rect tileClip{ Rrx, Rry, g.w, g.h };
    fillRoundedScissor(g, Rrx, Rry, tileClip, r, rad, rr, gg, bb);
  }

  static inline void strokeRoundedScissor(GfxRGB888& g, int Rrx,int Rry,
                                          const Rect& clip,
                                          const Rect& r, int rad,
                                          uint8_t rr,uint8_t gg,uint8_t bb)
  {
    fillRoundedScissor(g, Rrx,Rry, clip, {r.x, r.y, r.w, 1}, 0, rr,gg,bb);
    fillRoundedScissor(g, Rrx,Rry, clip, {r.x, r.y+r.h-1, r.w, 1}, 0, rr,gg,bb);
    fillRoundedScissor(g, Rrx,Rry, clip, {r.x, r.y, 1, r.h}, 0, rr,gg,bb);
    fillRoundedScissor(g, Rrx,Rry, clip, {r.x+r.w-1, r.y, 1, r.h}, 0, rr,gg,bb);
  }

  // ======================================================================
  // Widgets (normal mode)
  // ======================================================================
  void drawRefresh(GfxRGB888& g, int Rrx,int Rry, int cx,int cy,int R, bool pulse){
    uint8_t br=85,bg=95,bb=110;
    if (_rfPressed || pulse){ br=u8(br+42); bg=u8(bg+42); bb=u8(bb+42); }
    fillCircleClipped(g, Rrx,Rry, cx,cy, R, br,bg,bb);

    static constexpr float K = 0.017453292519943295f;
    const float a0 = -40.0f*K, a1 = 240.0f*K;
    drawArcBand(g, Rrx,Rry, cx,cy, R-7, R-2, a0,a1, 235,235,240);

    const float ah = a1;
    const int hx = cx + (int)((R-1)*cosf(ah));
    const int hy = cy + (int)((R-1)*sinf(ah));
    fillTriangleClipped(g, Rrx,Rry,
      hx,hy,
      hx - (int)(10*cosf(ah-0.9f)), hy - (int)(10*sinf(ah-0.9f)),
      hx - (int)(10*cosf(ah+0.9f)), hy - (int)(10*sinf(ah+0.9f)),
      235,235,240);
  }

  void drawArcBand(GfxRGB888& g, int Rrx, int Rry,
                  int cx,int cy, int rIn,int rOut, float a0,float a1,
                  uint8_t R,uint8_t G,uint8_t B)
  {
    if (rOut<=0||rIn<0||rIn>rOut) return;
    if (a1<a0){ float t=a0;a0=a1;a1=t; }

    const int y0 = imax(cy+rOut*(-1), Rry);
    const int y1 = imin(cy+rOut, Rry+g.h-1);
    for (int gy=y0; gy<=y1; ++gy){
      const int dy = gy - cy;
      int dxO = (int)floorf(sqrtf((float)rOut*rOut - (float)dy*dy));
      int xlO = cx - dxO, xrO = cx + dxO;
      int x0 = imax(xlO, Rrx), x1 = imin(xrO, Rrx+g.w-1);
      if (x1 < x0) continue;

      uint8_t* p = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int x=x0; x<=x1; ++x){
        float ang = atan2f((float)(gy-cy),(float)(x-cx));
        while (ang < a0) ang += 2.0f*PI_F32;
        while (ang > a1) ang -= 2.0f*PI_F32;
        if (ang<a0 || ang>a1){ p+=3; continue; }
        int dx = x - cx;
        int rr = dx*dx + dy*dy;
        if (rr>=rIn*rIn && rr<=rOut*rOut){ *p++=R; *p++=G; *p++=B; }
        else                              { p+=3; }
      }
    }
  }

  void drawToggle(GfxRGB888& g, int Rrx,int Rry, int x,int y,int w,int h){
    const uint8_t offR=80, offG=85, offB=95;
    const uint8_t onR =210, onG =40, onB =45;
    uint8_t rr = lerp8(offR, onR, _knobT);
    uint8_t gg = lerp8(offG, onG, _knobT);
    uint8_t bb = lerp8(offB, onB, _knobT);
    if (_tgPressed){ rr=u8(rr+28); gg=u8(gg+28); bb=u8(bb+28); }

    Rect rail{ x, y, w, h };
    fillRoundedClipped(g, Rrx,Rry, rail, h/2, rr,gg,bb);

    const int knobPad = 4;
    const int d       = h - 2*knobPad;
    const int cx0     = x + knobPad + d/2;
    const int cx1     = x + w - knobPad - d/2;
    const int cy      = y + h/2;
    const int cx      = (int)lroundf(cx0 + (cx1 - cx0) * _knobT);
    int knobR         = d/2;
    if (_tgPressed)   knobR = imax(6, knobR-1);

    fillCircleClipped(g, Rrx,Rry, cx,cy, knobR, 245,245,247);
  }

  void drawText(GfxRGB888& g, int Rrx,int Rry,
                int x,int y, const char* txt, int scale, bool bold=false)
  {
    const int glyphH = 7*scale;
    const int xLoc   = x - Rrx;
    const int yBase  = (y - Rry) + glyphH;
    if (bold){
      SimpleFont::drawTextStyled(g, xLoc+1, yBase, txt, 255,255,255, scale, 1);
    }
    SimpleFont::drawTextStyled(g, xLoc,   yBase, txt, 255,255,255, scale, 1);
  }

  int drawWrapped(GfxRGB888& g, int Rrx,int Rry,
                  int x,int y,int w, const char* text, int scale, bool bold=false)
  {
    const int lineH  = 7*scale + 4;
    const int spaceW = SimpleFont::textWidth(" ", scale, 1);

    char lineBuf[256]; int lineLen=0; int lineW=0;
    auto flushLine = [&](bool evenIfEmpty=false){
      if (lineLen>0 || evenIfEmpty){
        lineBuf[lineLen]=0;
        drawText(g, Rrx,Rry, x, y, lineBuf, scale, bold);
        y += lineH;
        lineLen=0; lineW=0;
      }
    };

    const char* p = text;
    while (*p){
      if (*p=='\n'){ flushLine(true); ++p; continue; }

      const char* wStart = p;
      while (*p && *p!=' ' && *p!='\n') ++p;
      int wLen = (int)(p - wStart);

      char wbuf[128];
      int copyN = (wLen > (int)sizeof(wbuf)-1) ? (int)sizeof(wbuf)-1 : wLen;
      memcpy(wbuf, wStart, copyN); wbuf[copyN]=0;
      int wordW = SimpleFont::textWidth(wbuf, scale, 1);

      int addW = (lineLen==0 ? wordW : lineW + spaceW + wordW);

      if (addW <= w){
        if (lineLen!=0){ lineBuf[lineLen++]=' '; lineW += spaceW; }
        memcpy(lineBuf+lineLen, wbuf, copyN); lineLen += copyN;
        lineW = addW;
      } else {
        flushLine(true);
        memcpy(lineBuf, wbuf, copyN); lineLen = copyN;
        lineW = wordW;
        if (lineW > w){ flushLine(true); }
      }

      while (*p==' ') ++p;
    }
    flushLine(false);
    return y;
  }

  // ======================================================================
  // PwCard drawing & input
  // ======================================================================
  void drawButton(GfxRGB888& g, int Rrx,int Rry, const Rect& clip,
                  const Rect& r, const char* label, bool primary, bool hold)
  {
    uint8_t br = primary? 210:90;
    uint8_t bg = primary? 40 :95;
    uint8_t bb = primary? 45 :110;
    if (hold){ br=u8(br+28); bg=u8(bg+28); bb=u8(bb+28); }

    fillRoundedScissor(g, Rrx,Rry, clip, r, 10, br,bg,bb);

    int tw = SimpleFont::textWidth(label, 1, 1);
    int tx = r.x + (r.w - tw)/2;
    int ty = r.y + (r.h - 7)/2 - 1;
    drawText(g, Rrx,Rry, tx, ty, label, 1, true);
  }

  static inline const char* wlStatusStr(wl_status_t s){
    switch (s){
      case WL_IDLE_STATUS:     return "Idle";
      case WL_NO_SSID_AVAIL:   return "SSID not found";
      case WL_SCAN_COMPLETED:  return "Scan completed";
      case WL_CONNECTED:       return "Connected";
      case WL_CONNECT_FAILED:  return "Connect failed";
      case WL_CONNECTION_LOST: return "Connection lost";
      case WL_DISCONNECTED:    return "Disconnected";
      default:                 return "?";
    }
  }

  void drawPwCard(GfxRGB888& g, int Rrx, int Rry, uint32_t now){
    // Body
    fillRoundedScissor(g, Rrx,Rry, _pwCard, _pwCard, 14, 35,38,48);
    const Rect clip{ _pwCard.x+2, _pwCard.y+2, _pwCard.w-4, _pwCard.h-4 };

    // Texts & spacing
    const int padX = 14;
    int x0 = _pwCard.x + padX;
    int y  = _pwCard.y + 18;

    // "Connect to:"
    drawText(g, Rrx,Rry, x0, y, "Connect to:", 2, true);
    y += (7*2) + SP_CONNECT_TO__TO_SSID;

    // SSID (bold)
    const String ss = (_selectedSsid.length()? _selectedSsid : String("(select SSID)"));
    drawText(g, Rrx,Rry, x0, y, ss.c_str(), 2, true);
    y += (7*2) + SP_SSID__TO_PW_LABEL;

    // "Password"
    drawText(g, Rrx,Rry, x0, y, "Password", 1, false);
    y += (7*1) + SP_LABEL__TO_TEXTBOX;

    // Textbox
    fillRoundedScissor (g, Rrx,Rry, clip, _pwTextbox, PW_TEXTBOX_RADIUS, 26,28,38);
    strokeRoundedScissor(g, Rrx,Rry, clip, _pwTextbox, PW_TEXTBOX_RADIUS, 80,85,95);

    // Teks + cursor blink
    const int txPad = 10;
    int txtX = _pwTextbox.x + txPad;
    int txtY = _pwTextbox.y + 9;

    int scale = 2;
    while (SimpleFont::textWidth(_pwBuf, scale, 0) > (_pwTextbox.w - 2*txPad) && scale > 1) --scale;
    drawText(g, Rrx,Rry, txtX, txtY, _pwBuf, scale, false);

    if (_pwFocused || _vk.visible()) {
      bool on = ((now - _cursorBlinkMs) / 400) % 2 == 0;
      if (on) {
        int tw = SimpleFont::textWidth(_pwBuf, scale, 0);
        int cx = txtX + tw + 2;
        int cy = _pwTextbox.y + 6;
        int ch = _pwTextbox.h - 12;
        fillRectScissor(g, Rrx,Rry, clip, cx, cy, 2, ch, 240,240,240);
      }
    }

    // Status bar (connecting / fail message) tepat di bawah textbox
    int statusY = _pwTextbox.y + _pwTextbox.h + 6;
    if (_connecting) {
      // anim dots
      int dots = (int)((now - _connectStartMs)/400) % 4; // 0..3
      char msg[64];
      snprintf(msg, sizeof(msg), "Connecting%s", (dots==0?".":dots==1?"..":dots==2?"...":""));
      drawText(g, Rrx,Rry, x0, statusY, msg, 1, false);
    } else if (_connectFailed) {
      char msg[96];
      snprintf(msg, sizeof(msg), "Failed: %s", wlStatusStr(_lastWL));
      drawText(g, Rrx,Rry, x0, statusY, msg, 1, true);
    } else if (_lastWL == WL_CONNECTED && WiFi.SSID().length()>0 && _connectionShownSsid.length()>0 && WiFi.SSID()==_connectionShownSsid) {
      drawText(g, Rrx,Rry, x0, statusY, "Connected.", 1, true);
    }

    // Buttons
    bool pulse = (_connectPulseTil && (int32_t)(millis() - _connectPulseTil) < 0);
    drawButton(g, Rrx,Rry, clip, _btnCancel,  "Cancel",  false, _btnCancelHold);
    drawButton(g, Rrx,Rry, clip, _btnConnect, "Connect", true,  (_btnConnectHold || pulse));
  }

  void startConnect(){
    if (_selectedSsid.length()==0) return;

    // pastikan WiFi ON & STA
    if (!_wifiOn) {
      requestSetWifi(true); // ini akan set mode STA + jadwal scan
    } else {
      // jika sedang scan, hentikan
      if (_scanning || WiFi.scanComplete() == -1) {
        WiFi.scanDelete();
      }
    }

    // putuskan koneksi lama jika ada
    WiFi.disconnect();
    delay(1); yield();

    // simpan password yang dipakai saat ini
    _lastConnectPw = String(_pwBuf);

    // Mulai koneksi (non-blocking)
    if (_pwLen > 0) {
      // WPA/WPA2
      WiFi.begin(_selectedSsid.c_str(), _pwBuf);
    } else {
      // open network (tanpa password)
      WiFi.begin(_selectedSsid.c_str());
    }

    _connecting          = true;
    _connectFailed       = false;
    _connectStartMs      = millis();
    _lastWL              = WL_IDLE_STATUS;
    _connectionShownSsid = _selectedSsid;

    // kecil efek pulse di tombol Connect (dipakai di drawPwCard)
    _connectPulseTil = millis() + 300;
  }

  void cancelAndBackToList(){
    // Jika sedang mencoba connect → batalkan
    if (_connecting) {
      WiFi.disconnect();
      _connecting = false;
    }

    // Tutup VK & hilangkan fokus textbox
    _vk.hide();
    _vkWasVisible = false;
    _pwFocused    = false;

    // Kembali ke mode normal (tombol refresh, toggle, dan list SSID)
    _cleanMode  = false;
    _showPwCard = false;

    // Reset status / highlight PwCard
    _btnCancelHold  = false;
    _btnConnectHold = false;
    _pwPressing     = false;

    // Pastikan layout & scroll normal mode valid
    recomputeLayout();
    updateContentHeight();
  }

  void handlePwCardInput(bool pressed, int x, int y){
    const int cx = x;
    const int cy = y;

    if (pressed && !_pwPressing){
      _pwPressing = true;
      _pwDownX = cx;
      _pwDownY = cy;

      // 1) Textbox → fokus + buka VK
      if (ptInRect(_pwTextbox, cx, cy)) {
        _pwFocused      = true;
        _cursorBlinkMs  = millis();
        _vk.show();
        return;
      }

      // 2) Tombol Cancel → eksekusi LANGSUNG (seperti yang sudah jalan)
      if (ptInRect(_btnCancel, cx, cy)) {
        _btnCancelHold = true;         // untuk highlight satu frame
        cancelAndBackToList();         // tutup VK + keluar dari clean mode
        return;
      }

      // 3) Tombol Connect → eksekusi LANGSUNG
      if (ptInRect(_btnConnect, cx, cy)) {
        _btnConnectHold = true;        // highlight tombol
        _vk.hide();                    // tutup keyboard
        _pwFocused = false;            // lepas fokus textbox
        startConnect();                // MULAI proses koneksi (non-blocking)
        return;
      }

      // 4) Tap di luar textbox & tombol → hilangkan fokus textbox
      _pwFocused = false;
    }
    else if (pressed) {
      // (optional: drag di area PwCard kalau suatu saat mau)
    }
    else {
      // ====== RELEASE ======
      // Di mode ini Connect/Cancel SUDAH dieksekusi saat press.
      // Release hanya untuk bereskan state visual.

      // Kalau release di area kosong semuanya → matikan fokus textbox
      if (!ptInRect(_pwTextbox, cx, cy) &&
          !ptInRect(_btnCancel,  cx, cy) &&
          !ptInRect(_btnConnect, cx, cy)) {
        _pwFocused = false;
      }

      // Bersihkan flag hold & gesture
      _btnCancelHold  = false;
      _btnConnectHold = false;
      _pwPressing     = false;
    }
  }

  int pickIndexFromY(int absY){
    const int cy = absY;
    if (!ptInRect(_listViewport, _downX, cy)) return -1;
    const int itemH = 60;
    const int gapY  = 10;
    const int relY = (cy - _listViewport.y) + _listScrollY;
    if (relY < 0) return -1;
    const int step = itemH + gapY;
    const int idx  = relY / step;
    const int yIn  = relY - idx * step;
    if (idx >= 0 && idx < _scanCount && yIn >= 0 && yIn < itemH) return idx;
    return -1;
  }

  // ======================================================================
  // SSID list drawing (mode normal)
  // ======================================================================
  void drawSmallCard(GfxRGB888& g, int Rrx,int Rry,
                     const Rect& r, const Rect& scissor,
                     const String& ssid, int rssi, int enc, int ch,
                     bool highlight, bool connected)
  {
    // Warna dasar: normal = abu gelap, connected = biru
    uint8_t baseR, baseG, baseB;
    if (connected) {
      baseR = 40;  baseG = 90;  baseB = 160;  // biru
    } else {
      baseR = 35;  baseG = 38;  baseB = 48;   // default card
    }

    const uint8_t hiR  = u8(baseR+18);
    const uint8_t hiG  = u8(baseG+18);
    const uint8_t hiB  = u8(baseB+18);

    fillRoundedScissor(g, Rrx,Rry, scissor, r, 10,
                       highlight ? hiR : baseR,
                       highlight ? hiG : baseG,
                       highlight ? hiB : baseB);

    if (highlight) {
      fillRectScissor(g, Rrx,Rry, scissor, r.x, r.y, r.w, 1,  90,95,110);
      fillRectScissor(g, Rrx,Rry, scissor, r.x, r.y+r.h-1, r.w, 1, 90,95,110);
      fillRectScissor(g, Rrx,Rry, scissor, r.x, r.y, 1, r.h,  90,95,110);
      fillRectScissor(g, Rrx,Rry, scissor, r.x+r.w-1, r.y, 1, r.h, 90,95,110);
    }

    const int pad=10;
    int tx = r.x + pad;
    int ty = r.y + pad;

    // SSID
    ty = drawWrapped(g, Rrx,Rry, tx, ty, r.w - 2*pad, ssid.c_str(), 2, true);

    // Info bawah
    char ln[96];
    snprintf(ln,sizeof(ln), "RSSI: %ddBm  |  Ch: %d  |  %s", rssi, ch, encStr(enc));
    (void)drawWrapped(g, Rrx,Rry, tx, ty, r.w - 2*pad, ln, 1, false);
  }

  void drawMainCard(GfxRGB888& g, int Rrx,int Rry, const Rect& r){
    if (r.h<=0 || r.w<=0) return;

    fillRoundedClipped(g, Rrx,Rry, r, CARD_RADIUS, Theme::UI_R, Theme::UI_G, Theme::UI_B);

    const int pad = 12;
    Rect inner{ r.x+pad, r.y+pad, r.w-2*pad, r.h-2*pad };
    _inner = inner;

    // Clear inner (isi card)
    {
      const int top = imax(inner.y, Rry);
      const int bot = imin(inner.y + inner.h, Rry + g.h);
      for (int gy=top; gy<bot; ++gy){
        int x0 = imax(inner.x, Rrx);
        int x1 = imin(inner.x + inner.w, Rrx + g.w);
        if (x1 <= x0) continue;
        uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
        for (int gx=x0; gx<x1; ++gx){ row[0]=26; row[1]=28; row[2]=38; row+=3; }
      }
    }

    int y = inner.y;

    // Status connected jika ada
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID().length() > 0) {
      char connected[64];
      snprintf(connected, sizeof(connected), "Connected to: %s", WiFi.SSID().c_str());
      y = drawWrapped(g, Rrx,Rry, inner.x, y, inner.w, connected, 2, true);
    } else {
      y = drawWrapped(g, Rrx,Rry, inner.x, y,
                      inner.w,
                      _wifiOn ? "WiFi: ON (STA mode, modem-sleep enabled)"
                              : "WiFi: OFF (radio disabled to save power)",
                      2, true);
    }

    y += 8;

    if (_wifiOn){
      const char* note =
        _scanning               ? "Scanning... please wait."
      : (_scanCount>=0)         ? "Tap refresh to rescan."
      : (_scanStartAt!=0)       ? "Starting scan shortly..."
                                : (WiFi.status()==WL_CONNECTED ? " " : "Tap refresh to scan.");
      y = drawWrapped(g, Rrx,Rry, inner.x, y, inner.w, note, 1, false);
    } else {
      y = drawWrapped(g, Rrx,Rry, inner.x, y, inner.w,
                      "Turn WiFi ON to scan available networks.", 1, false);
    }
    y += 10;

    Rect listVP{ inner.x, y, inner.w, inner.h - (y - inner.y) };
    if (listVP.h < 0) listVP.h = 0;
    _listViewport = listVP;

    const int itemH = 60;
    const int gapY  = 10;
    int totalH = 0;

    if (_wifiOn && _scanCount == 0){
      (void)drawWrapped(g, Rrx,Rry, inner.x, y, inner.w, "No networks found.", 1, false);
      totalH = 7 + 4;
    } else if (_wifiOn && _scanCount > 0){
      totalH = _scanCount * itemH + (_scanCount-1)*gapY;
    }

    _listScrollMax = 0;
    if (totalH > listVP.h) _listScrollMax = totalH - listVP.h;
    if (_listScrollY > _listScrollMax) _listScrollY = _listScrollMax;
    if (_listScrollY < 0) _listScrollY = 0;

    if (_wifiOn && _scanCount > 0 && listVP.h > 0){
      int baseY = y - _listScrollY;
      const bool wifiIsConnected = (WiFi.status() == WL_CONNECTED && WiFi.SSID().length() > 0);
      const String curSsid = wifiIsConnected ? WiFi.SSID() : String("");

      for (int i=0;i<_scanCount;++i){
        Rect sr{ listVP.x, baseY, listVP.w, itemH };
        const bool intersectsVP   = !(sr.y+sr.h <= listVP.y || sr.y >= listVP.y+listVP.h);
        const bool intersectsTile = !(sr.y+sr.h <= Rry      || sr.y >= Rry+g.h);
        if (intersectsVP && intersectsTile){
          String s = WiFi.SSID(i);
          if (s.length()==0) s = "(hidden)";
          const bool isHi        = (_ssidPressing && _ssidPressedIdx == i);
          const bool isConnected = (wifiIsConnected && (curSsid == s));

          drawSmallCard(g, Rrx,Rry, sr, listVP,
                        s, WiFi.RSSI(i), WiFi.encryptionType(i), WiFi.channel(i),
                        isHi, isConnected);
        }
        baseY += itemH + gapY;
      }
    }
  }

  // ======================================================================
  // Encodes
  // ======================================================================
  static const char* encStr(int t){
    switch ((wifi_auth_mode_t)t){
      case WIFI_AUTH_OPEN:            return "Open";
      case WIFI_AUTH_WEP:             return "WEP";
      case WIFI_AUTH_WPA_PSK:         return "WPA";
      case WIFI_AUTH_WPA2_PSK:        return "WPA2";
      case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
      case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
      case WIFI_AUTH_WPA3_PSK:        return "WPA3";
      case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
      default:                        return "?";
    }
  }

  // ======================================================================
  // Circle/triangle helpers
  // ======================================================================
  static inline void fillCircleClipped(GfxRGB888& g, int Rrx,int Rry,
                                       int cx,int cy,int r,
                                       uint8_t R,uint8_t G,uint8_t B)
  {
    const int y0 = imax(cy-r, Rry);
    const int y1 = imin(cy+r, Rry+g.h-1);
    for (int gy=y0; gy<=y1; ++gy){
      const int dy = gy - cy;
      int dx = (int)floorf(sqrtf((float)r*r - (float)dy*dy));
      int xl = imax(cx-dx, Rrx);
      int xr = imin(cx+dx, Rrx+g.w-1);
      if (xr < xl) continue;
      uint8_t* p = g.pix + ((size_t)(gy - Rry)*g.w + (xl - Rrx))*3;
      for (int x=xl; x<=xr; ++x){ *p++=R; *p++=G; *p++=B; }
    }
  }

  static inline void fillTriangleClipped(GfxRGB888& g, int Rrx,int Rry,
                                         int x1,int y1,int x2,int y2,int x3,int y3,
                                         uint8_t R,uint8_t G,uint8_t B)
  {
    auto sw=[&](int& a,int& b){int t=a;a=b;b=t;};
    if (y2<y1){ sw(x1,x2); sw(y1,y2); }
    if (y3<y1){ sw(x1,x3); sw(y1,y3); }
    if (y3<y2){ sw(x2,x3); sw(y2,y3); }

    auto span=[&](int y,int xa,int xb){
      if (y<Rry || y>=Rry+g.h) return;
      if (xa>xb) {int t=xa; xa=xb; xb=t;}
      xa = imax(xa, Rrx);
      xb = imin(xb, Rrx+g.w-1);
      if (xb<xa) return;
      uint8_t* p = g.pix + ((size_t)(y - Rry)*g.w + (xa - Rrx))*3;
      for (int x=xa; x<=xb; ++x){ *p++=R; *p++=G; *p++=B; }
    };
    auto lerpX=[](int y0,int y1,int x0,int x1,int y)->int{
      if (y1==y0) return x0; return x0 + (int)((int64_t)(x1-x0)*(y-y0)/(y1-y0));
    };

    int y=y1;
    for (; y<=y2; ++y){ span(y, lerpX(y1,y3,x1,x3,y), lerpX(y1,y2,x1,x2,y)); }
    for (; y<=y3; ++y){ span(y, lerpX(y1,y3,x1,x3,y), lerpX(y2,y3,x2,x3,y)); }
  }

  // ======================================================================
  // Anim helpers
  // ======================================================================
  void beginToggleAnim(bool on){
    _animFrom    = _knobT;
    _animTo      = on ? 1.0f : 0.0f;
    _animStartMs = millis();
    _animating   = true;
  }

  // ======================================================================
  // QS trampolines
  // ======================================================================
  static bool _qsGetWifi(void* ctx){
    return reinterpret_cast<PageWifi*>(ctx)->_wifiOn;
  }
  static void _qsSetWifi(bool on, void* ctx){
    reinterpret_cast<PageWifi*>(ctx)->requestSetWifi(on);
  }
};
