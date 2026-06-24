#pragma once
#include <Arduino.h>

struct BoardPins {

  struct {
    int M0, M1, AUX, RX, TX;
  } lora;

  int statusLed;

  int rtcSda;
  int rtcScl;

};
