#pragma once
#include "PageBase.h"
#include "pins_and_config.h"
#include "RP_batteryVD.h"
#include <math.h>
#include <string.h>

/*
  PageBattery – debug backend pembacaan baterai (multi-tile friendly)

  - Layout mirip card seperti PageClockDS3231 (card di dalam viewport).
  - Render pakai global coords + clipping (Rrx,Rry) → aman untuk multi-tile.
  - Tidak repeat "per tile", tiap tile hanya menggambar bagian card yang
    memang jatuh di tile tersebut.
  - Tampilkan:
      * State (CHARGING / DISCHARGING / USB ONLY / UNKNOWN)
      * sense_mV, vbat_mV, vbat_filt_mV
      * soc_true_pct, soc_ui_pct, soc_disp_int
      * remain_mAh
  - Sampling baterai ~1x per detik.
*/

class PageBattery : public PageBase {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("BATTERY");

    using BCFG = PinsAndConfig::BatteryVD;

    RPBVD_Config cfg;

    // ---- Pin dari PinsAndConfig::BatteryVD ----
    cfg.pin_adc             = BCFG::PIN_ADC;
    cfg.pin_enable          = BCFG::PIN_EN;
    cfg.enable_active_high  = BCFG::EN_ACTIVE_HIGH;

    // ---- ADC generic (ESP32 pakai analogReadMilliVolts) ----
    cfg.adc_resolution_bits = 12;
    cfg.adc_ref_mV          = 3300;

    // ---- Divider & kalibrasi ----
    cfg.divider_gain        = BCFG::DIV_GAIN;
    cfg.gain_corr           = BCFG::GAIN_CORR;
    cfg.offset_corr_mV      = BCFG::OFFSET_mV;

    // ---- Range SOC (display) ----
    // Pastikan di PinsAndConfig:
    //   VMIN_DISPLAY_mV = 3000; VMAX_DISPLAY_mV = 3900;
    cfg.vmin_display_mV     = BCFG::VMIN_DISPLAY_mV;
    cfg.vmax_display_mV     = BCFG::VMAX_DISPLAY_mV;

    // ---- Clamp aman ----
    cfg.vclip_min_mV        = 2500;
    cfg.vclip_max_mV        = 4500;

    // ---- Mode SOC: LINEAR (0% di vmin, 100% di vmax) ----
    cfg.mode                = RPBVD_SocMode::LINEAR;
    cfg.lut                 = nullptr;
    cfg.lut_n               = 0;

    // ---- Smoothing & feel angka ----
    cfg.ema_alpha_v         = 0.25f;
    cfg.ema_alpha_soc       = 0.20f;
    cfg.display_gamma       = 0.78f;
    cfg.hysteresis_pct      = 0.4f;
    cfg.min_step_ms         = 4000;

    // ---- Kapasitas baterai ----
    cfg.full_capacity_mAh   = BCFG::FULL_mAh;

    _batt.begin(cfg);

    // Baca awal supaya filter punya state
    _batt.firstMeasure(_reading);
    _lastSampleMs      = millis();
    _prevFiltMv        = (int)lroundf(_reading.vbat_filt_mV);
    _lastStateUpdateMs = _lastSampleMs;
    _status            = Status::Unknown;

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
    const uint32_t now = millis();

    // Sampling backend ~1 Hz, terpisah dari tile rendering
    if ((now - _lastSampleMs) >= 1000) {
      _lastSampleMs = now;
      RPBVD_Reading r{};
      _batt.sample(r);
      _reading = r;
      updateStatusFromSample(now);
    }

    // Gambar card sekali (per tile akan di-clip via Rrx,Rry)
    drawCard(g, Rrx, Rry);
  }

private:
  // ======================================================================
  // Helpers: types & math
  // ======================================================================
  struct Rect { int x=0, y=0, w=0, h=0; };

  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }

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

  int  _contentH = 0;

  void recomputeLayout() {
    const int cardX = EDGE;
    const int cardY = _viewportY0 + TITLE2CARD;
    const int cardW = TFT_WIDTH - 2*EDGE;
    int cardH = (_viewportY0 + _viewportH) - cardY - BOT_PAD;
    if (cardH < 140) cardH = 140;

    _card = { cardX, cardY, cardW, cardH };

    const int pad = 12;
    _inner = { _card.x + pad, _card.y + pad, _card.w - 2*pad, _card.h - 2*pad };
  }

  void updateContentHeight() {
    const int bottom = _card.y + _card.h + BOT_PAD;
    _contentH = bottom;
  }

  // ======================================================================
  // Low-level drawing (clip-aware, multi-tile safe)
  // ======================================================================
  static void fillRectClipped(GfxRGB888& g, int Rrx,int Rry,
                              int x,int y,int w,int h,
                              uint8_t R,uint8_t G,uint8_t B)
  {
    int x0 = imax(x, Rrx);
    int y0 = imax(y, Rry);
    int x1 = imin(x + w, Rrx + g.w);
    int y1 = imin(y + h, Rry + g.h);
    if (x1 <= x0 || y1 <= y0) return;

    for (int gy = y0; gy < y1; ++gy) {
      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx = x0; gx < x1; ++gx) {
        row[0] = R; row[1] = G; row[2] = B;
        row += 3;
      }
    }
  }

  static void fillRoundedClipped(GfxRGB888& g, int Rrx,int Rry,
                                 const Rect& r, int rad,
                                 uint8_t R,uint8_t G,uint8_t B)
  {
    if (r.w <= 0 || r.h <= 0) return;
    rad = (rad < 0) ? 0 : rad;
    if (rad > r.h/2) rad = r.h/2;
    if (rad > r.w/2) rad = r.w/2;

    const int x0 = r.x;
    const int y0 = r.y;
    const int w  = r.w;
    const int h  = r.h;

    const int drawTop = imax(y0, Rry);
    const int drawBot = imin(y0 + h, Rry + g.h);
    if (drawBot <= drawTop) return;

    for (int gy = drawTop; gy < drawBot; ++gy) {
      const int yy = gy - y0;
      int inset = 0;

      if (rad > 0) {
        if (yy < rad) {
          float dy = float(rad - 1 - yy);
          float dx = sqrtf(float(rad)*rad - dy*dy);
          inset = int(rad - floorf(dx));
        }
        int y2 = (h - 1 - yy);
        if (y2 < rad) {
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
      for (int gx = gx0; gx < gx1; ++gx) {
        row[0] = R; row[1] = G; row[2] = B;
        row += 3;
      }
    }
  }

  static void drawText(GfxRGB888& g, int Rrx,int Rry,
                       int x,int y, const char* txt,
                       int scale,
                       uint8_t R=255,uint8_t G=255,uint8_t B=255,
                       bool bold=false)
  {
    if (!txt) return;

    const int glyphH = 7 * scale;
    const int xLoc   = x - Rrx;
    const int yBase  = (y - Rry) + glyphH;

    if (bold) {
      SimpleFont::drawTextStyled(g, xLoc + 1, yBase,
                                 txt, R,G,B, scale, 1);
    }
    SimpleFont::drawTextStyled(g, xLoc,     yBase,
                               txt, R,G,B, scale, 1);
  }

  static int drawWrapped(GfxRGB888& g, int Rrx,int Rry,
                         int x,int y,int w,
                         const char* text, int scale,
                         uint8_t R=255,uint8_t G=255,uint8_t B=255,
                         bool bold=false)
  {
    if (!text) return y;

    const int lineH  = 7*scale + 4;
    const int spaceW = SimpleFont::textWidth(" ", scale, 1);

    char lineBuf[256];
    int  lineLen = 0;
    int  lineW   = 0;

    auto flushLine = [&](bool evenIfEmpty){
      if (lineLen > 0 || evenIfEmpty) {
        lineBuf[lineLen] = 0;
        drawText(g, Rrx,Rry, x, y, lineBuf, scale, R,G,B, bold);
        y += lineH;
        lineLen = 0;
        lineW   = 0;
      }
    };

    const char* p = text;
    while (*p) {
      if (*p == '\n') {
        flushLine(true);
        ++p;
        continue;
      }

      const char* wStart = p;
      while (*p && *p != ' ' && *p != '\n') ++p;
      int wLen = (int)(p - wStart);

      char wbuf[128];
      int copyN = (wLen > (int)sizeof(wbuf) - 1) ? (int)sizeof(wbuf) - 1 : wLen;
      memcpy(wbuf, wStart, copyN);
      wbuf[copyN] = 0;
      int wordW = SimpleFont::textWidth(wbuf, scale, 1);

      int addW = (lineLen == 0 ? wordW : lineW + spaceW + wordW);
      if (addW <= w) {
        if (lineLen != 0) {
          lineBuf[lineLen++] = ' ';
          lineW += spaceW;
        }
        memcpy(lineBuf + lineLen, wbuf, copyN);
        lineLen += copyN;
        lineW    = addW;
      } else {
        // line break
        flushLine(true);
        memcpy(lineBuf, wbuf, copyN);
        lineLen = copyN;
        lineW   = wordW;
        if (lineW > w) {
          // word too long, flush as own line
          flushLine(true);
        }
      }

      while (*p == ' ') ++p;
    }
    flushLine(false);
    return y;
  }

  void drawCard(GfxRGB888& g, int Rrx,int Rry) {
    // Background card
    fillRoundedClipped(g, Rrx,Rry, _card, CARD_RADIUS,
                       Theme::UI_R, Theme::UI_G, Theme::UI_B);

    // Clear inner (panel dalam card)
    const int top = imax(_inner.y, Rry);
    const int bot = imin(_inner.y + _inner.h, Rry + g.h);
    for (int gy = top; gy < bot; ++gy) {
      int x0 = imax(_inner.x, Rrx);
      int x1 = imin(_inner.x + _inner.w, Rrx + g.w);
      if (x1 <= x0) continue;

      uint8_t* row = g.pix + ((size_t)(gy - Rry)*g.w + (x0 - Rrx))*3;
      for (int gx = x0; gx < x1; ++gx) {
        row[0] = 26; row[1] = 28; row[2] = 38; // dark inner
        row += 3;
      }
    }

    int y = _inner.y + 4;
    const int x = _inner.x;
    const int w = _inner.w;

    // Title dalam card
    y = drawWrapped(g, Rrx,Rry, x, y, w,
                    "Battery monitor (debug)", 2,
                    255,255,0, true);
    y += 6;

    // Status string
    char statusBuf[64];
    snprintf(statusBuf, sizeof(statusBuf), "State: %s",
             (_status == Status::Charging)    ? "CHARGING" :
             (_status == Status::Discharging) ? "DISCHARGING" :
             (_status == Status::UsbOnly)     ? "USB ONLY" :
                                                "UNKNOWN");
    y = drawWrapped(g, Rrx,Rry, x, y, w,
                    statusBuf, 1,
                    255,255,255, false);
    y += 4;

    // Data backend
    char buf[192];
    snprintf(buf, sizeof(buf),
             "sense_mV     : %d\n"
             "vbat_mV      : %d\n"
             "vbat_filt_mV : %.1f\n"
             "soc_true_pct : %.1f\n"
             "soc_ui_pct   : %.1f\n"
             "soc_disp_int : %d\n"
             "remain_mAh   : %.0f",
             _reading.sense_mV,
             _reading.vbat_mV,
             _reading.vbat_filt_mV,
             _reading.soc_true_pct,
             _reading.soc_ui_pct,
             _reading.soc_disp_int,
             _reading.remain_mAh);

    y = drawWrapped(g, Rrx,Rry, x, y, w,
                    buf, 1,
                    255,255,255, false);
  }

  // ======================================================================
  // Status detection (charging / discharging / USB-only)
  // ======================================================================
  enum class Status : uint8_t {
    Unknown,
    Charging,
    Discharging,
    UsbOnly
  };

  void updateStatusFromSample(uint32_t nowMs) {
    using BCFG = PinsAndConfig::BatteryVD;

    const int sense = _reading.sense_mV;
    const int vfilt = (int)lroundf(_reading.vbat_filt_mV);

    // Heuristik: USB only (tanpa baterai) → sense sangat kecil
    if (sense < BCFG::SENSE_USB_ONLY_THRESH_mV) {
      _status            = Status::UsbOnly;
      _prevFiltMv        = vfilt;
      _lastStateUpdateMs = nowMs;
      return;
    }

    int dv = vfilt - _prevFiltMv;
    _prevFiltMv = vfilt;

    // Threshold untuk deteksi lonjakan cepat (spike charging / cabut charger)
    const int TH_FAST = 100;  // mV
    // const int TH_SLOW = 2; // bisa dipakai kalau mau deteksi drift pelan

    if (dv > TH_FAST) {
      _status            = Status::Charging;
      _lastStateUpdateMs = nowMs;
    } else if (dv < -TH_FAST) {
      _status            = Status::Discharging;
      _lastStateUpdateMs = nowMs;
    } else {
      // Tidak ada lonjakan signifikan: kalau masih Unknown, pakai level tegangan
      if (_status == Status::Unknown) {
        if (vfilt >= BCFG::VCHARGE_DETECT_mV) _status = Status::Charging;
        else                                  _status = Status::Discharging;
      }
    }
  }

  // ======================================================================
  // Members
  // ======================================================================
  RP_batteryVD   _batt;
  RPBVD_Reading  _reading{};
  uint32_t       _lastSampleMs      = 0;

  Status         _status            = Status::Unknown;
  int            _prevFiltMv        = 0;
  uint32_t       _lastStateUpdateMs = 0;
};
