#pragma once
#include <Arduino.h>
#include <PKAE_Timer.h>
#include "pins_and_config.h"

// Soft-latch power control berbasis tombol power:
// - begin() dipanggil di setup awal → menahan power (PIN_LATCH = ON).
// - tick() dipanggil tiap loop → baca tombol, kedip LED,
//   dan bila tombol ditahan cukup lama, memicu callback "long hold".
// - Short press (tekan singkat lalu lepas) bisa memicu callback "short press".
// - killPower() → lepas latch, lalu kedip cepat LED sampai power benar-benar hilang.
//
// Catatan: HOLD_OFF_MS tidak langsung memanggil killPower(),
// tetapi memanggil callback yang bisa digunakan untuk menampilkan popup.

class PowerControl {
public:
  using Callback0 = void(*)();

  void begin() {
    using P = PinsAndConfig::Power;

    // Beri sedikit delay supaya regulator/rail stabil sebelum menahan power
    delay(300);

    // Pin latch: segera ON supaya supply tetap hidup
    pinMode(P::PIN_LATCH, OUTPUT);
    digitalWrite(P::PIN_LATCH, P::LATCH_ON_LEVEL);

    // LED indikator
    pinMode(P::PIN_LED, OUTPUT);
    digitalWrite(P::PIN_LED, LOW);

    // Tombol power (pakai pull-up internal)
    pinMode(P::PIN_SWITCH, INPUT_PULLUP);

    _ledState   = LOW;
    _released   = true;
    _holdFired  = false;
    _pressMs    = 0;

    _stableLED.Reset();
    _buttonHeld.Reset();
  }

  // Dipanggil ketika tombol ditahan > HOLD_OFF_MS (sekali per press)
  void onLongHold(Callback0 cb) {
    _onLongHold = cb;
  }

  // Dipanggil ketika tombol ditekan singkat (press -> release),
  // dan TIDAK sampai memicu long-hold.
  void onShortPress(Callback0 cb) {
    _onShortPress = cb;
  }

  // Panggil sesering mungkin dari loop utama (non-blocking).
  void tick() {
    using P = PinsAndConfig::Power;

    const bool pressed = (digitalRead(P::PIN_SWITCH) == P::SW_ACTIVE_LEVEL);

    if (pressed) {
      // Transisi baru: tombol baru saja ditekan
      if (_released) {
        _released  = false;
        _holdFired = false;
        _pressMs   = millis();

        _buttonHeld.Reset();  // mulai hitung hold dari momen press
        _stableLED.Reset();   // sinkron ulang kedip LED
      }

      // Saat tombol ditahan, kedipkan LED tiap TOGGLE_BLINK_MS
      if (_stableLED.IsTimeUp()) {
        _ledState = !_ledState;
        digitalWrite(P::PIN_LED, _ledState);
      }

      // Cek apakah durasi hold sudah melewati ambang HOLD_OFF_MS
      if (!_holdFired && _buttonHeld.IsTimeUp()) {
        _holdFired = true;
        if (_onLongHold) _onLongHold();  // misalnya: buka popup shutdown
        // Jangan reset _buttonHeld di sini agar tidak retrigger berkali-kali.
      }
    } else {
      // Tombol dilepas → jika sebelumnya sedang ditekan, evaluasi short-press
      if (!_released) {
        const uint32_t now = millis();
        const uint32_t dt  = (now >= _pressMs) ? (now - _pressMs) : 0;

        // Short press hanya bila belum long-hold
        if (!_holdFired) {
          // (opsional) batasi agar bukan accidental bounce super singkat
          // Tapi kita biarkan longgar; jika mau, tambahkan dt >= 30.
          if (_onShortPress) _onShortPress();
        }

        _released = true;
      }

      _holdFired = false;
      _buttonHeld.Reset();
      _stableLED.Reset();
      _ledState = LOW;
      digitalWrite(P::PIN_LED, LOW);
    }
  }

  // Bisa kamu panggil manual kalau mau "shutdown dari software" nanti
  // (misalnya setelah user menekan tombol "Power off" di popup).
  void killPower() {
    using P = PinsAndConfig::Power;

    bool ledState = LOW;
    PKAE_Timer blinkLED(P::SHUTDOWN_BLINK_MS);

    // Lepas latch power: setelah ini board akan mati dalam beberapa ms–ratusan ms
    digitalWrite(P::PIN_LATCH, P::LATCH_OFF_LEVEL);

    // Kedip cepat LED sampai power benar-benar hilang.
    while (true) {
      if (blinkLED.IsTimeUp()) {
        ledState = !ledState;
        digitalWrite(P::PIN_LED, ledState);
      }
      delay(1);
    }
  }

private:
  bool       _ledState   = false;
  bool       _released   = true;
  bool       _holdFired  = false;
  uint32_t   _pressMs    = 0;

  Callback0  _onLongHold   = nullptr;
  Callback0  _onShortPress = nullptr;

  PKAE_Timer _stableLED{ PinsAndConfig::Power::TOGGLE_BLINK_MS };
  PKAE_Timer _buttonHeld{ PinsAndConfig::Power::HOLD_OFF_MS };
};
