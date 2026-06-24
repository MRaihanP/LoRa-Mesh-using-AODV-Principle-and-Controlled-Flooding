#pragma once
#include <Arduino.h>

// Forward declare tipe dari RP_MultiHop agar pins_and_config.h
// tidak perlu include RP_MultiHop.h langsung.
namespace NetRuntime {
  struct DeviceEntry;
  struct StaticRouteEntry;
  struct DataSlot;
  struct HelloSlot;
  struct FrameConfig;
  struct RoutingConfig;
  struct NetworkConfig;
}

namespace PinsAndConfig {

  // --- TFT / Touch / Timing ---
  struct TFT {
    // TftPins{ MOSI, MISO, SCK, CS, DC, RST, BL, TE }
    static constexpr int MOSI = 14;
    static constexpr int MISO = 13;
    static constexpr int SCK  = 9;
    static constexpr int CS   = 12;
    static constexpr int DC   = 11;
    static constexpr int RST  = 10;
    static constexpr int BL   = 21;
    static constexpr int TE   = 41;

    static constexpr uint32_t SPI_HZ  = 27000000;
    static constexpr bool     USE_TE  = true;
    static constexpr int      ROTATION= 0;
  };

  struct Touch {
    // TouchPins{ SDA, SCL, INT, RST }
    static constexpr int SDA = 39;
    static constexpr int SCL = 40;
    static constexpr int INT = -1;
    static constexpr int RST = -1;

    static constexpr bool MAP_ENABLED = true;
    static constexpr bool SWAP_XY     = false;
    static constexpr bool INVERT_X    = true;
    static constexpr bool INVERT_Y    = false;

    static constexpr int RAW_X_MIN = 5;
    static constexpr int RAW_X_MAX = 313;
    static constexpr int RAW_Y_MIN = 17;
    static constexpr int RAW_Y_MAX = 475;

    static constexpr int I2C_TIMEOUT_MS = 3;
  };

  // --- IMU / I2C bus for sensors (share with Touch pins) ---
  struct IMU {
    // Bus
    static constexpr int SDA = Touch::SDA;
    static constexpr int SCL = Touch::SCL;
    static constexpr uint32_t I2C_HZ = 400000; // 400 kHz

    // Addresses
    static constexpr uint8_t MPU_ADDR = 0x69;  // AD0=HIGH
    static constexpr uint8_t AK_ADDR  = 0x0C;  // fixed (bypass)

    // ===== RUNTIME SNAPSHOT UNTUK BACKEND (PDR, kompas, dsb.) =====
    // Status IMU
    static bool     imuOk;

    // Accel (g), Gyro (dps), Mag (uT) – sudah dikalibrasi & dimap ke frame device
    static float    ax_g, ay_g, az_g;
    static float    gx_dps, gy_dps, gz_dps;
    static float    mx_uT, my_uT, mz_uT;

    // Euler & heading
    static float    rollRad;        // rad
    static float    pitchRad;       // rad
    static float    yawRad;         // rad, heading view (0..2π)
    static float    headingRawRad;  // rad, sebelum offset
    static float    headingViewRad; // rad, sesudah offset (yang dipakai UI)
    static float    headingDeg;     // derajat 0..360

    // Magnitudo akselerasi total (g) – berguna untuk still/step detection
    static float    accelMag_g;
    static float    accForward_mps2;  // akselerasi LINIER sepanjang arah maju device (gravity removed & filtered)

    // Flag kalibrasi
    static bool     hasGyroCal;
    static bool     hasAccelCal;
    static bool     hasMagCal;

    // Timestamp terakhir update
    static uint32_t lastUpdateMs;
  };

  // --- BMP280 (pakai bus I2C yang sama dengan IMU) ---
  struct BMP280 {
    static constexpr uint8_t ADDR   = 0x76;
    static constexpr uint32_t I2C_HZ = 400000;     // 400 kHz
    // Nilai default (bisa kamu sesuaikan site kamu)
    static constexpr int SDA = Touch::SDA;
    static constexpr int SCL = Touch::SCL;
    static constexpr float SEA_LEVEL_PA_DEFAULT = 101325.0f; // Pa
    static constexpr float SITE_ALT_M_DEFAULT   = 400.0f;    // mdpl lokasi (untuk kalibrasi)
    // ===== RUNTIME SNAPSHOT UNTUK MAINPAGE =====
    // Altitude terhadap mean sea level (mdpl) -> ini yang nanti kita sebut "MSL"
    static float    altMSL_m;
    // Relative altitude terhadap baseline (Zero)
    static float    relAlt_m;
    // Valid flags
    static bool     altValid;
    static bool     relAltValid;
    // Timestamp terakhir update
    static uint32_t lastUpdateMs;
  };

  // pins_and_config.h (tambahkan di dalam namespace PinsAndConfig)
  struct SHT30 {
    // Konfigurasi hardware (sesuaikan dengan punyamu)
    static constexpr int      SDA     = IMU::SDA;
    static constexpr int      SCL     = IMU::SCL;
    static constexpr uint8_t  ADDR    = 0x45;
    static constexpr uint32_t I2C_HZ  = 100000;

    // Runtime snapshot – di-update oleh PageSensorSHT30, dibaca MainPage, dsb.
    static float     tempC;        // suhu terkoreksi (°C)
    static float     humPct;       // RH (%)
    static bool      tempValid;    // data suhu valid?
    static bool      humValid;     // data RH valid?
    static uint32_t  lastUpdateMs; // millis() terakhir update
  };

  struct GNSS {
    // Hardware pins
    static const int GPS_RX = 18; // ESP RX  <- GPS TX
    static const int GPS_TX = 8;  // ESP TX  -> GPS RX
    static constexpr uint32_t BAUD = 115200;

    // Runtime snapshot – di-update oleh PageGNSS_M10 backend
    static bool     connected;        // pernah lihat data dari modul?
    static bool     hasFix;           // lastPVT valid & gnssFixOK & fixType>=2
    static uint8_t  fixType;          // 0..5
    static bool     gnssFixOK;        // flags & 0x01
    static uint8_t  numSV;            // jumlah satelit dalam solusi

    static float    latDeg;           // derajat desimal
    static float    lonDeg;           // derajat desimal
    static float    altMSL_m;         // meter (hMSL)

    static float    hAcc_m;           // meter
    static float    vAcc_m;           // meter

    static float    speed2D_mps;      // m/s
    static float    speed3D_mps;      // m/s
    static float    groundTrackDeg;   // 0..360, NaN kalau speed kecil

    static uint32_t lastUpdateMs;     // millis() terakhir NAV-PVT diterima
    static uint32_t lastFixMs;        // millis() terakhir hasFix=true

    static float    velN_mps;      // kecepatan arah North (m/s)
    static float    velE_mps;      // kecepatan arah East (m/s)
    static float    velD_mps;      // kecepatan arah Down (m/s)

    // GNSS time (UTC epoch) dari RMC, untuk sinkron RTC
    static bool     timeValid;
    static uint32_t epochUtc;
    static uint32_t lastTimeMs;
  };

  // (Prepared for future RTC integration)
  struct RTC {
    static constexpr uint8_t ADDR = 0x68;
    static constexpr int SDA = Touch::SDA;
    static constexpr int SCL = Touch::SCL;
  };

  struct BatteryVD {
    // --- Hardware pins ---
    // Sesuai info kamu: enable divider = pin 11, ADC = pin 10.
    static constexpr int PIN_ADC         = 3;
    static constexpr int PIN_EN          = 47;
    static constexpr bool EN_ACTIVE_HIGH = true;  // HIGH = divider ON

    // --- Divider & kalibrasi (sesuaikan jika perlu) ---
    static constexpr float DIV_GAIN      = 2.0f;   // misal 2:1
    static constexpr float GAIN_CORR     = 1.0f;   // skala koreksi
    static constexpr int   OFFSET_mV     = 0;      // offset mV

    // --- Mapping SOC (display range) ---
    // Misal 0% di 3.2V dan 100% di 4.2V untuk 1S Li-ion
    static constexpr int VMIN_DISPLAY_mV = 3000;
    static constexpr int VMAX_DISPLAY_mV = 3900;

    // --- Kapasitas baterai (untuk estimasi mAh) ---
    static constexpr int FULL_mAh        = 3700;   // ubah sesuai pack kamu

    // --- Heuristik state detection (pakai di Page_Battery) ---
    // sense_mV sangat kecil -> divider tidak lihat tegangan -> kemungkinan USB only
    static constexpr int SENSE_USB_ONLY_THRESH_mV = 50;    // <50 mV dianggap NO BATTERY

    // vbat tinggi (mendekati max) -> diasumsikan CHARGING
    static constexpr int VCHARGE_DETECT_mV        = 4150;  // >4.15V dianggap CHARGING
    static int soc_display_pct;
  };

  struct NetLoRa {
    // Pin kontrol modul LoRa (E220) untuk jaringan Multi-Hop
    static constexpr int M0  = 4;
    static constexpr int M1  = 5;
    static constexpr int AUX = 15;

    // UART LoRa (pakai UART2 di firmware Navigator)
    static constexpr int RX  = 7;   // ESP RX  <- TX modul LoRa
    static constexpr int TX  = 6;   // ESP TX  -> RX modul LoRa
  };

  struct NetMultiHop {
    // Alias ke tipe dari NetRuntime (library RP_MultiHop)
    using DeviceEntry      = NetRuntime::DeviceEntry;
    using StaticRouteEntry = NetRuntime::StaticRouteEntry;
    using DataSlot         = NetRuntime::DataSlot;
    using HelloSlot        = NetRuntime::HelloSlot;
    using FrameConfig      = NetRuntime::FrameConfig;
    using RoutingConfig    = NetRuntime::RoutingConfig;
    using NetworkConfig    = NetRuntime::NetworkConfig;

    // Deklarasi array config (definisinya di .cpp)
    static const DeviceEntry      DEVICES[];
    static const StaticRouteEntry ROUTES[];
    static const DataSlot         DATA_SLOTS[];
    static const HelloSlot        HELLO_SLOTS[];

    static const FrameConfig      FRAME;
    static const RoutingConfig    ROUTING;
    static const NetworkConfig    NET;
    
    // ===== Runtime device ID (diisi oleh NetBackend) =====
    // idHash24  : 24-bit hash (0..0xFFFFFF)
    // idHex6    : string HEX 6 char + null, contoh "DE5C6F"
    static uint32_t idHash24;
    static char     idHex6[7];
    
    // Runtime snapshot (diisi oleh NetBackend)
    static uint8_t neighborCount;

  };

  struct MicroSD {
    // SPI pins (sesuai tabel kamu)
    static constexpr int PIN_MISO = 10;
    static constexpr int PIN_MOSI = 11;
    static constexpr int PIN_SCK  = 12;
    static constexpr int PIN_CS   = 48;

    // SPI freq (turunkan jika SD sering gagal)
    static constexpr uint32_t SPI_HZ = 20000000;

    // Folder path logs
    static constexpr const char* DIR_PATHS = "/paths";

    // Runtime flags
    static bool sdOk;
  };

  struct Power {
    // === Pin hardware ===
    // Sesuaikan dengan board kamu:
    // MY_POWER → pin ke basis NPN / gate PMOS yang menahan power
    static constexpr int PIN_LATCH  = 16;   // dulu MY_POWER
    static constexpr int PIN_SWITCH = 17;   // tombol power
    static constexpr int PIN_LED    = 1;    // LED indikator (bisa pakai LED lain kalau mau)

    // Level logika
    static constexpr uint8_t LATCH_ON_LEVEL  = HIGH;   // HIGH = tahan power
    static constexpr uint8_t LATCH_OFF_LEVEL = LOW;    // LOW  = lepas power
    static constexpr uint8_t SW_ACTIVE_LEVEL = LOW;    // tombol ke GND + INPUT_PULLUP

    // Timing (ms) – ambil dari sketch contoh
    static constexpr uint32_t TOGGLE_BLINK_MS    = 300;  // kedip pelan saat ditekan
    static constexpr uint32_t HOLD_OFF_MS        = 3000; // tahan 3s utk shutdown
    static constexpr uint32_t SHUTDOWN_BLINK_MS  = 100;  // kedip cepat sebelum mati
    
    static constexpr int PIN_BUZZER = 2;
    static bool sosActive;    
  };

  struct Buzzer{
    static constexpr int PIN = 2;
  };

  // --- Theme / Geometry (same as your sketch) ---
  struct Theme {
    static constexpr uint8_t UI_R = 35, UI_G = 38, UI_B = 48;  // panel/box
    static constexpr uint8_t SEP_R= 20, SEP_G= 20, SEP_B= 26;  // separator

    static constexpr int BAR_H = 30;   // top bar
    static constexpr int BOT_H = 60;   // bottom bar
    static constexpr int BOT_R = 16;   // bottom bar top radius
    static constexpr int EDGE_BUF = 10;

    static constexpr int LBOX_W=140,  LBOX_H=76;   // left box
    static constexpr int RBOX_W=120,  RBOX_H=52;   // right box
    static constexpr int BOX_RADIUS = 12;

    static constexpr int STRIP_H = 110; // SpriteBlitterEx tile height
  };
}