#pragma once
#include <Arduino.h>
#include <PKAE_Timer.h>

// Modul soft power sederhana:
// - begin(pinPower, pinSwitch)
//   pinPower : output ke transistor latch (HIGH = ON, LOW = OFF)
//   pinSwitch: input tombol (LOW = ditekan)
// - tick() dipanggil di loop()
//   tahan tombol >3 detik → matikan power
// Tambahan:
//   - LED indikator di pin 1 (LED_BUILTIN) untuk debug:
//     * Berkedip pelan saat tombol terdeteksi ditekan
//     * Berkedip cepat saat killPower() dieksekusi

class PB {
public:
  // Panggil sekali di setup()
  static void begin(int pinPower, int pinSwitch);

  // Panggil setiap loop()
  static void tick();

private:
  static bool     _enabled;
  static uint8_t  _pinPower;
  static uint8_t  _pinSwitch;
  static uint8_t  _pinLed;     // LED indicator (pin 1)

  static bool     _ledState;
  static bool     _released;   // state tombol: true = dilepas

  // Timer:
  static PKAE_Timer _buttonHeld;   // deteksi long-press (shutdown, 3000 ms)
  static PKAE_Timer _stableLed;    // kedip pelan saat tombol ditekan (300 ms)
  static PKAE_Timer _blinkLed;     // kedip cepat saat killPower (100 ms)

  static void killPower();
};
