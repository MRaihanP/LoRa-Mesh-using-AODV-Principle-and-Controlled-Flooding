#pragma once
#include <Arduino.h>
#include <driver/ledc.h>
#include "pins_and_config.h"

// =====================================================
//  BuzzerTone – simple tone helper (max ~3 detik)
//  - Menggunakan LEDC PWM di GPIO yang didefinisikan di pins_and_config
//  - Pola nada untuk:
//      * toneBoot()
//      * toneShutdown()
//      * toneGpsLocked()
//      * tonePairOk()
// =====================================================
namespace BuzzerTone {

  // --- Konfigurasi LEDC ---
  // PENTING:
  // - Jangan pakai TIMER_0 / CHANNEL_0 kalau sudah dipakai backlight TFT.
  // - Pilih kombinasi yang belum dipakai di project-mu.
  static constexpr ledc_mode_t       kMode       = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t      kTimer      = LEDC_TIMER_1;    // <-- ganti ke TIMER_1
  static constexpr ledc_channel_t    kChannel    = LEDC_CHANNEL_1;  // <-- ganti ke CHANNEL_1
  static constexpr ledc_timer_bit_t  kResolution = LEDC_TIMER_10_BIT;
  static constexpr uint32_t          kDuty       = (1u << kResolution) / 2;

  // Pastikan PIN ini TIDAK dipakai untuk TFT / backlight / tombol power, dll.
  static constexpr int kPin = PinsAndConfig::Buzzer::PIN;

  // --- Init: panggil sekali di setup() ---
  inline void begin() {
    // Timer khusus buzzer
    ledc_timer_config_t t{};
    t.speed_mode       = kMode;
    t.duty_resolution  = kResolution;
    t.timer_num        = kTimer;
    t.freq_hz          = 2000;        // default awal, nanti diubah per nada
    t.clk_cfg          = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    // Channel khusus buzzer
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

  // --- Low-level: start/stop tone ---
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

  // Helper sederhana: beep blocking
  inline void beep(uint32_t hz, uint32_t durMs) {
    start(hz);
    delay(durMs);
    stop();
  }

  // =====================================================
  //  Pola nada (maksimal jauh di bawah 3 detik)
  // =====================================================

  // 1) BOOTUP – tiga nada naik
  inline void toneBoot() {
    beep(1200, 100);
    delay(40);
    beep(1600, 100);
    delay(40);
    beep(2000, 160);
  }

  // 2) SHUTDOWN – tiga nada turun
  inline void toneShutdown() {
    beep(1800, 120);
    delay(40);
    beep(1300, 120);
    delay(40);
    beep( 800, 260);
  }

  // 3) GPS LOCKED – double beep pendek tinggi
  inline void toneGpsLocked() {
    beep(2200, 80);
    delay(60);
    beep(2200, 120);
  }

  // 4) PAIRED OK – dua nada naik
  inline void tonePairOk() {
    beep(1700, 90);
    delay(40);
    beep(2100, 150);
  }
}
