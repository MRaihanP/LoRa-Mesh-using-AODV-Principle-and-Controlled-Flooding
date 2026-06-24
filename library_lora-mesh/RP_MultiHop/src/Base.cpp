#include "Base.h"

MeshDeviceBase::MeshDeviceBase(const char* role, const BoardPins& pins)
: Lora(2), _role(role), _pins(pins)
{}

void MeshDeviceBase::begin() {
  // LoRa pins
  pinMode(_pins.lora.M0, OUTPUT);
  pinMode(_pins.lora.M1, OUTPUT);
  pinMode(_pins.lora.AUX, INPUT_PULLUP);

  digitalWrite(_pins.lora.M0, LOW);
  digitalWrite(_pins.lora.M1, LOW);
  delay(40);

  // Status LED
  pinMode(_pins.statusLed, OUTPUT);
  digitalWrite(_pins.statusLed, LOW);

  // Tidak lagi menginisialisasi RTC di sini.
  // Inisialisasi RTC dilakukan di level aplikasi / sketch (.ino).
}

void MeshDeviceBase::initLoraUart() {
  Lora.begin(9600, SERIAL_8N1, _pins.lora.RX, _pins.lora.TX);
}

void MeshDeviceBase::blinkStatus() {
  digitalWrite(_pins.statusLed, HIGH);
  delay(100);
  digitalWrite(_pins.statusLed, LOW);
}
