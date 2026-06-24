#pragma once
#include <Arduino.h>

class Id {
public:
  static void begin() {
    if (_initialized) return;
    _initialized = true;

    uint64_t mac = ESP.getEfuseMac();

    char macBuf[13];
    uint32_t hi = (uint32_t)(mac >> 32);
    uint32_t lo = (uint32_t)(mac & 0xFFFFFFFFu);
    snprintf(macBuf, sizeof(macBuf), "%04X%08X",
             (unsigned int)hi, (unsigned int)lo);

    uint8_t b[6];
    for (int i = 0; i < 6; ++i) {
      b[5 - i] = (mac >> (8 * i)) & 0xFF;
    }

    uint32_t h = 2166136261u;
    const uint32_t FNV_PRIME = 16777619u;
    for (int i = 0; i < 6; ++i) {
      h ^= b[i];
      h *= FNV_PRIME;
    }

    _id24 = (h & 0xFFFFFFu);

    char idBuf[7];
    snprintf(idBuf, sizeof(idBuf), "%06X", (unsigned int)_id24);
    _hex6 = String(idBuf);

    Serial.print("[Id] MAC=");
    Serial.print(macBuf);
    Serial.print(" HASH24=");
    Serial.println(_hex6);
  }

  static uint32_t id24() {
    return _id24;
  }

  static String idHex6() {
    return _hex6;
  }

private:
  static bool     _initialized;
  static uint32_t _id24;
  static String   _hex6;
};
