#pragma once
#include <Arduino.h>
#include <driver/ledc.h>

// =====================================================
//  BuzzerTone – helper tone berbasis LEDC (ESP32)
//  Target: Firmware_Relay (ESP32-S3 / ESP32 family)
//  Pin buzzer: GPIO02
//
//  Catatan akademik:
//  - LEDC memberi PWM stabil (frekuensi presisi) dibanding tone() biasa.
//  - Pola nada dibuat sangat singkat agar tidak mengganggu time-slot LoRa.
// =====================================================
namespace BuzzerTone {

  // --- Konfigurasi LEDC ---
  // Hindari TIMER_0/CHANNEL_0 jika board Anda memakai untuk periferal lain.
  static constexpr ledc_mode_t       kMode       = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t      kTimer      = LEDC_TIMER_1;
  static constexpr ledc_channel_t    kChannel    = LEDC_CHANNEL_1;
  static constexpr ledc_timer_bit_t  kResolution = LEDC_TIMER_10_BIT;
  static constexpr uint32_t          kDuty       = (1u << kResolution) / 2;

  static constexpr int kPin = 2; // GPIO02 (sesuai permintaan)

  inline void begin() {
    ledc_timer_config_t t{};
    t.speed_mode       = kMode;
    t.duty_resolution  = kResolution;
    t.timer_num        = kTimer;
    t.freq_hz          = 2000;
    t.clk_cfg          = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c{};
    c.gpio_num   = kPin;
    c.speed_mode = kMode;
    c.channel    = kChannel;
    c.intr_type  = LEDC_INTR_DISABLE;
    c.timer_sel  = kTimer;
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
  }

  inline void start(uint32_t hz) {
    if (!hz) {
      ledc_set_duty(kMode, kChannel, 0);
      ledc_update_duty(kMode, kChannel);
      return;
    }
    ledc_set_freq(kMode, kTimer, hz);
    ledc_set_duty(kMode, kChannel, kDuty);
    ledc_update_duty(kMode, kChannel);
  }

  inline void stop() {
    ledc_set_duty(kMode, kChannel, 0);
    ledc_update_duty(kMode, kChannel);
  }

  // Beep blocking singkat (hindari durasi panjang)
  inline void beep(uint32_t hz, uint32_t durMs) {
    start(hz);
    delay(durMs);
    stop();
  }

  // =====================================================
  //  Pola nada (event)
  // =====================================================

  // BOOTUP – tiga nada naik
  inline void toneBoot() {
    beep(1200, 90);  delay(35);
    beep(1600, 90);  delay(35);
    beep(2050, 140);
  }

  // SHUTDOWN – tiga nada turun
  inline void toneShutdown() {
    beep(1800, 110); delay(35);
    beep(1300, 110); delay(35);
    beep( 850, 240);
  }

  // GPS LOCKED – double beep tinggi
  inline void toneGpsLocked() {
    beep(2250, 70); delay(55);
    beep(2250, 110);
  }

  // GPS LOSS – double beep rendah
  inline void toneGpsLoss() {
    beep(900, 90);  delay(70);
    beep(900, 150);
  }

  // NET PAIRED – dua nada naik (lebih "positif")
  inline void tonePairOk() {
    beep(1700, 80); delay(35);
    beep(2150, 140);
  }

  // TX – ping sangat singkat
  inline void toneTx() {
    beep(2100, 25);
  }

  // ACK – ping sedikit lebih panjang & tinggi
  inline void toneAck() {
    beep(2600, 40);
  }

  // FORWARD RX – indikasi paket masuk untuk diteruskan
  inline void toneForwardRx() {
    beep(1500, 30); delay(25);
    beep(1500, 30);
  }
}
