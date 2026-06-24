#pragma once
#include <string.h>
#include <WiFi.h>
#include <time.h>
#include <RP_TFTDisplay.h>
#include "OS.h"
#include "pins_and_config.h"
#include "RP_batteryVD.h"

int PinsAndConfig::BatteryVD::soc_display_pct = -1;

enum class TopBarBatteryStatus : uint8_t {
  Unknown = 0,
  Discharging,
  Charging
};

class TopBar {
public:
  void begin(AppOS* os){
    _os = os;
    drawTopBarOnce();

    // ===== Clock (left, UTC) =====
    _clkScale = 2;
    const int th   = 7 * _clkScale;
    const int tw   = SimpleFont::textWidth("00:00", _clkScale, 1);
    const int padL = 8;
    const int yNudge = -1;

    _clkX  = padL;
    _clkY  = (Theme::BAR_H - th) / 2 + yNudge;
    _clkRx = _clkX - 2;
    _clkRy = _clkY - 1;
    _clkRw = tw + 4;
    _clkRh = th + 2;

    // Init clock string dari waktu sistem (UTC) kalau sudah valid
    makeClockString(_clkStr, _clkStr);
    drawClockROI(_clkStr);

    // ===== Battery (right) – geometry =====
    _batW  = 32;
    _batH  = 16;
    _batGap= 3;
    _batCx = TFT_WIDTH - 27;
    _batCy = Theme::BAR_H/2;
    computeBatteryROI(_batCx, _batCy, _batW, _batH, _batGap,
                      _batRx,_batRy,_batRw,_batRh);

    // ===== Init real battery backend & first icon =====
    initBatteryBackend();
    {
      int pct = _batReading.soc_disp_int;
      if (pct < 0)   pct = 0;
      if (pct > 100) pct = 100;
      bool charging = (_batStatus == TopBarBatteryStatus::Charging);
      drawBatteryIconROI(pct, charging);
    }

    // ===== Wi-Fi icon (topbar, kiri baterai, tanpa background circle) =====
    _wifiCx = 0; _wifiCy = _batCy;
    {
      const int outerR   = WIFI_TOPBAR_OUTER_R;
      const int rBg      = outerR + 10;  // kira-kira lebar glyph
      const int wifiW    = rBg * 2;

      const int batLeft  = _batCx - _batW/2;
      const int wifiRight= batLeft - WIFI_TOPBAR_GAP;
      _wifiCx = wifiRight - wifiW/2;

      _wifiRx = _wifiCx - rBg;
      _wifiRw = rBg * 2;
      _wifiRy = 0;
      _wifiRh = Theme::BAR_H;
    }
    _wifiConnected = (WiFi.status() == WL_CONNECTED);
    drawWifiIconROI(_wifiConnected);

    // ===== ID capsule (center) =====
    setDeviceId("000000");
  }

  // Dipanggil dari loop saat UI sleep:
  // Update SoC global (PinsAndConfig::BatteryVD::soc_display_pct) TANPA render.
  void backendTick(uint32_t nowMs){
    if (_batInited){
      sampleBattery(nowMs, false);
    }
  }

  void tick(uint32_t nowMs, float dt){
    (void)dt; // saat ini tidak dipakai di TopBar

    // ===== Battery sampling + charging detection =====
    if (_batInited){
      sampleBattery(nowMs, /*drawIcon=*/true);
    }

    // ===== Clock (UTC, anti-flicker: hanya redraw jika menit berubah) =====
    char buf[6];
    if (makeClockString(buf, _clkStr)) {
      memcpy(_clkStr, buf, sizeof(_clkStr));
      drawClockROI(_clkStr);
    }

    // ===== Wi-Fi status → icon topbar =====
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (nowConnected != _wifiConnected){
      _wifiConnected = nowConnected;
      drawWifiIconROI(_wifiConnected);
    }
  }

  // Dipanggil setelah full flush (renderer swap) untuk memunculkan TopBar lagi
  void redrawAll(){
    drawTopBarOnce();
    drawClockROI(_clkStr);

    int pct = _batReading.soc_disp_int;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    bool charging = (_batStatus == TopBarBatteryStatus::Charging);
    drawBatteryIconROI(pct, charging);

    drawWifiIconROI(_wifiConnected);
    if (_idRw>0 && _idRh>0) drawIdBadgeROI();
  }

  void repaintSlice(int y0, int y1){
    y0 = max(y0, 0); y1 = min(y1, Theme::BAR_H);
    if (y1<=y0) return;
    int ox=0; int oy=y0; int ow=TFT_WIDTH; int oh=(y1-y0);
    _os->blit.blit(_os->ui.tft(), _os->bg, ox,oy,ow,oh, [&](GfxRGB888& g,int,int,int Rw,int Rh){
      uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
      for (size_t i=0;i<N;++i){ *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B; }
      if (Rh>0){
        uint8_t* row = g.pix + ((size_t)(Rh-1)*Rw)*3;
        for (int i=0;i<Rw;++i){
          row[3*i+0]=Theme::SEP_R;
          row[3*i+1]=Theme::SEP_G;
          row[3*i+2]=Theme::SEP_B;
        }
      }
    });
  }

  void drawBatteryIconROI(int pct, bool charging){
    int rw=_batRw, rh=_batRh;
    if (rw<=0 || rh<=0) return;

    _os->blit.blit(_os->ui.tft(), _os->bg, _batRx,_batRy,rw,rh,
      [&](GfxRGB888& gg,int Rrx,int Rry,int Rw,int Rh){
        // repaint bar bg
        uint8_t* p = gg.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }

        // separator if ROI crosses bottom
        const int sepYGlobal = Theme::BAR_H - 1;
        for (int y=0; y<Rh; ++y){
          const int gy = _batRy + y;
          if (gy == sepYGlobal){
            for (int x=0; x<Rw; ++x){
              uint8_t* q = gg.pix + ((size_t)y*Rw + x)*3;
              q[0]=Theme::SEP_R; q[1]=Theme::SEP_G; q[2]=Theme::SEP_B;
            }
          }
        }

        batteryIcon(gg, Rrx, Rry,
                    _batCx,_batCy,
                    _batW,_batH,_batGap,
                    pct,
                    90,90,95,   // body
                    255,255,255,// stroke
                    true,       // showCap
                    charging);  // charging flag
      }
    );
  }

  void setDeviceId(const char* hex){
    if (!hex) return;
    char newText[ID_MAX_LEN+1];
    size_t n = strlen(hex);
    if (n > ID_MAX_LEN) n = ID_MAX_LEN;
    memcpy(newText, hex, n); newText[n] = '\0';
    if (strcmp(newText, _idStr) == 0) return;

    if (_idRw > 0 && _idRh > 0){
      repaintROI(_idRx, _idRy, _idRw, _idRh);
    }

    computeIdLayout(newText);
    memcpy(_idStr, newText, n+1);
    drawIdBadgeROI();
  }

  // Getter opsional kalau nanti mau dipakai Page_Battery atau page lain
  const RPBVD_Reading& batteryReading() const { return _batReading; }
  TopBarBatteryStatus  batteryStatus()  const { return _batStatus; }

private:
  AppOS* _os = nullptr;

  using Theme = PinsAndConfig::Theme;

  static constexpr long CLOCK_TZ_OFFSET_SEC = 7L * 3600L;

  // Battery icon layout
  int _batCx=0,_batCy=0,_batW=32,_batH=16,_batGap=3;
  int _batRx=0,_batRy=0,_batRw=0,_batRh=0;

  // Real battery backend
  RP_batteryVD        _bat;
  RPBVD_Reading       _batReading{};
  bool                _batInited        = false;
  uint32_t            _batLastSampleMs  = 0;
  float               _batLastVFilt     = 0.0f;
  bool                _batHasPrevVFilt  = false;
  TopBarBatteryStatus _batStatus        = TopBarBatteryStatus::Unknown;

  // Clock (UTC)
  int _clkX=0, _clkY=0;
  int _clkRx=0,_clkRy=0,_clkRw=0,_clkRh=0;
  int _clkScale=2;
  char _clkStr[6] = {'0','0',':','0','0','\0'};

  // ID badge
  static constexpr uint8_t ID_R = 0, ID_G = 80, ID_B = 255;
  int _idScale = 2;
  int _idPadX  = 10;
  int _idPadY  = 3;
  int _idX=0, _idY=0;
  int _idRx=0,_idRy=0,_idRw=0,_idRh=0;
  static constexpr int ID_MAX_LEN = 32;
  char _idStr[ID_MAX_LEN+1] = {0};

  // Wi-Fi icon (TopBar, kiri baterai, tanpa background circle)
  bool _wifiConnected = false;
  static constexpr int WIFI_TOPBAR_GAP      = 6;  // jarak Wi-Fi ↔ baterai (px)
  static constexpr int WIFI_TOPBAR_OUTER_R  = 8;  // scale glyph Wi-Fi
  int _wifiCx=0,_wifiCy=0;
  int _wifiRx=0,_wifiRy=0,_wifiRw=0,_wifiRh=0;

  // ===== Battery backend helpers =====
  void initBatteryBackend(){
    using BCFG = PinsAndConfig::BatteryVD;
    RPBVD_Config cfg;

    cfg.pin_adc             = BCFG::PIN_ADC;
    cfg.pin_enable          = BCFG::PIN_EN;
    cfg.enable_active_high  = BCFG::EN_ACTIVE_HIGH;

    cfg.adc_resolution_bits = 12;
    cfg.adc_ref_mV          = 3300;

    cfg.divider_gain        = BCFG::DIV_GAIN;
    cfg.gain_corr           = BCFG::GAIN_CORR;
    cfg.offset_corr_mV      = BCFG::OFFSET_mV;

    // Mapping SOC 0% di 3.0V dan 100% di 3.9V (diatur di PinsAndConfig)
    cfg.vmin_display_mV     = BCFG::VMIN_DISPLAY_mV;  // misal 3000
    cfg.vmax_display_mV     = BCFG::VMAX_DISPLAY_mV;  // misal 3900

    cfg.vclip_min_mV        = 2500;
    cfg.vclip_max_mV        = 4500;

    cfg.mode                = RPBVD_SocMode::LINEAR;
    cfg.lut                 = nullptr;
    cfg.lut_n               = 0;

    cfg.ema_alpha_v         = 0.25f;
    cfg.ema_alpha_soc       = 0.20f;
    cfg.display_gamma       = 0.78f;
    cfg.hysteresis_pct      = 0.4f;
    cfg.min_step_ms         = 4000;

    cfg.full_capacity_mAh   = BCFG::FULL_mAh;

    _bat.begin(cfg);

    // Baca awal supaya filter punya state
    _bat.firstMeasure(_batReading);
    _batLastSampleMs  = millis();
    _batLastVFilt     = _batReading.vbat_filt_mV;
    _batHasPrevVFilt  = true;
    _batStatus        = TopBarBatteryStatus::Unknown;
    _batInited        = true;

    // Seed runtime SoC global (0..100)
    int pct0 = _batReading.soc_disp_int;
    if (pct0 < 0)   pct0 = 0;
    if (pct0 > 100) pct0 = 100;
    PinsAndConfig::BatteryVD::soc_display_pct = pct0;
  }

  void sampleBattery(uint32_t nowMs, bool drawIcon){
    const uint32_t SAMPLE_MS = 1000u;
    if (nowMs - _batLastSampleMs < SAMPLE_MS) return;
    _batLastSampleMs = nowMs;

    RPBVD_Reading r;
    _bat.sample(r);
    _batReading = r;

    float v = r.vbat_filt_mV;
    if (_batHasPrevVFilt){
      float dv = v - _batLastVFilt;

      const float CHG_THRESH  = 20.0f;   // mV
      const float DISC_THRESH = -20.0f;  // mV

      if (dv > CHG_THRESH){
        _batStatus = TopBarBatteryStatus::Charging;
      } else if (dv < DISC_THRESH){
        _batStatus = TopBarBatteryStatus::Discharging;
      }
    } else {
      _batStatus       = TopBarBatteryStatus::Unknown;
      _batHasPrevVFilt = true;
    }

    _batLastVFilt = v;

    int pct = _batReading.soc_disp_int;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    // Update runtime SoC global yang bisa diakses NetBackend
    PinsAndConfig::BatteryVD::soc_display_pct = pct;

    if (drawIcon){
      bool charging = (_batStatus == TopBarBatteryStatus::Charging);
      drawBatteryIconROI(pct, charging);
    }
  }

  // ===== Helpers (top bar bg, ROI, ID, clock, Wi-Fi) =====
  void drawTopBarOnce(){
    int bx=0, by=0; int bw=TFT_WIDTH, bh=Theme::BAR_H;
    _os->blit.blit(_os->ui.tft(), _os->bg, bx,by,bw,bh,
      [&](GfxRGB888& g,int,int,int Rw,int Rh){
        uint8_t* p = g.pix; size_t N = (size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }
        if (Rh>0){
          uint8_t* row = g.pix + ((size_t)(Rh-1)*Rw)*3;
          for (int i=0;i<Rw;++i){
            row[3*i+0]=Theme::SEP_R;
            row[3*i+1]=Theme::SEP_G;
            row[3*i+2]=Theme::SEP_B;
          }
        }
      }
    );
  }

  void repaintROI(int rx,int ry,int rw,int rh){
    if (rw<=0 || rh<=0) return;
    _os->blit.blit(_os->ui.tft(), _os->bg, rx,ry,rw,rh,
      [&](GfxRGB888& g,int /*Rrx*/,int /*Rry*/,int Rw,int Rh){
        uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }
        const int sepY = Theme::BAR_H - 1;
        for (int y=0; y<Rh; ++y){
          if (ry + y == sepY){
            uint8_t* row = g.pix + ((size_t)y*Rw)*3;
            for (int x=0; x<Rw; ++x){
              uint8_t* q=&row[x*3];
              q[0]=Theme::SEP_R; q[1]=Theme::SEP_G; q[2]=Theme::SEP_B;
            }
          }
        }
      }
    );
  }

  void drawClockROI(const char* text){
    int rw=_clkRw, rh=_clkRh;
    if (rw<=0 || rh<=0) return;

    _os->blit.blit(_os->ui.tft(), _os->bg, _clkRx,_clkRy,rw,rh,
      [&](GfxRGB888& g,int Rrx,int Rry,int Rw,int Rh){
        uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }

        const int sepYGlobal = Theme::BAR_H - 1;
        for (int y=0; y<Rh; ++y){
          const int gy = _clkRy + y;
          if (gy == sepYGlobal){
            for (int x=0; x<Rw; ++x){
              uint8_t* q = g.pix + ((size_t)y*Rw + x)*3;
              q[0]=Theme::SEP_R; q[1]=Theme::SEP_G; q[2]=Theme::SEP_B;
            }
          }
        }

        const int glyphH     = 7 * _clkScale;
        const int yBaseLocal = (_clkY - Rry) + glyphH;
        const int xLocal     = (_clkX - Rrx);

        SimpleFont::drawTextStyled(g, xLocal, yBaseLocal, text,
                                   255,255,255, _clkScale, 1,
                                   SimpleFont::AlignLeft, -1, SimpleFont::Normal);
      }
    );
  }

  void computeIdLayout(const char* text){
    const int glyphH = 7 * _idScale;
    const int textW  = SimpleFont::textWidth(text, _idScale, 1);
    const int capH   = glyphH + 2*_idPadY;
    const int capW   = textW  + 2*_idPadX;

    _idRw = capW; _idRh = capH;
    _idRx = (TFT_WIDTH  - _idRw) / 2;
    _idRy = (Theme::BAR_H - _idRh) / 2;

    _idX  = _idRx + (_idRw - textW) / 2;
    _idY  = _idRy + (_idRh - glyphH) / 2;
  }

  void drawIdBadgeROI(){
    if (_idRw<=0 || _idRh<=0) return;
    int rw=_idRw, rh=_idRh;

    _os->blit.blit(_os->ui.tft(), _os->bg, _idRx,_idRy,rw,rh,
      [&](GfxRGB888& g,int /*Rrx*/,int /*Rry*/,int Rw,int Rh){
        uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }

        const int sepY = Theme::BAR_H - 1;
        for (int y=0; y<Rh; ++y){
          if (_idRy + y == sepY){
            uint8_t* row = g.pix + ((size_t)y*Rw)*3;
            for (int x=0; x<Rw; ++x){
              uint8_t* q=&row[x*3];
              q[0]=Theme::SEP_R; q[1]=Theme::SEP_G; q[2]=Theme::SEP_B;
            }
          }
        }

        const int r = Rh/2;
        for (int y=0; y<Rh; ++y){
          int inset=0;
          if (r>0){
            if (y < r){
              float dy = (float)(r - 1 - y);
              float dx = sqrtf((float)r*r - dy*dy);
              inset = (int)(r - floorf(dx));
            }
            int y2 = (Rh - 1 - y);
            if (y2 < r){
              float dy = (float)(r - 1 - y2);
              float dx = sqrtf((float)r*r - dy*dy);
              int inset2 = (int)(r - floorf(dx));
              if (inset2 > inset) inset = inset2;
            }
          }
          uint8_t* row = g.pix + ((size_t)y*Rw)*3;
          for (int x=0; x<Rw; ++x){
            const bool inside = (x>=inset && x<(Rw-inset));
            if (inside){
              row[0]=ID_R; row[1]=ID_G; row[2]=ID_B;
            }
            row += 3;
          }
        }

        const int glyphH     = 7 * _idScale;
        const int yBaseLocal = (_idY - _idRy) + glyphH;
        const int xLocal     = (_idX - _idRx);

        SimpleFont::drawTextStyled(g, xLocal, yBaseLocal, _idStr,
                                   255,255,255, _idScale, 1,
                                   SimpleFont::AlignLeft, -1, SimpleFont::Normal);
      }
    );
  }

  // Pakai waktu sistem (UTC) → ditampilkan sebagai UTC+7 → "HH:MM"
  bool makeClockString(char out[6], const char prev[6]){
    time_t now = 0;
    time(&now);

    // Kalau waktu belum pernah di-set (masih 0), jangan ubah tampilan
    if (now <= 0){
      memcpy(out, prev, 6);
      return false;
    }

    // Hanya untuk tampilan: geser ke UTC+7
    time_t local = now + CLOCK_TZ_OFFSET_SEC;

    struct tm t{};
    gmtime_r(&local, &t); // di-backend tetap UTC, di sini cuma offset view

    out[0] = (char)('0' + (t.tm_hour / 10));
    out[1] = (char)('0' + (t.tm_hour % 10));
    out[2] = ':';
    out[3] = (char)('0' + (t.tm_min / 10));
    out[4] = (char)('0' + (t.tm_min % 10));
    out[5] = '\0';

    // Hanya trigger redraw kalau menit/hours berubah
    return (out[0]!=prev[0] || out[1]!=prev[1] ||
            out[3]!=prev[3] || out[4]!=prev[4]);
  }

  // ===== Wi-Fi glyph helper (tanpa background circle) =====
  static void drawWifiTopbarIcon(GfxRGB888& g, int Rrx, int Rry,
                                 int cx, int cy, int outerR){
    if (outerR <= 6) return;

    // Geometrinya meniru wifiIcon() di Icon.h tapi tanpa circle bg.
    const float fDot     = 0.14f;
    const float fGap     = 0.10f;
    const float fThick   = 0.12f;
    const float thetaDeg = 45.0f;

    float rDot0  = fDot   * outerR;
    float gap0   = fGap   * outerR;
    float th0    = fThick * outerR;
    float total0 = rDot0 + (gap0 + th0) * 3.0f;
    if (total0 < 1.0f) total0 = 1.0f;
    float S = (float)outerR / total0;

    float rDot = max(2.0f, rDot0 * S);
    float gap  = max(1.0f, gap0  * S);
    float th   = max(2.0f, th0   * S);

    float ri1 = rDot + gap;
    float ro1 = ri1  + th;
    float ri2 = ro1  + gap;
    float ro2 = ri2  + th;
    float ri3 = ro2  + gap;
    float ro3 = ri3  + th;

    int ax = cx;
    int ay = cy + (int)roundf((outerR - rDot) * 0.5f);

    icFillWedgeRingTop(g, Rrx, Rry, ax, ay, ri3, ro3, thetaDeg, 255, 255, 255);
    icFillWedgeRingTop(g, Rrx, Rry, ax, ay, ri2, ro2, thetaDeg, 255, 255, 255);
    icFillWedgeRingTop(g, Rrx, Rry, ax, ay, ri1, ro1, thetaDeg, 255, 255, 255);
    icFillCircle      (g, Rrx, Rry, ax, ay, (int)roundf(rDot), 255, 255, 255);
  }

  void drawWifiIconROI(bool connected){
    if (_wifiRw<=0 || _wifiRh<=0) return;

    _os->blit.blit(_os->ui.tft(), _os->bg, _wifiRx,_wifiRy,_wifiRw,_wifiRh,
      [&](GfxRGB888& g,int Rrx,int Rry,int Rw,int Rh){
        // repaint bar bg
        uint8_t* p = g.pix; size_t N=(size_t)Rw*Rh;
        for (size_t i=0;i<N;++i){
          *p++=Theme::UI_R; *p++=Theme::UI_G; *p++=Theme::UI_B;
        }

        // separator di bawah bar
        const int sepYGlobal = Theme::BAR_H - 1;
        for (int y=0; y<Rh; ++y){
          const int gy = _wifiRy + y;
          if (gy == sepYGlobal){
            for (int x=0; x<Rw; ++x){
              uint8_t* q = g.pix + ((size_t)y*Rw + x)*3;
              q[0]=Theme::SEP_R; q[1]=Theme::SEP_G; q[2]=Theme::SEP_B;
            }
          }
        }

        if (connected){
          drawWifiTopbarIcon(g, Rrx, Rry, _wifiCx, _wifiCy, WIFI_TOPBAR_OUTER_R);
        }
      }
    );
  }
};
