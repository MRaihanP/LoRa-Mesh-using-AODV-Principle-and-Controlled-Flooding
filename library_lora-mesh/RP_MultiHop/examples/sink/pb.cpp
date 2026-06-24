#include "pb.h"

// ----------------------------
// Static var definition
// ----------------------------
bool    PB::_enabled   = false;
uint8_t PB::_pinPower  = 0;
uint8_t PB::_pinSwitch = 0;
uint8_t PB::_pinLed    = 1;   // LED_BUILTIN = pin 1 (sesuai code dasar)

bool PB::_ledState = false;
bool PB::_released = true;

// 3000 ms: tahan tombol 3 detik untuk shutdown
PKAE_Timer PB::_buttonHeld(3000);
// 300 ms: kedip pelan saat tombol ditekan (indikasi PB baca tombol)
PKAE_Timer PB::_stableLed(300);
// 100 ms: kedip cepat saat killPower (indikasi power akan diputus)
PKAE_Timer PB::_blinkLed(100);

void PB::begin(int pinPower, int pinSwitch) {
  _pinPower  = (uint8_t)pinPower;
  _pinSwitch = (uint8_t)pinSwitch;
  _enabled   = true;

  // Sedikit delay untuk memastikan supply stabil (opsional)
  delay(300);

  // Set pin MY_POWER ke HIGH supaya latch NPN tetap ON
  pinMode(_pinPower, OUTPUT);
  digitalWrite(_pinPower, HIGH);

  // Tombol pakai pull-up internal (LOW = ditekan)
  pinMode(_pinSwitch, INPUT_PULLUP);

  // LED indikator di pin 1 (LED_BUILTIN sesuai contoh)
  pinMode(_pinLed, OUTPUT);
  digitalWrite(_pinLed, LOW);
  _ledState = LOW;
  _released = true;

  // Reset semua timer
  _buttonHeld.Reset();
  _stableLed.Reset();
  _blinkLed.Reset();
}

void PB::killPower() {
  // Turunkan MY_POWER, NPN off → board mati.
  digitalWrite(_pinPower, LOW);

  // Kedip cepat LED sampai power benar-benar hilang
  while (true) {
    if (_blinkLed.IsTimeUp()) {
      _ledState = !_ledState;
      digitalWrite(_pinLed, _ledState);
    }
  }
}

void PB::tick() {
  if (!_enabled) return;

  bool pressed = (digitalRead(_pinSwitch) == LOW);

  if (pressed) {
    // Tombol dianggap sedang ditekan

    // Kedip pelan sebagai indikasi "PB membaca tombol"
    if (_stableLed.IsTimeUp()) {
      _ledState = !_ledState;
      digitalWrite(_pinLed, _ledState);
    }

    // Deteksi long-press > 3 detik → shutdown
    if (_buttonHeld.IsTimeUp()) {
      killPower();
    }

    _released = false;
  } else {
    // Tombol dilepas → reset semua state terkait tombol
    if (!_released) {
      _released = true;
      _ledState = LOW;
      digitalWrite(_pinLed, LOW);
    }

    _buttonHeld.Reset();
    _stableLed.Reset();
  }
}
