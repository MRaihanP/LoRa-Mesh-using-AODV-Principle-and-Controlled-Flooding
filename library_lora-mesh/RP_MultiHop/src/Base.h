#pragma once
#include <Arduino.h>
#include "Pins.h"
#include "NetTime.h"

class MeshDeviceBase {
public:
  MeshDeviceBase(const char* role, const BoardPins& pins);

  virtual void begin();
  virtual void loop() = 0;

protected:
  HardwareSerial Lora;
  const char*    _role;
  BoardPins      _pins;

  void initLoraUart();
  void blinkStatus();
};
