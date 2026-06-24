#pragma once
#include "PageBase.h"
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <RP_TFTDisplay.h>
#include "pins_and_config.h"
#include "rtc.h"


/*
  PageClockDS3231 – v1.3
  - Menggunakan PinsAndConfig::RTC untuk SDA/SCL/ADDR.
  - Mengandalkan WiFi yang sudah diurus PageWifi (global WiFi STA).
  - Tombol "SYNC FROM NTP" dieksekusi LANGSUNG saat press pertama di tombol:
      1) Cek WiFi.status().
      2) configTime(0,0, ...) + tunggu NTP valid (non-blocking).
      3) Pilih boundary 10 detik ke depan, lalu tulis UTC ke DS3231.
  - Konten muat 1 layar → tidak pakai scroll khusus.
*/

class PageClockDS3231 : public PageBase {
public:
  void begin(AppOS* os, TopBar* top){
    PageBase::begin(os, top);
    setTitle("CLOCK DS3231");

    // I2C bus sama dengan Touch/IMU/BMP/SHT, OS sudah Wire.begin(SDA,SCL).
    Wire.setClock(PinsAndConfig::IMU::I2C_HZ); // 400 kHz

    _statusLine = "Tap tombol di bawah untuk sync RTC dari NTP.";
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

protected:
  // ======================================================================
  // Render
  // ======================================================================
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const uint32_t nowMs = millis();

    // Step state sync & baca RTC berkala
    stepSyncMachine(nowMs);
    updateRtcCache(nowMs);

    // Gambar card utama
    drawCard(g, Rrx, Rry);
  }

  // ======================================================================
  // Input
  // ======================================================================
  void handleContentInput(bool pressed, int x, int y) override {
    PageBase::handleContentInput(pressed, x, y);

    const int cx = x;
    const int cy = y;   // tidak ada page scroll khusus di page ini

    const bool inBtn = ptInRect(_btnSync, cx, cy);

    if (pressed) {
      if (!_pressing){
        _pressing    = true;
        _btnSyncHold = inBtn;
        if (inBtn){
          // EKSEKUSI LANGSUNG SAAT PRESS PERTAMA
          onSyncButtonTapped();
        }
      } else {
        // drag keluar tombol → hilangkan highlight
        if (!inBtn) _btnSyncHold = false;
      }
    } else {
      // RELEASE: hanya bersihkan visual, aksi sudah dilakukan di PRESS
      _pressing    = false;
      _btnSyncHold = false;
    }
  }

private:
  // ======================================================================
  // Helpers: types & math
  // ======================================================================
  struct Rect { int x=0, y=0, w=0, h=0; };

  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }
  static inline bool ptInRect(const Rect& r,int x,int y){
    return (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h);
  }

  // BCD & waktu
  static uint8_t toBCD(uint8_t v){ return ((v/10)<<4) | (v%10); }
  static uint8_t fromBCD(uint8_t b){ return (10*((b>>4)&0x0F)) + (b&0x0F); }
  static void epochToUTC(time_t t, tm &out){ gmtime_r(&t, &out); }

  // ======================================================================
  // Theme & layout
  // ======================================================================
  using Theme = PinsAndConfig::Theme;
  static constexpr int EDGE        = Theme::EDGE_BUF;
  static constexpr int TITLE2CARD  = 10;
  static constexpr int CARD_RADIUS = Theme::BOX_RADIUS;
  static constexpr int BOT_PAD     = 20;

  Rect _card{};
  Rect _inner{};
  Rect _btnSync{};

  int  _contentH = 0;

  void recomputeLayout(){
    const int cardX = EDGE;
    const int cardY = _viewportY0 + TITLE2CARD;
    const int cardW = TFT_WIDTH - 2*EDGE;
    int cardH = (_viewportY0 + _viewportH) - cardY - BOT_PAD;
    if (cardH < 140) cardH = 140;
    _card = { cardX, cardY, cardW, cardH };

    const int pad = 12;
    _inner = { _card.x+pad, _card.y+pad, _card.w-2*pad, _card.h-2*pad };

    const int btnH = 36;
    _btnSync = {
      _inner.x,
      _inner.y + _inner.h - btnH,
      _inner.w,
      btnH
    };
  }

  void updateContentHeight(){
    const int bottom = _card.y + _card.h + BOT_PAD;
    _contentH = bottom;
  }

  // ======================================================================
  // Low-level drawing
  // ======================================================================
  static void fillRectClipped(GfxRGB888& g, int Rrx,int Rry,
                              int x,int y,int w,int h,
                              uint8_t R,uint8_t G,uint8_t B)
  {
    int x0 = imax(x, Rrx);
    int y0 = imax(y, Rry);
    int x1 = imin(x+w, Rrx + g.w);
    int y1 = imin(y+h, Rry + g.h);
    if (x1 <= x0 || y1 <= y0) return;

    for (int gy=y0; gy<y1; ++gy){
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx=x0; gx<x1; ++gx){
        row[0]=R; row[1]=G; row[2]=B;
        row += 3;
      }
    }
  }

  static void fillRoundedClipped(GfxRGB888& g, int Rrx,int Rry,
                                 const Rect& r, int rad,
                                 uint8_t R,uint8_t G,uint8_t B)
  {
    if (r.w<=0 || r.h<=0) return;
    rad = (rad < 0) ? 0 : rad;
    if (rad > r.h/2) rad = r.h/2;
    if (rad > r.w/2) rad = r.w/2;

    const int x0 = r.x;
    const int y0 = r.y;
    const int w  = r.w;
    const int h  = r.h;

    const int drawTop = imax(y0, Rry);
    const int drawBot = imin(y0+h, Rry+g.h);
    if (drawBot <= drawTop) return;

    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yy = gy - y0;
      int inset = 0;

      if (rad > 0){
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

      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (gx0 - Rrx))*3;
      for (int gx=gx0; gx<gx1; ++gx){
        row[0]=R; row[1]=G; row[2]=B;
        row += 3;
      }
    }
  }

  static void drawText(GfxRGB888& g, int Rrx,int Rry,
                       int x,int y, const char* txt,
                       int scale, bool bold=false)
  {
    const int glyphH = 7*scale;
    const int xLoc   = x - Rrx;
    const int yBase  = (y - Rry) + glyphH;
    if (bold){
      SimpleFont::drawTextStyled(g, xLoc+1, yBase,
                                 txt, 255,255,255, scale, 1);
    }
    SimpleFont::drawTextStyled(g, xLoc,   yBase,
                               txt, 255,255,255, scale, 1);
  }

  int drawWrapped(GfxRGB888& g, int Rrx,int Rry,
                  int x,int y,int w,
                  const char* text, int scale, bool bold=false)
  {
    if (!text) return y;
    const int lineH  = 7*scale + 4;
    const int spaceW = SimpleFont::textWidth(" ", scale, 1);

    char lineBuf[256];
    int  lineLen=0;
    int  lineW =0;

    auto flushLine = [&](bool evenIfEmpty=false){
      if (lineLen>0 || evenIfEmpty){
        lineBuf[lineLen] = 0;
        drawText(g, Rrx,Rry, x, y, lineBuf, scale, bold);
        y += lineH;
        lineLen = 0;
        lineW   = 0;
      }
    };

    const char* p = text;
    while (*p){
      if (*p == '\n'){
        flushLine(true);
        ++p;
        continue;
      }

      const char* wStart = p;
      while (*p && *p!=' ' && *p!='\n') ++p;
      int wLen = (int)(p - wStart);

      char wbuf[128];
      int copyN = (wLen > (int)sizeof(wbuf)-1) ? (int)sizeof(wbuf)-1 : wLen;
      memcpy(wbuf, wStart, copyN);
      wbuf[copyN] = 0;
      int wordW = SimpleFont::textWidth(wbuf, scale, 1);

      int addW = (lineLen==0 ? wordW : lineW + spaceW + wordW);
      if (addW <= w){
        if (lineLen != 0){
          lineBuf[lineLen++]=' ';
          lineW += spaceW;
        }
        memcpy(lineBuf+lineLen, wbuf, copyN);
        lineLen += copyN;
        lineW    = addW;
      } else {
        flushLine(true);
        memcpy(lineBuf, wbuf, copyN);
        lineLen = copyN;
        lineW   = wordW;
        if (lineW > w){
          flushLine(true);
        }
      }

      while (*p==' ') ++p;
    }
    flushLine(false);
    return y;
  }

  void drawButton(GfxRGB888& g, int Rrx,int Rry,
                  const Rect& r, const char* label, bool hold)
  {
    uint8_t r0 = 210, g0 = 40, b0 = 45; // primary
    if (hold){
      r0 = (uint8_t)imax(0, imin(255, r0 + 30));
      g0 = (uint8_t)imax(0, imin(255, g0 + 30));
      b0 = (uint8_t)imax(0, imin(255, b0 + 30));
    }
    fillRoundedClipped(g, Rrx,Rry, r, 10, r0,g0,b0);

    int tw = SimpleFont::textWidth(label, 1, 1);
    int tx = r.x + (r.w - tw)/2;
    int ty = r.y + (r.h - 7)/2 - 1;
    drawText(g, Rrx,Rry, tx, ty, label, 1, true);
  }

  void drawCard(GfxRGB888& g, int Rrx,int Rry){
    // Background card
    fillRoundedClipped(g, Rrx,Rry, _card, CARD_RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    // Clear inner
    {
      const int top = imax(_inner.y, Rry);
      const int bot = imin(_inner.y + _inner.h, Rry + g.h);
      for (int gy = top; gy < bot; ++gy){
        int x0 = imax(_inner.x, Rrx);
        int x1 = imin(_inner.x + _inner.w, Rrx + g.w);
        if (x1 <= x0) continue;
        uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
        for (int gx = x0; gx < x1; ++gx){
          row[0] = 26; row[1] = 28; row[2] = 38;
          row += 3;
        }
      }
    }

    int y = _inner.y + 4;
    const int x = _inner.x;
    const int w = _inner.w;

    // Title dalam card
    y = drawWrapped(g, Rrx,Rry, x, y, w, "RTC DS3231 (UTC)", 2, true);
    y += 6;

    // WiFi status
    String wifiLine;
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID().length() > 0){
      wifiLine = "WiFi: CONNECTED (" + WiFi.SSID() + ")";
    } else {
      wifiLine = "WiFi: NOT CONNECTED";
    }
    y = drawWrapped(g, Rrx,Rry, x, y, w, wifiLine.c_str(), 1, false);
    y += 4;

    // RTC waktu
    char rtcBuf[80];
    if (_rtcValid){
      snprintf(rtcBuf, sizeof(rtcBuf),
               "RTC UTC: %04d-%02d-%02d  %02d:%02d:%02d",
               _rtcTm.tm_year + 1900,
               _rtcTm.tm_mon + 1,
               _rtcTm.tm_mday,
               _rtcTm.tm_hour,
               _rtcTm.tm_min,
               _rtcTm.tm_sec);
    } else {
      snprintf(rtcBuf, sizeof(rtcBuf),
               "RTC: belum terbaca / tidak ada respon.");
    }
    y = drawWrapped(g, Rrx,Rry, x, y, w, rtcBuf, 1, false);
    y += 4;

    // Status sync
    y = drawWrapped(g, Rrx,Rry, x, y, w, _statusLine.c_str(), 1, false);
    y += 8;

    // Tombol sync (posisi fix pada _btnSync)
    drawButton(g, Rrx,Rry, _btnSync, "SYNC FROM NTP", _btnSyncHold);
  }

  void updateRtcCache(uint32_t nowMs){
    const uint32_t RTC_READ_INTERVAL_MS = 500; // baca tiap 0.5s
    if ((nowMs - _lastRtcReadMs) < RTC_READ_INTERVAL_MS) return;
    _lastRtcReadMs = nowMs;

    uint32_t epoch = RtcTime::now();
    if (epoch == 0) {
      _rtcValid = false;
      return;
    }

    time_t e = (time_t)epoch;
    tm t{};
    gmtime_r(&e, &t);   // epoch UTC -> struct tm UTC

    _rtcTm    = t;
    _rtcValid = true;
  }

  // ======================================================================
  // NTP sync state machine (non-blocking)
  // ======================================================================
  void onSyncButtonTapped(){
    if (_syncActive){
      // sedang proses, abaikan
      return;
    }

    if (WiFi.status() != WL_CONNECTED){
      _statusLine = "WiFi belum terhubung.\nSilakan connect dulu di halaman WiFi.";
      return;
    }

    // Mulai NTP sekali (pakai koneksi WiFi global dari PageWifi)
    configTime(0, 0,
      "pool.ntp.org",
      "time.google.com",
      "time.cloudflare.com");

    _syncActive   = true;
    _syncState    = 1;  // tunggu NTP valid
    _ntpStartMs   = millis();
    _statusLine   = "Sync NTP...";
  }

  void stepSyncMachine(uint32_t nowMs){
    if (!_syncActive) return;

    if (_syncState == 1){
      // Tunggu NTP valid (time() > 2023-11-14 ±)
      time_t now = 0;
      time(&now);
      if (now > 1700000000){
        // NTP OK → pilih boundary 10 detik berikutnya + 2 slot (seperti contohmu)
        _targetEpoch = ((now / 10) + 2) * 10;
        _syncState   = 2;
        _statusLine  = "NTP OK.\nMenunggu boundary 10 detik untuk set RTC...";
      } else if (nowMs - _ntpStartMs > 15000){
        _syncActive  = false;
        _syncState   = 0;
        _statusLine  = "Gagal sync NTP (timeout).";
      }
    }
    else if (_syncState == 2){
      // Menunggu sampai epoch >= targetEpoch
      time_t now = 0;
      time(&now);
      if (now >= _targetEpoch){
        // Set DS3231 via backend tunggal
        RtcTime::setFromEpoch((uint32_t)_targetEpoch);

        _syncActive    = false;
        _syncState     = 0;
        _lastSyncEpoch = _targetEpoch;

        char buf[96];
        snprintf(buf, sizeof(buf),
                "RTC sudah di-set dari NTP.\nTarget epoch = %ld.",
                (long)_targetEpoch);
        _statusLine = buf;
      }
    }
  }

  // ======================================================================
  // Members
  // ======================================================================
  // Input/button state
  bool _pressing    = false;
  bool _btnSyncHold = false;

  // RTC cache
  bool     _rtcValid       = false;
  tm       _rtcTm{};
  uint32_t _lastRtcReadMs  = 0;

  // NTP sync
  bool     _syncActive     = false;
  uint8_t  _syncState      = 0;   // 0=idle,1=wait NTP,2=wait boundary
  uint32_t _ntpStartMs     = 0;
  time_t   _targetEpoch    = 0;
  time_t   _lastSyncEpoch  = 0;

  // UI text
  String   _statusLine;
};
