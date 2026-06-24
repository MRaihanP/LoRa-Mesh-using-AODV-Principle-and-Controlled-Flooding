// ===================== WiFi TCP Bridge =====================
#include <WiFi.h>

#define AP_SSID     "ESP32-Sink"   // nama WiFi bebas
#define AP_PASSWORD "12345678"     // min 8 karakter
#define TCP_PORT    9999

static WiFiServer tcpServer(TCP_PORT);
static WiFiClient tcpClient;

#include <Arduino.h>

#include "RP_MultiHop.h"
#include "rtc.h"
#include "pins_and_config.h"
#include <TinyGPSPlus.h>
#include <TimeLib.h>

// ===================== ID device global =====================
uint32_t gDeviceId24 = 0;
String   gDeviceIdHex6;

HardwareSerial GPSSerial(1);     // UART1 untuk GPS
HardwareSerial RS485Serial(0);   // UART0 untuk RS485

TinyGPSPlus gps;

static bool   gSinkGpsLocked = false;
static double gSinkLat = 0.0;
static double gSinkLon = 0.0;

// ===================== RGB LED (ESP32-S3 built-in RGB) =====================
// Menggunakan API bawaan ESP32 Arduino Core: rgbLedWrite(pin, r, g, b)
// (sesuai contoh sketch yang kamu berikan).
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 21
#endif

static uint32_t gLedOffAtMs = 0;

static inline void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}

static inline void ledPulse(uint8_t r, uint8_t g, uint8_t b, uint16_t durMs = 60) {
  gLedOffAtMs = millis() + durMs;
  ledSet(r, g, b);
}

static inline void ledTick() {
  if (gLedOffAtMs != 0 && (int32_t)(millis() - gLedOffAtMs) >= 0) {
    gLedOffAtMs = 0;
    ledSet(0, 0, 0);
  }
}

static void pollSinkGps() {
  while (GPSSerial.available()) {
    gps.encode((char)GPSSerial.read());
  }

  bool lockedNow = gps.location.isValid() && gps.location.isUpdated();

  if (lockedNow) {
    gSinkLat = gps.location.lat();
    gSinkLon = gps.location.lng();
    gSinkGpsLocked = true;

    NetRuntime::setSinkPosition(true, gSinkLat, gSinkLon);

    if (gps.date.isValid() && gps.time.isValid()) {
      tmElements_t tm;
      tm.Year   = CalendarYrToTm((int)gps.date.year());
      tm.Month  = (int)gps.date.month();
      tm.Day    = (int)gps.date.day();
      tm.Hour   = (int)gps.time.hour();
      tm.Minute = (int)gps.time.minute();
      tm.Second = (int)gps.time.second();

      time_t t = makeTime(tm);
      RtcTime::tickGpsSync(true, (uint32_t)t, millis());
    }
  }
}

static void rs485Begin() {
  pinMode(AppCfg::RS485_DE_RE, OUTPUT);
  digitalWrite(AppCfg::RS485_DE_RE, LOW); // mode receive

  RS485Serial.begin(
    AppCfg::RS485_BAUD,
    SERIAL_8N1,
    AppCfg::RS485_RX,
    AppCfg::RS485_TX
  );
}

static void rs485WriteLine(const char* s) {
  if (!s || !*s) return;

  digitalWrite(AppCfg::RS485_DE_RE, HIGH); // transmit
  delayMicroseconds(80);

  RS485Serial.println(s);
  RS485Serial.flush();

  delayMicroseconds(80);
  digitalWrite(AppCfg::RS485_DE_RE, LOW);  // kembali receive
}

extern "C" void sinkRawLineMirror(const char* line) {
  rs485WriteLine(line);   // tetap ada, tidak berubah

  // Cek client baru jika belum ada yang connect
  if (!tcpClient || !tcpClient.connected()) {
    tcpClient = tcpServer.available();
  }
  // Forward frame ke TCP client
  if (tcpClient && tcpClient.connected()) {
    tcpClient.println(line);
  }
}

// ===================== Hook callback untuk event RP_MultiHop =====================
// 1) TX HELLO: hijau
static void onTxHello(void*) {
  ledPulse(0, 255, 0, 50);
}

// 2) RX HELLO: biru
static void onRxHello(void*, const char* line) {
  (void)line;
  ledPulse(0, 0, 255, 50);
}

// 3) RX DATA (proto apapun, string CSV data): ungu
static void onRxData(void*, const char* line) {
  (void)line;
  ledPulse(255, 0, 255, 60);
}

// 4) Frame advance: putih singkat
static void onFrameAdvance(void*) {
  ledPulse(255, 255, 255, 25);
}

MeshSink device(AppCfg::PINS);

void setup() {
  Serial.begin(115200);

  // ESP32 sebagai Access Point
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[AP] IP: ");
  Serial.println(WiFi.softAPIP());  // selalu 192.168.4.1
  tcpServer.begin();
  Serial.printf("[TCP] Listening on port %d\n", TCP_PORT);

  delay(200);
  Serial.println();
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  Serial.println("=== START SINK (Waveshare ESP32-S3-Zero) ===");

  // LED boot pulse (MERAH singkat)
  ledPulse(255, 0, 0, 120);

  // RTC backend (DS3231)
  RtcTime::begin(AppCfg::PINS.rtcSda, AppCfg::PINS.rtcScl);
  RtcTime::setSyncPolicy(AppCfg::RTC_SYNC_THRESHOLD_SEC, AppCfg::RTC_SYNC_INTERVAL_MS);

  // GPS Neo-6M -> UART1
  GPSSerial.begin(AppCfg::GPS_BAUD, SERIAL_8N1, AppCfg::GPS_RX, AppCfg::GPS_TX);

  // RS485 -> UART0
  rs485Begin();

  // ID device
  Id::begin();
  gDeviceId24   = Id::id24();
  gDeviceIdHex6 = Id::idHex6();

  Serial.print("[BOOT] This ID = ");
  Serial.println(gDeviceIdHex6);

  // Runtime tuning (meniru Sink lama, tapi via konstanta di NetConfig/NetRuntimeConfig bila tersedia)
  // Jika kamu punya NetRuntime::applyTuning versi baru, silakan dipakai di sini.
  NetRuntime::applyConfig(AppCfg::NET_CFG);

  // Pasang hook LED sesuai event yang kamu minta
  device.setHookTxHello(onTxHello, nullptr);
  device.setHookRxHello(onRxHello, nullptr);
  device.setHookRxData(onRxData, nullptr);
  device.setHookFrameAdvance(onFrameAdvance, nullptr);

  device.begin();
}

void loop() {
  ledTick();
  pollSinkGps();
  device.loop();
  delay(1);
}