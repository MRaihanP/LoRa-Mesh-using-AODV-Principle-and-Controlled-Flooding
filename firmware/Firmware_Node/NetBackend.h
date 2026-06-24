#pragma once
#include <stdint.h>

class NetBackend {
public:
  using TelemetryHookFn = void (*)(uint32_t nowMs, void* user);

  static void begin();
  static void tick();

  // Hook dipanggil tepat sebelum build payload.
  static void setTelemetryHook(TelemetryHookFn fn, void* user);

private:
  // ...
};

