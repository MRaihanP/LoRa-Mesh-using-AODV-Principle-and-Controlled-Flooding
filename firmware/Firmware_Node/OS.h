#pragma once
#include <RP_TFTDisplay.h>
#include <math.h>
#include "pins_and_config.h"

// ===== Small helpers =====
static inline float rad2deg(float r){ return r * 57.2957795f; }
static inline float norm360f(float a){ a=fmodf(a,360.0f); if (a<0) a+=360.0f; return a; }

// Short aliases
using Theme = PinsAndConfig::Theme;

class AppOS {
public:
  UiApp            ui;
  BackgroundRGB888 bg;
  SpriteBlitterEx  blit;
  FramePacer       pacer;

  void begin(){
    UiHwConfig cfg;
    cfg.tft      = TftPins{
      PinsAndConfig::TFT::MOSI, PinsAndConfig::TFT::MISO, PinsAndConfig::TFT::SCK,
      PinsAndConfig::TFT::CS,   PinsAndConfig::TFT::DC,   PinsAndConfig::TFT::RST,
      PinsAndConfig::TFT::BL,   PinsAndConfig::TFT::TE
    };
    cfg.touch    = TouchPins{
      PinsAndConfig::Touch::SDA, PinsAndConfig::Touch::SCL,
      PinsAndConfig::Touch::INT, PinsAndConfig::Touch::RST
    };
    cfg.rotation = PinsAndConfig::TFT::ROTATION;
    cfg.spiHz    = PinsAndConfig::TFT::SPI_HZ;
    cfg.useTE    = PinsAndConfig::TFT::USE_TE;
    cfg.framebufferFmt = PixelFormat::RGB888;
    cfg.doubleBuffer   = true;

    ui.begin(cfg);
    ui.tft().setBacklightPercent(100);
    ui.tft().setAutoVSync(false);
    if (PinsAndConfig::TFT::USE_TE){ ui.tft().setTEGuardMicros(260); }
    pacer.begin(PinsAndConfig::TFT::USE_TE, 60.0f);

    // Background (black)
    bg.createBlackPSRAM();
    ui.tft().writeRectRGB888(0,0, TFT_WIDTH, TFT_HEIGHT, bg.ptr(), TFT_WIDTH);

    // Blitter: full-width strips
    blit.begin(TFT_WIDTH, Theme::STRIP_H);

    // Touch mapping (unchanged)
    ui.touch().setMappingEnabled(PinsAndConfig::Touch::MAP_ENABLED);
    ui.touch().setSwapXY(PinsAndConfig::Touch::SWAP_XY);
    ui.touch().setInvertX(PinsAndConfig::Touch::INVERT_X);
    ui.touch().setInvertY(PinsAndConfig::Touch::INVERT_Y);
    ui.touch().setRawBounds(
      PinsAndConfig::Touch::RAW_X_MIN, PinsAndConfig::Touch::RAW_X_MAX,
      PinsAndConfig::Touch::RAW_Y_MIN, PinsAndConfig::Touch::RAW_Y_MAX
    );
    ui.touch().setScreenSize(TFT_WIDTH, TFT_HEIGHT);
    ui.touch().setI2CTimeoutMs(PinsAndConfig::Touch::I2C_TIMEOUT_MS);
  }

  void tickStart(){ pacer.tickStart(); }
  void tickEnd(){ pacer.tickEndAndWait(ui.tft()); }

  // Read first valid touch point
  bool readTouch1(int &tx, int &ty){
    TouchFT5x16::TouchPoint tp[2];
    if (!ui.touch().read(tp,2)) return false;
    for (int i=0;i<2;i++){ if (tp[i].valid){ tx=tp[i].x; ty=tp[i].y; return true; } }
    return false;
  }
};

// Forward declarations
class TopBar;
class QuickSettingsPanel;
class MainPage;

// Modules
#include "TopBar.h"
#include "QuickSettings.h"
#include "MainPage.h"