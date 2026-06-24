#include "OS.h"               // umbrella

#include "MenuPage.h"
#include "IRouter.h"
#include "PageRouter.h"

#include "Page_SimpleChat.h"
#include "Page_Network.h"
#include "Page_LogPath.h"
#include "Page_LogData.h"
#include "Page_SensorMPU9250.h"
#include "Page_SensorBMP280.h"
#include "Page_SensorSHT30.h"
#include "Page_GNSS_M10.h"
#include "Page_ClockDS3231.h"
#include "Page_MicroSD.h"
#include "Page_Battery.h"
#include "Page_Wifi.h"
#include "PowerControl.h"
#include "pins_and_config.h"
#include "PowerPopupOverlay.h"
#include "TresnoLogo.h"
#include "BuzzerTone.h"
#include "NetBackend.h"
#include "TimeSync.h"

#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <Preferences.h>
extern "C" {
  #include "esp_bt.h"
}

// ==== App building blocks ====
static AppOS              os;
static TopBar             topBar;
static QuickSettingsPanel qs;
static MainPage           page;

// Pages & Router
static MenuPage           menu;
static PageSimpleChat     simpleChat;
static PageNetwork        pageNet;
static PageLogPath        pageLogPath;
static PageLogData        pageLogData;

static PageSensorMPU9250  pageMPU;
static PageSensorBMP280   pageBMP;
static PageSensorSHT30    pageSHT;
static PageGNSS_M10       pageM10;
static PageClockDS3231    pageCLK;
static PageMicroSD        pageMSD;
static PageBattery        pageBAT;
static PageWifi           pageWifi;

static PageRouter         router;
static PowerControl       powerCtl;
static PowerPopupOverlay  powerPopup;

static void handlePowerLongHold();
static void handlePowerButtonPressedInPopup();

// ===== Definisi runtime SHT30 (wajib, padanan dari deklarasi di pins_and_config.h) =====
float    PinsAndConfig::SHT30::tempC       = NAN;
float    PinsAndConfig::SHT30::humPct      = NAN;
bool     PinsAndConfig::SHT30::tempValid   = false;
bool     PinsAndConfig::SHT30::humValid    = false;
uint32_t PinsAndConfig::SHT30::lastUpdateMs= 0;

// ===== Runtime snapshot BMP280 (untuk MainPage) =====
float    PinsAndConfig::BMP280::altMSL_m     = 0.0f;
float    PinsAndConfig::BMP280::relAlt_m     = 0.0f;
bool     PinsAndConfig::BMP280::altValid     = false;
bool     PinsAndConfig::BMP280::relAltValid  = false;
uint32_t PinsAndConfig::BMP280::lastUpdateMs = 0;

// ===== Runtime snapshot GNSS M10 =====
bool     PinsAndConfig::GNSS::connected       = false;
bool     PinsAndConfig::GNSS::hasFix          = false;
uint8_t  PinsAndConfig::GNSS::fixType         = 0;
bool     PinsAndConfig::GNSS::gnssFixOK       = false;
uint8_t  PinsAndConfig::GNSS::numSV           = 0;

float    PinsAndConfig::GNSS::latDeg          = NAN;
float    PinsAndConfig::GNSS::lonDeg          = NAN;
float    PinsAndConfig::GNSS::altMSL_m        = NAN;

float    PinsAndConfig::GNSS::hAcc_m          = NAN;
float    PinsAndConfig::GNSS::vAcc_m          = NAN;

float    PinsAndConfig::GNSS::speed2D_mps     = 0.0f;
float    PinsAndConfig::GNSS::speed3D_mps     = 0.0f;
float    PinsAndConfig::GNSS::groundTrackDeg  = NAN;

float    PinsAndConfig::GNSS::velN_mps       = 0.0f;
float    PinsAndConfig::GNSS::velE_mps       = 0.0f;
float    PinsAndConfig::GNSS::velD_mps       = 0.0f;

uint32_t PinsAndConfig::GNSS::lastUpdateMs    = 0;
uint32_t PinsAndConfig::GNSS::lastFixMs       = 0;

bool     PinsAndConfig::GNSS::timeValid  = false;
uint32_t PinsAndConfig::GNSS::epochUtc   = 0;
uint32_t PinsAndConfig::GNSS::lastTimeMs = 0;

bool     PinsAndConfig::IMU::imuOk          = false;

float    PinsAndConfig::IMU::ax_g           = 0.0f;
float    PinsAndConfig::IMU::ay_g           = 0.0f;
float    PinsAndConfig::IMU::az_g           = 0.0f;

float    PinsAndConfig::IMU::gx_dps         = 0.0f;
float    PinsAndConfig::IMU::gy_dps         = 0.0f;
float    PinsAndConfig::IMU::gz_dps         = 0.0f;

float    PinsAndConfig::IMU::mx_uT          = 0.0f;
float    PinsAndConfig::IMU::my_uT          = 0.0f;
float    PinsAndConfig::IMU::mz_uT          = 0.0f;

float    PinsAndConfig::IMU::rollRad        = 0.0f;
float    PinsAndConfig::IMU::pitchRad       = 0.0f;
float    PinsAndConfig::IMU::yawRad         = 0.0f;
float    PinsAndConfig::IMU::headingRawRad  = 0.0f;
float    PinsAndConfig::IMU::headingViewRad = 0.0f;
float    PinsAndConfig::IMU::headingDeg     = 0.0f;

float    PinsAndConfig::IMU::accelMag_g     = 0.0f;
float    PinsAndConfig::IMU::accForward_mps2  = 0.0f;

bool     PinsAndConfig::IMU::hasGyroCal     = false;
bool     PinsAndConfig::IMU::hasAccelCal    = false;
bool     PinsAndConfig::IMU::hasMagCal      = false;

uint32_t PinsAndConfig::IMU::lastUpdateMs   = 0;

// ===== Global BLE power state (untuk QS) =====
static bool g_bleOn = false;

// Dipanggil QS ketika ikon BLE di-tap
static void qsBleSet(bool on, void* ctx){
  (void)ctx;
  if (on == g_bleOn) return;

  if (on){
    // Nyalakan BT controller (BLE). btStart() return bool.
    if (btStart()){
      g_bleOn = true;
    } else {
      // Jika gagal start, paksa flag tetap false
      g_bleOn = false;
    }
  } else {
    // Matikan BT controller → radio BLE off, hemat daya
    btStop();
    g_bleOn = false;
  }
}

// Dipakai QS untuk membaca status aktual
static bool qsBleGet(void* ctx){
  (void)ctx;
  return g_bleOn;
}

// =====================================================
//  Helper: Sync system clock (time()) from DS3231 (UTC)
//  - Dipakai saat boot, sebelum TopBar mulai dipakai
// =====================================================
static bool rtcReadUTC_DS3231(struct tm &out) {
  using RtcCfg = PinsAndConfig::RTC;

  // Asumsi: Wire.begin(SDA,SCL) sudah dipanggil di AppOS::begin()
  Wire.beginTransmission(RtcCfg::ADDR);
  Wire.write(0x00); // mulai dari register detik
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(RtcCfg::ADDR, (uint8_t)7) != 7) {
    return false;
  }

  auto fromBCD = [](uint8_t b) -> uint8_t {
    return (uint8_t)(10 * ((b >> 4) & 0x0F) + (b & 0x0F));
  };

  uint8_t secB  = Wire.read();
  uint8_t minB  = Wire.read();
  uint8_t hrB   = Wire.read();
  uint8_t dowB  = Wire.read(); (void)dowB;
  uint8_t dateB = Wire.read();
  uint8_t monB  = Wire.read();
  uint8_t yrB   = Wire.read();

  struct tm t{};
  t.tm_sec  = fromBCD(secB & 0x7F);
  t.tm_min  = fromBCD(minB & 0x7F);
  t.tm_hour = fromBCD(hrB  & 0x3F);
  t.tm_mday = fromBCD(dateB & 0x3F);
  t.tm_mon  = fromBCD(monB & 0x1F) - 1;
  t.tm_year = fromBCD(yrB) + 100;  // tahun = 2000 + yrB
  t.tm_isdst = 0;

  out = t;
  return true;
}

static void handleSOSBuzzer() {
  static uint32_t lastMs = 0;
  static bool toneOn = false;

  if (!PinsAndConfig::Power::sosActive) {
    if (toneOn) {
      BuzzerTone::stop();
      toneOn = false;
    }
    return;
  }

  if (millis() - lastMs >= 500) {
    lastMs = millis();
    toneOn = !toneOn;
    if (toneOn) BuzzerTone::start(2000);  // frekuensi SOS
    else        BuzzerTone::stop();
  }
}

static void syncSystemClockFromRTC() {
  struct tm t{};
  if (!rtcReadUTC_DS3231(t)) {
    // Gagal baca RTC → biarkan system clock apa adanya
    return;
  }

  // Di project ini, system time diperlakukan sebagai UTC (configTime(0,0,...))
  // Jadi kita bisa pakai mktime() langsung (default TZ = UTC di ESP32 jika belum di-set)
  time_t epoch = mktime(&t);
  if (epoch <= 0) return;

  struct timeval tv;
  tv.tv_sec  = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// =====================================================
//   SPLASH SCREEN: FULL BLACK + LOGO "TRESNO"
// =====================================================
static void renderSplash(){
  const int W = TFT_WIDTH;
  const int H = TFT_HEIGHT;

  const float scale = 2.0f;      // ukuran mesh logo + teks
  const int   cx    = W / 2;
  const int   cy    = H / 2;

  const int tileH = os.blit.maxH();

  // ===== 1) Gambar splash (logo + "TRESNO") di atas background hitam =====
  os.tickStart();

  for (int ty = 0; ty < H; ty += tileH) {
    int h = tileH;
    if (ty + h > H) h = H - ty;   // strip terakhir

    int w = W;                    // NOTE: harus non-const int (karena blit minta int&)
    os.blit.blit(os.ui.tft(), os.bg, 0, ty, w, h,
      [&](GfxRGB888& g, int Rrx, int Rry, int /*Rw*/, int /*Rh*/) {
        // bg sudah hitam, tinggal gambar mesh logo + teks
        TresnoLogo::drawMesh(g, Rrx, Rry, cx, cy, scale, "TRESNO");
      }
    );
  }

  os.tickEnd();

  delay(1800);

  // ===== 2) Clear lagi ke full black sebelum main page jalan =====
  os.tickStart();

  for (int ty = 0; ty < H; ty += tileH) {
    int h = tileH;
    if (ty + h > H) h = H - ty;

    int w = W;    // lagi-lagi: non-const int, walau nilainya tetap
    os.blit.blit(os.ui.tft(), os.bg, 0, ty, w, h,
      [&](GfxRGB888& /*g*/, int /*Rrx*/, int /*Rry*/, int /*Rw*/, int /*Rh*/) {
        // No-op → yang dikirim ke TFT hanya isi bg (hitam polos)
      }
    );
  }

  os.tickEnd();
}

// =====================
// STEP6: UI State Machine (ACTIVE / DIM / SLEEP)
// =====================
enum UiMode : uint8_t {
  UI_ACTIVE = 0,
  UI_DIM,
  UI_SLEEP,
};

static UiMode   gUiMode = UI_ACTIVE;

// =====================
// STEP5 (tetap): Brightness + Auto Dim/Sleep
// =====================
static uint8_t  gUserBrightnessPct = 80;   // persist
static uint32_t gLastInteractMs    = 0;

static constexpr uint32_t UI_DIM_IDLE_MS   = 20000u;  // 20 detik idle -> dim
static constexpr uint32_t UI_SLEEP_IDLE_MS = 60000u;  // 60 detik idle -> UI sleep

static Preferences gPrefs;

// -------------------------------------
// Clamp + backlight helpers (INI YANG BENAR)
// -------------------------------------
static uint8_t clampUserPct(int v){
  if (v < 5)   return 5;
  if (v > 100) return 100;
  return (uint8_t)v;
}

// ACTIVE/DIM: tidak boleh 0
static void applyBacklightUser(uint8_t pct){
  os.ui.tft().setBacklightPercent(clampUserPct(pct));
}

// SLEEP: boleh 0
static void applyBacklightRaw(uint8_t pct){
  os.ui.tft().setBacklightPercent(pct);
}

// -------------------------------------
// NVS (tetap dipakai)
// -------------------------------------
static uint8_t loadBrightnessFromNVS(){
  gPrefs.begin("tresno", true); // read-only
  uint8_t v = gPrefs.getUChar("br_pct", 80);
  gPrefs.end();
  return clampUserPct(v);
}

static void saveBrightnessToNVS(uint8_t pct){
  pct = clampUserPct(pct);
  gPrefs.begin("tresno", false);
  gPrefs.putUChar("br_pct", pct);
  gPrefs.end();
}

static bool loadAutoSleepFromNVS(){
  gPrefs.begin("tresno", true);
  bool v = gPrefs.getBool("as_on", false);   // default OFF
  gPrefs.end();
  return v;
}

static void saveAutoSleepToNVS(bool on){
  gPrefs.begin("tresno", false);
  gPrefs.putBool("as_on", on);
  gPrefs.end();
}

// -------------------------------------
// Dim computation (tetap dipakai)
// -------------------------------------
static uint8_t computeDimPct(uint8_t userPct){
  int dim = (int)clampUserPct(userPct) / 3;
  if (dim < 5) dim = 5;
  if (dim > 20) dim = 20;
  return (uint8_t)dim;
}

// -------------------------------------
// Mode helpers
// -------------------------------------
static bool isUiSleep(){ return gUiMode == UI_SLEEP; }
static bool isUiDim()  { return gUiMode == UI_DIM; }
static bool isUiActive(){ return gUiMode == UI_ACTIVE; }

// Forward decl (sudah ada di file kamu)
static void forceWifiOffForSleep();
static void forceRedrawAfterModalClosed();

// -------------------------------------
// Transisi mode (SATU pintu)
// -------------------------------------
static void uiSetModeActive(uint32_t now){
  (void)now;
  gUiMode = UI_ACTIVE;
  applyBacklightUser(gUserBrightnessPct);
}

static void uiSetModeDim(uint32_t now){
  (void)now;
  gUiMode = UI_DIM;
  applyBacklightUser(computeDimPct(gUserBrightnessPct));
}

static void uiSetModeSleep(uint32_t now){
  (void)now;

  // Tutup modal dulu (QS + popup)
  if (qs.isOpen()) {
    qs.requestClose();
    const uint32_t t0 = millis();
    while (qs.isOpen() && (uint32_t)(millis() - t0) < 260u) {
      qs.update();
      delay(10);
      yield();
    }
  }
  if (powerPopup.isOpen()) {
    powerPopup.close();
  }

  // BLE OFF
  qsBleSet(false, nullptr);

  // WiFi OFF
  forceWifiOffForSleep();

  gUiMode = UI_SLEEP;
  applyBacklightRaw(0);   // INI kunci: sleep benar-benar mati
}

// Wrapper (biar nama tetap sama seperti code kamu)
static void enterSleep(){ uiSetModeSleep(millis()); }

static void exitSleep(){
  const uint32_t now = millis();

  uiSetModeActive(now);
  gLastInteractMs = now; // reset idle timer

  // Safety: modal harus tidak nyangkut
  if (qs.isOpen()) qs.requestClose();
  if (powerPopup.isOpen()) powerPopup.close();

  // Paksa refresh telemetry ringan
  pageSHT.backendTick(now);
  pageBMP.backendTick(now);

  forceRedrawAfterModalClosed();
}

// -------------------------------------
// Interaction / idle manager
// -------------------------------------
static void markUserInteraction(uint32_t now){
  gLastInteractMs = now;

  // Kalau sedang DIM, sentuhan/aksi user mengembalikan ACTIVE
  if (isUiDim()){
    uiSetModeActive(now);
  }
}

static bool gAutoSleepEnabled = false;
static bool gQsReady = false;

static void uiIdleManager(uint32_t now){
  if (isUiSleep()) return;

  if (!gAutoSleepEnabled){
    // autosleep OFF → jangan pernah DIM/SLEEP otomatis
    if (isUiDim()) uiSetModeActive(now);
    return;
  }

  uint32_t idle = (uint32_t)(now - gLastInteractMs);

  if (idle >= UI_SLEEP_IDLE_MS){
    uiSetModeSleep(now);
    return;
  }

  if (idle >= UI_DIM_IDLE_MS){
    if (!isUiDim()) uiSetModeDim(now);
    return;
  }

  if (!isUiActive()){
    uiSetModeActive(now);
  }
}

static void qsAutoSleepSet(bool on, void* ctx){
  (void)ctx;
  gAutoSleepEnabled = on;

  // reset idle timer
  markUserInteraction(millis());

  // kalau OFF dan sedang DIM, balik ACTIVE
  if (!gAutoSleepEnabled && isUiDim()){
    uiSetModeActive(millis());
  }
}

static bool qsAutoSleepGet(void* ctx){
  (void)ctx;
  return gAutoSleepEnabled;
}

static void qsBrightnessSet(uint8_t pct, void* ctx){
  (void)ctx;

  // clamp user brightness (5..100)
  gUserBrightnessPct = clampUserPct(pct);

  // QS kadang memanggil callback saat init dengan default 100.
  // Abaikan sampai QS benar-benar siap.
  if (!gQsReady) return;

  // apply kalau tidak sleep
  if (!isUiSleep()){
    uiSetModeActive(millis());
  }

  // simpan segera (biar survive walau power putus mendadak)
  saveBrightnessToNVS(gUserBrightnessPct);

  markUserInteraction(millis());
}

static uint8_t qsBrightnessGet(void* ctx){
  (void)ctx;
  return gUserBrightnessPct;
}

static void forceRedrawAfterModalClosed(){
  router.pumpAfterOverlayClosed();

  if      (router.inMenu())           menu.onQSClosed();
  else if (router.inSimpleChat())     simpleChat.onQSClosed();
  else if (router.inNetwork())        pageNet.onQSClosed();
  else if (router.inLogPath())        pageLogPath.onQSClosed();
  else if (router.inLogData())        pageLogData.onQSClosed();
  else if (router.inSensorMPU9250())  pageMPU.onQSClosed();
  else if (router.inSensorBMP280())   pageBMP.onQSClosed();
  else if (router.inSensorSHT30())    pageSHT.onQSClosed();
  else if (router.inGNSS_M10())       pageM10.onQSClosed();
  else if (router.inClockDS3231())    pageCLK.onQSClosed();
  else if (router.inMicroSD())        pageMSD.onQSClosed();
  else if (router.inBattery())        pageBAT.onQSClosed();
  else if (router.inWifi())           pageWifi.onQSClosed();
  else                                page.onQSClosed();
}

static void handlePowerShortPress() {
  if (!isUiSleep()) enterSleep();
  else              exitSleep();
}

static void handlePowerLongHold() {
  if (isUiSleep()) exitSleep();
  powerPopup.open();
}

static void handlePowerButtonPressedInPopup() {
  // Mainkan nada shutdown dulu (blocking ±0.6–0.7 s)
  BuzzerTone::toneShutdown();

  saveBrightnessToNVS(gUserBrightnessPct);
  saveAutoSleepToNVS(gAutoSleepEnabled);

  // Dipanggil ketika icon power di popup disentuh
  powerCtl.killPower();
}

// Always-on tick (jalan walau sleep)
static void tickAlwaysOn(uint32_t now, float dt){
  (void)dt;

  powerCtl.tick();
  handleSOSBuzzer();

  // GNSS/PDR & IMU harus selalu update (path & heading)
  pageMPU.backendTick(now);
  pageM10.backendTick(now);
  TimeSync::tick(now);

  // Network wajib selalu aktif dan tidak boleh dipengaruhi sleep
  NetBackend::tick();

  // Telemetry ringan: hanya saat UI ON.
  // Saat UI sleep, telemetry akan di-refresh lewat netTelemetryHook() sebelum payload.
  if (!isUiSleep()) {
    pageSHT.backendTick(now);
    pageBMP.backendTick(now);
    // Battery icon/display memang ditangani TopBar.tick() saat UI ON,
    // dan topBar.backendTick() saat sleep via hook kalau kamu pakai itu.
  }
}

static void netTelemetryHook(uint32_t nowMs, void* user){
  (void)user;
  if (!isUiSleep()) return;

  pageSHT.forceSample(nowMs);
  pageBMP.forceSample(nowMs);

  topBar.backendTick(nowMs);   // pastikan TopBar.backendTick() drawIcon=false
}

static void forceWifiOffForSleep(){
  // Sinkronkan state PageWifi + QS icon + stop scan/timers internal PageWifi
  pageWifi.requestSetWifi(false);

  // Safety hard-off (jaga-jaga kalau stack masih nyala)
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

static bool readTouchThrottled(int& tx, int& ty){
  static uint32_t lastMs = 0;
  const uint32_t now = millis();

  if ((uint32_t)(now - lastMs) < 20u) return false;  // 50 Hz
  lastMs = now;

  bool touching = os.readTouch1(tx, ty);
  if (touching) markUserInteraction(now);
  return touching;
}

static bool allowUiFrame(){
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - lastMs) < 33u) return false; // 30 FPS
  lastMs = now;
  return true;
}

static void pumpRouterThrottled(uint32_t now){
  static uint32_t lastMs = 0;
  // 20ms ~50Hz, atau 33ms ~30Hz (sesuaikan dengan allowUiFrame)
  if ((uint32_t)(now - lastMs) < 20u) return;
  lastMs = now;
  router.pumpAfterOverlayClosed();
}

// =====================================================

void setup(){
  Serial.begin(115200); delay(150);

  powerCtl.begin();

  gUserBrightnessPct = loadBrightnessFromNVS();
  gAutoSleepEnabled = loadAutoSleepFromNVS();
  gLastInteractMs = millis();

  os.begin();                    // init HW (ui, bg, blit, dsb)
  // paksa backlight ikut NVS (os.begin sering default 100)
  applyBacklightUser(gUserBrightnessPct);

  // ----- Buzzer: init + nada boot sekali -----
  BuzzerTone::begin();      // siapkan LEDC untuk buzzer
  BuzzerTone::toneBoot();   // mainkan pola nada boot (blocking < 1 detik)

  syncSystemClockFromRTC();

  // ==== Splash screen TRESNO ====
  renderSplash();

  // Inisialisasi popup overlay (butuh akses ke os.ui, os.bg, os.blit)
  powerPopup.begin(&os);
  powerPopup.onPowerPressed(&handlePowerButtonPressedInPopup);

  // HAPUS / KOMENTAR:
  // pinMode(PinsAndConfig::Power::PIN_BUZZER, OUTPUT);
  // digitalWrite(PinsAndConfig::Power::PIN_BUZZER, LOW);

  // GANTI DENGAN:
  BuzzerTone::stop();

  powerCtl.onShortPress(&handlePowerShortPress);

  // Register callback: hold tombol power 3 detik → buka popup overlay
  powerCtl.onLongHold(&handlePowerLongHold);

  topBar.begin(&os);             // top bar
  page.begin(&os, &topBar);      // main page
  qs.bindBrightness(nullptr, &qsBrightnessSet, &qsBrightnessGet);
  qs.bindAutoSleep(nullptr, &qsAutoSleepSet, &qsAutoSleepGet);
  qs.bindBle(nullptr, &qsBleSet, &qsBleGet);

  gQsReady = false;
  qs.begin(&os);
  gQsReady = true;

  qsBleSet(false, nullptr);   // pastikan radio BLE mati di awal
  applyBacklightUser(gUserBrightnessPct);
  powerPopup.bindQSSos(&qs.sos());
  pageWifi.linkQuickSettings(&qs);  // mengikat QS ke Page_Wifi
  menu.begin(&os, &topBar);      // menu page

  // Sub pages
  simpleChat.begin(&os, &topBar);
  pageNet.begin(&os, &topBar);
  pageLogPath.begin(&os, &topBar);
  pageLogData.begin(&os, &topBar);

  pageMPU.begin(&os, &topBar);
  pageBMP.begin(&os, &topBar);
  pageSHT.begin(&os, &topBar);
  pageM10.begin(&os, &topBar);
  pageCLK.begin(&os, &topBar);
  pageMSD.begin(&os, &topBar);
  pageBAT.begin(&os, &topBar);
  pageWifi.begin(&os, &topBar);

  RtcTime::begin();
  TimeSync::begin();

  // === Inisialisasi backend Multi-Hop Node ===
  NetBackend::begin();
  NetBackend::setTelemetryHook(&netTelemetryHook, nullptr);

  // === Set ID capsule sekali (final) dari NetBackend ===
  topBar.setDeviceId(PinsAndConfig::NetMultiHop::idHex6);

  router.begin(&os, &topBar, &qs,
               &page, &menu,
               &simpleChat, &pageNet, &pageLogPath, &pageLogData,
               &pageMPU, &pageBMP, &pageSHT, &pageM10, &pageCLK, &pageMSD, &pageBAT, &pageWifi);

  qs.attachRouter(&router);
  menu.attachRouter(&router);
  simpleChat.attachRouter(&router);
  pageNet.attachRouter(&router);
  pageLogPath.attachRouter(&router);
  pageLogData.attachRouter(&router);
  pageMPU.attachRouter(&router);
  pageBMP.attachRouter(&router);
  pageSHT.attachRouter(&router);
  pageM10.attachRouter(&router);
  pageCLK.attachRouter(&router);
  pageMSD.attachRouter(&router);
  pageBAT.attachRouter(&router);
  pageWifi.attachRouter(&router);
}

void loop(){
  const uint32_t now = millis();
  static uint32_t prev = now;
  float dt = (now - prev) * 0.001f;
  prev = now;

  // Selalu jalan (bahkan saat UI sleep)
  tickAlwaysOn(now, dt);

  uiIdleManager(now);

  if (isUiSleep()){
    delay(2);
    return;
  }

  // ===== UI ON =====
  os.tickStart();

  // Topbar throttle
  static uint32_t lastTopBarMs = 0;
  if ((uint32_t)(now - lastTopBarMs) >= 250u) {
    lastTopBarMs = now;
    topBar.tick(now, dt);
  }

  // QS
  qs.update();
  if (qs.isOpen()){
    int tx=0, ty=0;
    const bool touching = readTouchThrottled(tx,ty);
    qs.handleOutsideTapToClose(touching, tx, ty);
    os.tickEnd();
    return;
  }
  if (qs.consumeJustClosed()){
    forceRedrawAfterModalClosed();
  }

  // Power popup
  powerPopup.update(now, dt);
  if (powerPopup.isOpen()){
    int tx=0, ty=0;
    const bool touching = readTouchThrottled(tx,ty);
    powerPopup.handleTouch(touching, tx, ty);
    os.tickEnd();
    return;
  }
  if (powerPopup.consumeJustClosed()){
    forceRedrawAfterModalClosed();
  }

  pumpRouterThrottled(now);

  // Throttle page render (30 FPS)
  if (!allowUiFrame()){
    os.tickEnd();
    return;
  }

  // Render current page
  if      (router.inMenu())           menu.tick(now, dt);
  else if (router.inSimpleChat())     simpleChat.tick(now, dt);
  else if (router.inNetwork())        pageNet.tick(now, dt);
  else if (router.inLogPath())        pageLogPath.tick(now, dt);
  else if (router.inLogData())        pageLogData.tick(now, dt);
  else if (router.inSensorMPU9250())  pageMPU.tick(now, dt);
  else if (router.inSensorBMP280())   pageBMP.tick(now, dt);
  else if (router.inSensorSHT30())    pageSHT.tick(now, dt);
  else if (router.inGNSS_M10())       pageM10.tick(now, dt);
  else if (router.inClockDS3231())    pageCLK.tick(now, dt);
  else if (router.inMicroSD())        pageMSD.tick(now, dt);
  else if (router.inBattery())        pageBAT.tick(now, dt);
  else if (router.inWifi())           pageWifi.tick(now, dt);
  else                                page.tick(now, dt);

  os.tickEnd();
}